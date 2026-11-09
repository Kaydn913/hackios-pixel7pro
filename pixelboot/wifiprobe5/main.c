/*
 * wifiprobe5: BAR0/BAR2 sizing + assignment, outbound MEM iATU, ChipCommon (0x18000000) and full
 * AI EROM core enumeration on the BCM4389 (WIFI-PLAN.md, W1 -> W2).
 *
 * Keeps the Gen1 x1 link setup from wifiprobe4 as-is, then:
 *   1. Sizes BAR0 (0x10/0x14) and BAR2 (0x18/0x1c) on bus 1 dev 0.
 *   2. Programs iATU outbound region 1 (DBI + 0x300200) for MEM 0x60000000..0x607fffff -> 0x60000000,
 *      sets root port MEMORY_BASE/LIMIT (DBI 0x20 = 0x60706000), assigns BAR0 = 0x60000000 and
 *      BAR2 = 0x60400000, and enables MEM + MASTER on bus 1 command (0x04 = 0x6).
 *   3. Points BAR0's backplane window (PCI config 0x80) at 0x18000000 (SI_ENUM_BASE_DEFAULT),
 *      reads ChipCommon ID at BAR0 + 0x0 and EROM pointer at BAR0 + 0xfc.
 *   4. Walks the entire Broadcom AI (AMBA Interconnect) EROM table to list every core's ID,
 *      revision, register base, and DMP wrapper (agent) address, and checks BAR2 access.
 */
#include "board.h"

#define PMU_ISOL        (0x18060000ul + 0x3ec4)
#define CMU_TOP         0x1e080000ul
#define CMU_HSI2        0x14400000ul
#define HSI2_GPIO       0x14440000ul
#define PCIE1_ELBI      0x14520000ul
#define PCIE1_PCS       0x14540000ul
#define PCIE1_PHY       0x14550000ul
#define PCIE1_DBI       0x14800000ul
#define PCIE1_ATU       (PCIE1_DBI + 0x300000ul)        /* iATU unrolled: region N at +0x200*N */
#define PCIE1_MEM       0x60000000ul                    /* DT ranges: 0x60000000..0x60feffff */
#define PCIE1_BAR0_ADDR 0x60000000ul
#define PCIE1_BAR2_ADDR 0x60400000ul
#define PCIE1_CFG       0x60ffe000ul                    /* DT "config" window */

#define GPIO_CON        0x0
#define GPIO_DAT        0x4
#define GPIO_PUD        0x8
#define PIN_PERST       0
#define PIN_CLKREQ      1
#define PIN_WL_REG_ON   4

/* ELBI */
#define ELBI_APP_LTSSM_ENABLE   0x054
#define ELBI_DEVICE_TYPE        0x080
#define ELBI_RDLH_LINKUP        0x2c8
#define ELBI_PM_DSTATE          0x2e8
#define ELBI_LINKDOWN_RST_CTRL  0x3a0
#define ELBI_SOFT_RESET         0x3a4
#define  SOFT_PWR_RESET         (1u << 1)
#define  SOFT_NON_STICKY_RESET  (1u << 3)
#define ELBI_QCH_SEL            0x3a8
#define ELBI_APP_REQ_EXIT_L1    0x3bc
#define ELBI_MSTR_PEND_SEL_NAK  0x474
#define ELBI_DBI_L1_EXIT_DIS    0x1078
#define ELBI_PHY_PORT_RST       0x1400
#define ELBI_PHY_CMN_RST        0x1404
#define ELBI_PHY_INIT_RST       0x1408

#define S_L0                    0x11

#define GATE_ON(v)      (((v) | (1u << 20) | (1u << 21)) & ~(1u << 28))

static void
udelay(u64 us)
{
	u64 freq, start, now;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(start));
	do {
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
	} while (now - start < us * freq / 1000000);
}

static void
mdelay(u64 ms)
{
	while (ms--) {
		udelay(1000);
		if ((ms & 31) == 0)
			wdt_pet();
	}
}

static u64
now_us(void)
{
	u64 freq, t;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t / (freq / 1000000);
}

static int
poll(const char *what, u64 addr, u32 mask, u32 want, u64 max_us)
{
	u32 v = 0;
	u64 i;

	for (i = 0; i < max_us; i++) {
		v = *reg(addr);
		if ((v & mask) == want)
			break;
		udelay(1);
		if ((i & 0x3fff) == 0)
			wdt_pet();
	}
	puts_("wifiprobe5: ");
	puts_(what);
	puts_(i < max_us ? ": ok after " : ": TIMEOUT after ");
	hex(i, 6);
	puts_(" us, value 0x");
	hex(v, 8);
	put('\n');
	return i < max_us;
}

static void
pin_field(u64 off, unsigned pin, u32 val)
{
	u32 v = *reg(HSI2_GPIO + off);

	v &= ~(0xfu << (4 * pin));
	v |= (val & 0xf) << (4 * pin);
	*reg(HSI2_GPIO + off) = v;
}

static void
pin_set(unsigned pin, int high)
{
	u32 v = *reg(HSI2_GPIO + GPIO_DAT);

	if (high)
		v |= 1u << pin;
	else
		v &= ~(1u << pin);
	*reg(HSI2_GPIO + GPIO_DAT) = v;
}

struct regval {
	u32 off, val;
};

static const struct regval phy_common[] = {
	{ 0x018, 0x50 }, { 0x048, 0x33 }, { 0x068, 0x01 }, { 0x070, 0x12 }, { 0x08c, 0x00 },
	{ 0x090, 0x21 }, { 0x0b0, 0x14 }, { 0x0b8, 0x50 }, { 0x0e0, 0x51 }, { 0x100, 0x00 },
	{ 0x104, 0x80 }, { 0x140, 0x38 }, { 0x180, 0xa4 }, { 0x188, 0x03 }, { 0x2a8, 0x38 },
	{ 0x2e4, 0x12 },
	{ 0x400, 0x80 }, { 0x408, 0x20 }, { 0x550, 0x00 }, { 0x5a8, 0x00 }, { 0x5ec, 0xff },
	{ 0x458, 0x02 }, { 0x5b0, 0x34 }, { 0x450, 0x20 },
	{ 0x0ac, 0x34 }, { 0x0b4, 0x34 }, { 0x0e0, 0x50 }, { 0x0f4, 0x00 }, { 0x0f8, 0x08 },
	{ 0x104, 0xa0 }, { 0x11c, 0x19 }, { 0x124, 0x17 }, { 0x220, 0x41 },
};

static const struct regval phy_lane[] = {
	{ 0x82c, 0x08 }, { 0x830, 0x24 }, { 0x878, 0x80 }, { 0x894, 0x40 }, { 0x8c0, 0x00 },
	{ 0x8f4, 0x30 }, { 0x908, 0x05 }, { 0x90c, 0xe0 }, { 0x914, 0xd4 }, { 0x91c, 0xd3 },
	{ 0x920, 0xce }, { 0x924, 0x01 }, { 0x928, 0x35 }, { 0x92c, 0xba }, { 0x930, 0x41 },
	{ 0x934, 0x15 }, { 0x938, 0x13 }, { 0x93c, 0x4e }, { 0x948, 0x43 }, { 0x94c, 0xfc },
	{ 0x954, 0x10 }, { 0x958, 0x69 }, { 0x964, 0x40 }, { 0x9b4, 0xf6 }, { 0x9c0, 0x2d },
	{ 0x9c4, 0xb7 }, { 0x9cc, 0x3c }, { 0x9dc, 0x7e }, { 0xa40, 0x02 }, { 0xa70, 0x26 },
	{ 0xa74, 0x00 }, { 0xb40, 0x06 }, { 0xb44, 0x06 }, { 0xb48, 0x04 }, { 0xb4c, 0x03 },
	{ 0xb50, 0x03 }, { 0xb54, 0x03 }, { 0xb58, 0x03 }, { 0xb5c, 0x03 }, { 0xc10, 0x1b },
	{ 0xc44, 0x10 }, { 0xc48, 0x10 }, { 0xc4c, 0x10 }, { 0xc50, 0x10 }, { 0xc54, 0x02 },
	{ 0xc58, 0x02 }, { 0xc5c, 0x02 }, { 0xc60, 0x02 }, { 0xc6c, 0x02 }, { 0xc70, 0x02 },
	{ 0xca8, 0xe7 }, { 0xcac, 0x00 }, { 0xcb0, 0x0e }, { 0xccc, 0x1c }, { 0xcd4, 0x05 },
	{ 0xcd8, 0x77 }, { 0xcdc, 0x7a }, { 0xdb4, 0x2f },
	{ 0x8fc, 0x80 }, { 0x914, 0xf4 }, { 0x91c, 0xd3 }, { 0x920, 0xca }, { 0x928, 0x3d },
	{ 0x92c, 0xb8 }, { 0x930, 0x41 }, { 0x934, 0x17 }, { 0x93c, 0x4c }, { 0x948, 0x73 },
	{ 0x94c, 0xfc }, { 0x96c, 0x55 }, { 0x988, 0x78 }, { 0x994, 0x3b }, { 0x9b4, 0xf6 },
	{ 0x9c4, 0xff }, { 0x9c8, 0x20 }, { 0xa08, 0x2f }, { 0xb9c, 0x3f },
	{ 0x9cc, 0x00 }, { 0xcd8, 0xff }, { 0xcdc, 0x6e },
	{ 0x82c, 0x0f }, { 0x830, 0x60 }, { 0x834, 0x7e },
	{ 0xc08, 0x00 }, { 0xc10, 0x09 }, { 0xc40, 0x04 }, { 0xc70, 0x00 },
	{ 0x9b4, 0x76 },
};

static const struct regval phy_clk[] = {
	{ 0xbcc, 0x41 }, { 0xbd4, 0x41 }, { 0xbdc, 0x68 }, { 0xbe0, 0x00 }, { 0xbe4, 0xd0 },
};

#define N(a)    (sizeof(a) / sizeof((a)[0]))

static void
write_table(u64 base, const struct regval *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		*reg(base + t[i].off) = t[i].val;
}

static void
phy_all_pwrdn_clear(void)
{
	static const u32 offs[] = {
		0x404, 0x408, 0x40c,
		0x800, 0x804, 0xa40, 0xa44, 0xa48, 0xa4c, 0xa50,
		0x1000, 0x1004, 0x1240, 0x1244, 0x1248, 0x124c, 0x1250,
	};

	*reg(PCIE1_PHY + 0xd8) = 0x28;
	mdelay(1);
	for (unsigned i = 0; i < N(offs); i++)
		*reg(PCIE1_PHY + offs[i]) = 0;
	*reg(PCIE1_PCS + 0x184) = 0;
}

static void
phy_config(void)
{
	u32 v;

	*reg(PCIE1_PHY + 0xd8) = 0x28;
	*reg(PCIE1_PCS + 0x008) |= 1u << 7;

	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 0;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 0;
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 0;
	udelay(10);
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 1;
	udelay(10);

	write_table(PCIE1_PHY, phy_common, N(phy_common));
	write_table(PCIE1_PHY, phy_lane, N(phy_lane));
	write_table(PCIE1_PHY, phy_clk, N(phy_clk));
	write_table(PCIE1_PHY + 0x800, phy_clk, N(phy_clk));
	wdt_pet();

	*reg(PCIE1_PCS + 0x004) = 0;
	*reg(PCIE1_PCS + 0x154) = 0x700d5;
	*reg(PCIE1_PCS + 0x150) = 0x300ff;
	*reg(PCIE1_PCS + 0x170) = 0x40;
	v = *reg(PCIE1_PCS + 0x008);
	v |= (1u << 4) | (1u << 5);
	*reg(PCIE1_PCS + 0x008) = v;
	*reg(PCIE1_PCS + 0x190) = 0x100b0808;

	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 1;

	v = *reg(PCIE1_PHY + 0x5d0);
	v |= 1u << 4;
	v &= ~(1u << 3);
	*reg(PCIE1_PHY + 0x5d0) = v;
	*reg(PCIE1_PHY + 0xbf4) = 4;
}

static void
iatu_outbound(unsigned idx, u32 type, u64 cpu_base, u32 size, u64 pci_target)
{
	u64 r = PCIE1_ATU + (u64)idx * 0x200;

	*reg(r + 0x08) = (u32)cpu_base;
	*reg(r + 0x0c) = (u32)(cpu_base >> 32);
	*reg(r + 0x10) = (u32)(cpu_base + size - 1);
	*reg(r + 0x14) = (u32)pci_target;
	*reg(r + 0x18) = (u32)(pci_target >> 32);
	*reg(r + 0x00) = type;
	*reg(r + 0x04) = 1u << 31;
	udelay(10);
}

static u64
size_bar64(u32 off)
{
	u32 orig_lo = *reg(PCIE1_CFG + off);
	u32 orig_hi = *reg(PCIE1_CFG + off + 4);
	u32 mask_lo, mask_hi;

	*reg(PCIE1_CFG + off) = 0xffffffffu;
	*reg(PCIE1_CFG + off + 4) = 0xffffffffu;
	mask_lo = *reg(PCIE1_CFG + off) & ~0xfu;
	mask_hi = *reg(PCIE1_CFG + off + 4);
	*reg(PCIE1_CFG + off) = orig_lo;
	*reg(PCIE1_CFG + off + 4) = orig_hi;
	if (!mask_lo && !mask_hi)
		return 0;
	return ~(((u64)mask_hi << 32) | mask_lo) + 1;
}

/* Read/write any 32-bit backplane address via BAR0 window (config 0x80) */
static u32 cur_bar0_win;

static u32
bp_read32(u32 addr)
{
	u32 win = addr & ~0xfffu;

	if (win != cur_bar0_win) {
		*reg(PCIE1_CFG + 0x80) = win;
		(void)*reg(PCIE1_CFG + 0x80);
		cur_bar0_win = win;
	}
	return *reg(PCIE1_BAR0_ADDR + (addr & 0xfffu));
}

static void
bp_write32(u32 addr, u32 val)
{
	u32 win = addr & ~0xfffu;

	if (win != cur_bar0_win) {
		*reg(PCIE1_CFG + 0x80) = win;
		(void)*reg(PCIE1_CFG + 0x80);
		cur_bar0_win = win;
	}
	*reg(PCIE1_BAR0_ADDR + (addr & 0xfffu)) = val;
}

static void
ai_reset_core(u32 wrap_base, u32 pre, u32 rst, u32 post)
{
	/* Ensure reset is asserted first */
	if ((bp_read32(wrap_base + 0x800) & 1u) == 0) {
		bp_write32(wrap_base + 0x408, pre | 3u); /* FGC | CLK */
		(void)bp_read32(wrap_base + 0x408);
		bp_write32(wrap_base + 0x800, 1u);
		udelay(20);
	}
	bp_write32(wrap_base + 0x408, rst | 3u); /* FGC | CLK */
	(void)bp_read32(wrap_base + 0x408);
	udelay(10);
	bp_write32(wrap_base + 0x800, 0u);
	for (unsigned k = 0; k < 100 && (bp_read32(wrap_base + 0x800) & 1u); k++)
		udelay(1);
	bp_write32(wrap_base + 0x408, post | 1u); /* CLK */
	(void)bp_read32(wrap_base + 0x408);
	udelay(10);
}

static u32 armca7_base, armca7_wrap;
static u32 sysmem_base, sysmem_wrap;
static u32 pcie2_base, pcie2_wrap;

/* DMP EROM helpers matching brcmf_chip_dmp_erom_scan (downloads/ref/brcmfmac/chip.c) */
static u32
dmp_get_desc(u32 *eromaddr, u32 *type)
{
	u32 val = bp_read32(*eromaddr);
	*eromaddr += 4;
	if (!type)
		return val;
	*type = val & 0xfu;
	if ((*type & ~0x8u) == 0x5u)
		*type = 0x5u;
	return val;
}

static int
dmp_get_regaddr(u32 *eromaddr, u32 *regbase, u32 *wrapbase)
{
	u32 desc = 0, val, szdesc;
	u32 stype, sztype, wraptype;

	*regbase = 0;
	*wrapbase = 0;

	val = dmp_get_desc(eromaddr, &desc);
	if (desc == 0x3u) {                     /* DMP_DESC_MASTER_PORT */
		wraptype = 3u;                  /* DMP_SLAVE_TYPE_MWRAP */
	} else if (desc == 0x5u) {              /* DMP_DESC_ADDRESS */
		*eromaddr -= 4;
		wraptype = 2u;                  /* DMP_SLAVE_TYPE_SWRAP */
	} else {
		*eromaddr -= 4;
		return -1;
	}

	for (unsigned iter = 0; iter < 64; iter++) {
		do {
			val = dmp_get_desc(eromaddr, &desc);
			if (desc == 0xfu) {     /* DMP_DESC_EOT */
				*eromaddr -= 4;
				return -1;
			}
		} while (desc != 0x5u && desc != 0x1u);

		if (desc == 0x1u) {             /* crossed into next DMP_DESC_COMPONENT */
			*eromaddr -= 4;
			return 0;
		}

		if (val & 0x8u)                 /* DMP_DESC_ADDRSIZE_GT32 */
			(void)dmp_get_desc(eromaddr, 0);

		sztype = (val & 0x30u) >> 4;
		if (sztype == 3u) {             /* DMP_SLAVE_SIZE_DESC */
			szdesc = dmp_get_desc(eromaddr, 0);
			if (szdesc & 0x8u)
				(void)dmp_get_desc(eromaddr, 0);
		}

		/* look only for 4K (0) or 8K (1) register regions */
		if (sztype != 0u && sztype != 1u)
			continue;

		stype = (val & 0xc0u) >> 6;
		if (*regbase == 0 && stype == 0u)
			*regbase = val & ~0xfffu;
		if (*wrapbase == 0 && stype == wraptype)
			*wrapbase = val & ~0xfffu;
		if (*regbase && *wrapbase)
			break;
	}
	return 0;
}

/* Walk the Broadcom AI EROM table and log each core (ID, rev, base0, wrapper0) */
static void
walk_erom(u32 erom_base)
{
	u32 eromaddr = erom_base;
	u32 desc_type = 0;
	unsigned cores = 0;

	puts_("wifiprobe5: walking AI EROM table at 0x");
	hex(erom_base, 8);
	put('\n');
	for (unsigned steps = 0; steps < 512 && desc_type != 0xfu; steps++) {
		u32 val = dmp_get_desc(&eromaddr, &desc_type);
		if (!(val & 1u) || desc_type == 0u || desc_type != 1u)
			continue;

		u32 id = (val >> 8) & 0xfffu;
		val = dmp_get_desc(&eromaddr, &desc_type);
		if ((val & 0xfu) != 1u) {
			puts_("wifiprobe5: EROM unexpected second comp desc\n");
			break;
		}
		u32 nmw = (val >> 14) & 0x1fu;
		u32 nsw = (val >> 19) & 0x1fu;
		u32 rev = (val >> 24) & 0xffu;

		/* need core with ports (or PMU 0x827 / GCI 0x840) */
		if (nmw + nsw == 0 && id != 0x827u && id != 0x840u)
			continue;

		u32 core_base = 0, wrap_base = 0;
		if (dmp_get_regaddr(&eromaddr, &core_base, &wrap_base) != 0)
			continue;

		if (id == 0x847u && !armca7_base) {
			armca7_base = core_base;
			armca7_wrap = wrap_base;
		} else if (id == 0x849u && !sysmem_base) {
			sysmem_base = core_base;
			sysmem_wrap = wrap_base;
		} else if (id == 0x83cu && !pcie2_base) {
			pcie2_base = core_base;
			pcie2_wrap = wrap_base;
		}

		puts_("  core[");
		hex(cores++, 2);
		puts_("] id=0x");
		hex(id, 3);
		puts_(" rev=0x");
		hex(rev, 2);
		puts_(" base=0x");
		hex(core_base, 8);
		puts_(" wrap=0x");
		hex(wrap_base, 8);
		if (wrap_base) {
			puts_(" ioctrl=0x");
			hex(bp_read32(wrap_base + 0x408), 8);
			puts_(" resetctrl=0x");
			hex(bp_read32(wrap_base + 0x800), 8);
		}
		put('\n');
		wdt_pet();
	}
}

void
at_entry(u64 el, u64 dtb)
{
	u64 x0;

	(void)dtb;
	log_open();
	puts_("\nwifiprobe5: BAR0/BAR2 sizing + assignment, ChipCommon + AI EROM walk\n");
	kv("wifiprobe5: at entry, EL", el);
	if (el != 2)
		return;
	x0 = psci_call(0x82000504, PMU_ISOL, 0, 0);
	if ((x0 & ~1ul) == 0)
		psci_call(0x82000504, PMU_ISOL, 1, x0 | 1);
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	const u64 t_start = now_us();
	u32 v, s, last;
	u64 i, t0, bar0_sz, bar2_sz;
	int linked = 0;

	(void)el;
	(void)entry_el;
	(void)dtb;
	wdt_pet();

	kv("wifiprobe5: pmu 0x3ec4 (PHY isolation bypass)", *reg(PMU_ISOL));
	*reg(CMU_TOP + 0x20c8) = GATE_ON(*reg(CMU_TOP + 0x20c8));
	*reg(CMU_HSI2 + 0x620) |= 1u << 4;
	for (unsigned k = 0; k < 1000 && (*reg(CMU_HSI2 + 0x620) & (1u << 16)); k++)
		udelay(1);
	for (u32 off = 0x2008; off <= 0x20e4; off += 4)
		if ((off >= 0x2008 && off <= 0x200c) || (off >= 0x2018 && off <= 0x2024) ||
		    (off >= 0x2050 && off <= 0x2074) || (off >= 0x207c && off <= 0x2080) ||
		    (off >= 0x2094 && off <= 0x20a0) || (off >= 0x20c8 && off <= 0x20e4))
			*reg(CMU_HSI2 + off) = GATE_ON(*reg(CMU_HSI2 + off));
	udelay(100);

	/* PERST# low, CLKREQ# fn 2 pull-up, WL_REG_ON high + 200 ms */
	pin_field(GPIO_PUD, PIN_PERST, 0);
	pin_set(PIN_PERST, 0);
	pin_field(GPIO_CON, PIN_PERST, 1);
	pin_set(PIN_PERST, 0);
	pin_field(GPIO_PUD, PIN_CLKREQ, 3);
	pin_field(GPIO_CON, PIN_CLKREQ, 2);
	pin_field(GPIO_PUD, PIN_WL_REG_ON, 0);
	pin_set(PIN_WL_REG_ON, 1);
	pin_field(GPIO_CON, PIN_WL_REG_ON, 1);
	pin_set(PIN_WL_REG_ON, 1);
	mdelay(200);
	wdt_pet();

	phy_all_pwrdn_clear();
	phy_config();
	wdt_pet();

	poll("PLL lock (phy 0x3f0 bit 3)", PCIE1_PHY + 0x3f0, 1u << 3, 1u << 3, 10000);
	poll("CDR lock (phy 0xe0c bit 2)", PCIE1_PHY + 0xe0c, 1u << 2, 1u << 2, 10000);
	poll("OC done (phy 0xde8 = 0xf)", PCIE1_PHY + 0xde8, 0xf, 0xf, 10000);
	wdt_pet();

	/* soft power reset, device type RC, non-sticky reset, EQ off */
	v = *reg(PCIE1_ELBI + ELBI_SOFT_RESET);
	v &= ~SOFT_PWR_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	mdelay(1);
	v |= SOFT_PWR_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	*reg(PCIE1_ELBI + ELBI_DEVICE_TYPE) = 4;
	v = *reg(PCIE1_ELBI + ELBI_SOFT_RESET);
	v |= SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	udelay(10);
	v &= ~SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	mdelay(1);
	v |= SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	*reg(PCIE1_DBI + 0x890) = 0x12000;

	/* PERST# high + 20 ms */
	pin_set(PIN_PERST, 1);
	mdelay(20);

	/* L1 exit, linkdown reset manual, Q-channel, NAK, DBI L1 exit */
	v = *reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1);
	v |= 1u | (1u << 4);
	*reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1) = v;
	*reg(PCIE1_ELBI + ELBI_LINKDOWN_RST_CTRL) = 1u << 1;
	v = *reg(PCIE1_ELBI + ELBI_QCH_SEL);
	v &= ~((0xfu << 8) | (0xfu << 4) | 0xfu);
	*reg(PCIE1_ELBI + ELBI_QCH_SEL) = v;
	*reg(PCIE1_ELBI + ELBI_MSTR_PEND_SEL_NAK) = 1;
	*reg(PCIE1_ELBI + ELBI_DBI_L1_EXIT_DIS) = 1;

	/* RC setup: same as wifiprobe4 + memory base/limit 0x60000000..0x607fffff */
	*reg(PCIE1_DBI + 0x8bc) = 1;                            /* DBI_RO_WR_EN */
	*reg(PCIE1_DBI + 0x00) = 0xeced144du;                   /* Samsung Exynos RC ch1 */
	v = *reg(PCIE1_DBI + 0x710);
	v = (v & ~(0x3fu << 16)) | (0x1u << 16);
	*reg(PCIE1_DBI + 0x710) = v;
	v = *reg(PCIE1_DBI + 0x80c);
	v = (v & ~(0x1fu << 8)) | (0x1u << 8);
	*reg(PCIE1_DBI + 0x80c) = v;
	v = *reg(PCIE1_DBI + 0x70 + 0x0c);
	v &= ~((0x7u << 15) | 0xfu);
	v |= (0x7u << 15) | 0x3u;
	*reg(PCIE1_DBI + 0x70 + 0x0c) = v;
	*reg(PCIE1_DBI + 0x40 + 0x04) = 0;
	v = *reg(PCIE1_DBI + 0x70 + 0x30);
	v = (v & ~0xfu) | 0x2u;
	*reg(PCIE1_DBI + 0x70 + 0x30) = v;
	*reg(PCIE1_DBI + 0x18) = 0x00ff0100;                    /* buses 0, 1, ff */
	*reg(PCIE1_DBI + 0x20) = 0x60706000;                    /* mem base 0x60000000, limit 0x607fffff */
	*reg(PCIE1_DBI + 0x04) |= 0x6;                          /* MEM + MASTER */

	/* LTSSM enable, poll for L0 */
	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 1;
	t0 = now_us();
	last = 0xffffffff;
	for (i = 0; ; i++) {
		v = *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP);
		s = v & 0x3f;
		if (v != last) {
			puts_("wifiprobe5: ltssm +");
			hex(now_us() - t0, 6);
			puts_(" us: 0x2c8 = 0x");
			hex(v, 8);
			puts_(", state 0x");
			hex(s, 2);
			put('\n');
			last = v;
		}
		if (s == S_L0) {
			linked = 1;
			break;
		}
		if (now_us() - t0 > 500000)
			break;
		udelay(2);
		if ((i & 0xfff) == 0)
			wdt_pet();
	}
	kv(linked ? "wifiprobe5: LINK UP (L0) after us" : "wifiprobe5: NO L0", now_us() - t0);
	wdt_pet();

	if (linked) {
		mdelay(3);
		v = *reg(PCIE1_DBI + 0x80);
		kv("wifiprobe5: dbi link ctrl/status (0x80)", v);

		/* iATU outbound 0: CFG0 to bus 1 dev 0 fn 0 at 0x60ffe000 */
		iatu_outbound(0, 0x4, PCIE1_CFG, 0x1000, 1u << 24);
		/* iATU outbound 1: MEM 0x60000000..0x607fffff -> 0x60000000 (8 MB covers BAR0 + BAR2) */
		iatu_outbound(1, 0x0, PCIE1_MEM, 0x800000, PCIE1_MEM);

		v = *reg(PCIE1_CFG + 0x0);
		kv("wifiprobe5: bus 1 vendor/device", v);

		/* Size BAR0 (0x10) and BAR2 (0x18) */
		bar0_sz = size_bar64(0x10);
		bar2_sz = size_bar64(0x18);
		kv("wifiprobe5: BAR0 size", bar0_sz);
		kv("wifiprobe5: BAR2 size", bar2_sz);

		/* Assign BAR0 = 0x60000000, BAR2 = 0x60400000, and enable MEM + MASTER on bus 1 */
		*reg(PCIE1_CFG + 0x10) = (u32)PCIE1_BAR0_ADDR;
		*reg(PCIE1_CFG + 0x14) = 0;
		*reg(PCIE1_CFG + 0x18) = (u32)PCIE1_BAR2_ADDR;
		*reg(PCIE1_CFG + 0x1c) = 0;
		*reg(PCIE1_CFG + 0x04) = (*reg(PCIE1_CFG + 0x04) & ~0x7u) | 0x6u;
		udelay(10);
		dump("bus 1 config space after BAR assignment", PCIE1_CFG, 0x90);
		wdt_pet();

		/* Point BAR0 window (config 0x80, BRCMF_PCIE_BAR0_WINDOW) at ChipCommon 0x18000000 */
		*reg(PCIE1_CFG + 0x80) = 0x18000000u;
		cur_bar0_win = 0x18000000u;
		udelay(10);
		kv("wifiprobe5: bus 1 config 0x80 (BAR0_WINDOW)", *reg(PCIE1_CFG + 0x80));

		v = *reg(PCIE1_BAR0_ADDR + 0x00);
		kv("wifiprobe5: BAR0+0x00 ChipCommon ID (low16 = 0x4389)", v);
		kv("wifiprobe5:   chip id", v & 0xffff);
		kv("wifiprobe5:   chip rev", (v >> 16) & 0xf);
		kv("wifiprobe5:   package", (v >> 20) & 0xf);
		kv("wifiprobe5:   num cores", (v >> 24) & 0xf);
		kv("wifiprobe5:   chip type (1=AXI)", (v >> 28) & 0xf);
		kv("wifiprobe5: BAR0+0x04 ChipCommon capabilities", *reg(PCIE1_BAR0_ADDR + 0x04));
		u32 erom = *reg(PCIE1_BAR0_ADDR + 0xfc);
		kv("wifiprobe5: BAR0+0xfc ChipCommon EROM pointer", erom);
		dump("BAR0 ChipCommon first 0x40 bytes", PCIE1_BAR0_ADDR, 0x40);
		wdt_pet();

		/*
		 * Request WL/ARM power domain (dm1, SRPWR_DMN1_ARMBPSD_MASK = 1<<1) via
		 * PCIe2 powerctl (BAR0 + 0x2000 + 0x1e8) and ChipCommon powerctl (0x180001e8),
		 * plus DAR_PCIE_PWR_CTRL (BAR0 + 0x2000 + 0xa0c), before reading dm1 wrappers/cores.
		 */
		kv("wifiprobe5: pcie2 powerctl (BAR0+0x21e8) before", *reg(PCIE1_BAR0_ADDR + 0x21e8));
		kv("wifiprobe5: chipc powerctl (0x180001e8) before", bp_read32(0x180001e8u));
		*reg(PCIE1_BAR0_ADDR + 0x21e8) |= (3u << 8);   /* DMN0_PCIE | DMN1_ARMBPSD */
		bp_write32(0x180001e8u, bp_read32(0x180001e8u) | (3u << 8));
		*reg(PCIE1_BAR0_ADDR + 0x2a0c) |= (2u << 8);   /* DAR_PCIE_PWR_CTRL DMN1 */
		poll("pcie2 powerctl DMN1 status (bit 17)", PCIE1_BAR0_ADDR + 0x21e8, 2u << 16, 2u << 16, 20000);
		kv("wifiprobe5: pcie2 powerctl (BAR0+0x21e8) after", *reg(PCIE1_BAR0_ADDR + 0x21e8));
		kv("wifiprobe5: chipc powerctl (0x180001e8) after", bp_read32(0x180001e8u));

		/* Enable coherent BAR1/BAR2 access for CA7 (PCIE_CFG_SUBSYSTEM_CONTROL = 0x88, bits 8:9) */
		*reg(PCIE1_CFG + 0x88) |= 0x300u;
		kv("wifiprobe5: bus 1 config 0x88 (SUBSYSTEM_CONTROL)", *reg(PCIE1_CFG + 0x88));

		if (erom && erom != 0xffffffffu) {
			walk_erom(erom);
			bp_read32(0x18000000u);
		}
		wdt_pet();

		/* Apply BAR1 window sizing WAR on PCIe2 core (configaddr=0x120 <- 0x4e0, R/W configdata=0x124) */
		if (pcie2_base) {
			bp_write32(pcie2_base + 0x120, 0x4e0u);
			u32 bar2cfg = bp_read32(pcie2_base + 0x124);
			bp_write32(pcie2_base + 0x124, bar2cfg);
			kv("wifiprobe5: pcie2 reg 0x4e0 (REG_BAR2_CONFIG)", bar2cfg);
		}

		/* Query SYSMEM (0x849) RAM size */
		if (sysmem_base && sysmem_wrap) {
			u32 ioctl = bp_read32(sysmem_wrap + 0x408);
			u32 rst = bp_read32(sysmem_wrap + 0x800);
			if (((ioctl & 3u) != 1u) || (rst & 1u)) {
				puts_("wifiprobe5: resetting SYSMEM core\n");
				ai_reset_core(sysmem_wrap, 0, 0, 0);
			}
			u32 coreinfo = bp_read32(sysmem_base + 0x00);
			u32 nb = (coreinfo & 0xf0u) >> 4;
			u32 memsize = 0;
			for (u32 idx = 0; idx < nb; idx++) {
				bp_write32(sysmem_base + 0x10, idx);
				u32 bankinfo = bp_read32(sysmem_base + 0x40);
				u32 bsz = ((bankinfo & 0x7fu) + 1) * 8192u;
				memsize += bsz;
			}
			kv("wifiprobe5: SYSMEM coreinfo", coreinfo);
			kv("wifiprobe5: SYSMEM num banks", nb);
			kv("wifiprobe5: SYSMEM total RAM size (bytes)", memsize);
		}
		wdt_pet();

		/* Test BAR2 (TCM / SYSMEM window) across PCI_BAR1_WIN (0x84) = 0x0 and 0x400000 */
		*reg(PCIE1_CFG + 0x84) = 0x00000000u;
		(void)*reg(PCIE1_CFG + 0x84);
		kv("wifiprobe5: PCI_BAR1_WIN (0x84) = 0x0", *reg(PCIE1_CFG + 0x84));
		kv("wifiprobe5: BAR2+0x000000 read (addr 0)", *reg(PCIE1_BAR2_ADDR + 0x000000));
		kv("wifiprobe5: BAR2+0x200000 read (RAM base)", *reg(PCIE1_BAR2_ADDR + 0x200000));
		*reg(PCIE1_BAR2_ADDR + 0x200000) = 0x4861636bu; /* 'Hack' */
		kv("wifiprobe5: BAR2+0x200000 write/readback", *reg(PCIE1_BAR2_ADDR + 0x200000));

		/* Switch PCI_BAR1_WIN (0x84) to 0x00400000 to reach top of RAM (0x49fffc) */
		*reg(PCIE1_CFG + 0x84) = 0x00400000u;
		(void)*reg(PCIE1_CFG + 0x84);
		kv("wifiprobe5: PCI_BAR1_WIN (0x84) = 0x400000", *reg(PCIE1_CFG + 0x84));
		*reg(PCIE1_BAR2_ADDR + 0x09fffc) = 0x694f5331u; /* 'iOS1' */
		kv("wifiprobe5: BAR2+0x09fffc (0x49fffc) write/readback", *reg(PCIE1_BAR2_ADDR + 0x09fffc));
		*reg(PCIE1_CFG + 0x84) = 0x00000000u;
		(void)*reg(PCIE1_CFG + 0x84);

		kv("wifiprobe5: elbi IRQ2 after MEM reads (bit 24 = CTO)", *reg(PCIE1_ELBI + 0x8));
	}

	/* leave the chip off and in reset */
	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 0;
	pin_set(PIN_PERST, 0);
	pin_set(PIN_WL_REG_ON, 0);
	kv("wifiprobe5: total us", now_us() - t_start);
	puts_("wifiprobe5: done; letting the watchdog reset the phone\n");
	hang();
}
