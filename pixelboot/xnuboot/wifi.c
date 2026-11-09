/*
 * The Wi-Fi chip's PCIe link, brought up before XNU (WIFI-PLAN.md W3a): the BCM4389 on PCIe1 of
 * gs201, powered (WL_REG_ON), its link trained to L0 and its BARs assigned, so that the kernel's
 * driver (bsd/dev/redwood_bcm4389.c) only has to map the windows and talk to the chip. The same
 * steps as the probes that found them (pixelboot/wifiprobe..wifiprobe13): the PMU's PCIe isolation
 * through the secure monitor, at EL2 (wifi_isolation, from at_entry); HSI2's clocks, the pins
 * (PERST#, CLKREQ#, WL_REG_ON), the PHY and PCS tables of Google's pcie-exynos-gs201-rc-cal.c, the
 * controller's soft resets and DBI setup, the LTSSM to L0; then the iATU's outbound windows (CFG0
 * for bus 1 at 0x60ffe000, memory at 0x60000000) and the chip's BAR0 (0x60000000) and BAR2
 * (0x60400000), memory and bus master on. Built only with WIFI_PCIE (XDEFINES=WIFI_PCIE=1 in
 * tools/ios-run.sh); wifi_pcie_up() returns 1 with the link up, and xnuboot tells the kernel so
 * with the boot-arg bcmlink=1 (the kernel must not touch PCIe space otherwise: it would fault).
 */
#include "board.h"

#ifdef WIFI_PCIE

#define PMU_ISOL        (0x18060000ul + 0x3ec4)
#define CMU_TOP         0x1e080000ul
#define CMU_HSI2        0x14400000ul
#define HSI2_GPIO       0x14440000ul
#define PCIE1_ELBI      0x14520000ul
#define PCIE1_PCS       0x14540000ul
#define PCIE1_PHY       0x14550000ul
#define PCIE1_DBI       0x14800000ul
#define PCIE1_ATU       (PCIE1_DBI + 0x300000ul)
#define PCIE1_MEM       0x60000000ul
#define PCIE1_BAR0_ADDR 0x60000000ul
#define PCIE1_BAR2_ADDR 0x60400000ul
#define PCIE1_CFG       0x60ffe000ul

#define GPIO_CON        0x0
#define GPIO_DAT        0x4
#define GPIO_PUD        0x8
#define PIN_PERST       0
#define PIN_CLKREQ      1
#define PIN_WL_REG_ON   4

#define ELBI_APP_LTSSM_ENABLE   0x054
#define ELBI_DEVICE_TYPE        0x080
#define ELBI_RDLH_LINKUP        0x2c8
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
#define N(a)            (sizeof(a) / sizeof((a)[0]))

static u64
now_us(void)
{
	u64 freq, t;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t / (freq / 1000000);
}

static void
udelay(u64 us)
{
	u64 t0 = now_us();

	while (now_us() - t0 < us)
		;
}

static void
mdelay(u64 ms)
{
	while (ms--) {
		udelay(1000);
		if ((ms & 0x3f) == 0)
			wdt_pet();
	}
}

static int
poll(const char *what, u64 addr, u32 mask, u32 want, u64 max_us)
{
	u32 v = 0;
	u64 t0 = now_us();

	while (now_us() - t0 < max_us) {
		v = *reg(addr);
		if ((v & mask) == want) {
			puts_("xnuboot: wifi: ");
			puts_(what);
			puts_(" ok\n");
			return 1;
		}
	}
	puts_("xnuboot: wifi: ");
	puts_(what);
	puts_(" TIMEOUT, value ");
	hex(v, 8);
	put('\n');
	return 0;
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

/* pcie-exynos-gs201-rc-cal.c, num_lanes 1 */
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

/* at EL2, from at_entry: the PCIe PHY's isolation off, through the secure monitor (an Exynos SMC) */
void
wifi_isolation(u64 el)
{
	u64 x0;

	if (el != 2)
		return;
	x0 = psci_call(0x82000504, PMU_ISOL, 0, 0);
	if ((x0 & ~1ul) == 0)
		psci_call(0x82000504, PMU_ISOL, 1, x0 | 1);
}

/* 1: the link is up and the chip's BARs are assigned */
int
wifi_pcie_up(void)
{
	u64 t0;
	u32 v, id;
	int linked = 0;

	kv("xnuboot: wifi: pmu isolation", *reg(PMU_ISOL));
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

	/* PERST# low (an output), CLKREQ# its function, WL_REG_ON high: the chip powers up (~200 ms) */
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

	phy_all_pwrdn_clear();
	phy_config();
	if (!poll("PLL lock", PCIE1_PHY + 0x3f0, 1u << 3, 1u << 3, 10000) ||
	    !poll("CDR lock", PCIE1_PHY + 0xe0c, 1u << 2, 1u << 2, 10000) ||
	    !poll("OC done", PCIE1_PHY + 0xde8, 0xf, 0xf, 10000))
		return 0;

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

	pin_set(PIN_PERST, 1);
	mdelay(20);

	v = *reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1);
	v |= 1u | (1u << 4);
	*reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1) = v;
	*reg(PCIE1_ELBI + ELBI_LINKDOWN_RST_CTRL) = 1u << 1;
	v = *reg(PCIE1_ELBI + ELBI_QCH_SEL);
	v &= ~((0xfu << 8) | (0xfu << 4) | 0xfu);
	*reg(PCIE1_ELBI + ELBI_QCH_SEL) = v;
	*reg(PCIE1_ELBI + ELBI_MSTR_PEND_SEL_NAK) = 1;
	*reg(PCIE1_ELBI + ELBI_DBI_L1_EXIT_DIS) = 1;

	/* the root port: DBI writable, its ids, one lane, gen 2 target, bus numbers 0/1/ff, a memory window */
	*reg(PCIE1_DBI + 0x8bc) = 1;
	*reg(PCIE1_DBI + 0x00) = 0xeced144du;
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
	*reg(PCIE1_DBI + 0x18) = 0x00ff0100;
	*reg(PCIE1_DBI + 0x20) = 0x60706000;
	*reg(PCIE1_DBI + 0x04) |= 0x6;

	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 1;
	t0 = now_us();
	while (now_us() - t0 < 500000) {
		if ((*reg(PCIE1_ELBI + ELBI_RDLH_LINKUP) & 0x3f) == S_L0) {
			linked = 1;
			break;
		}
		udelay(2);
	}
	kv(linked ? "xnuboot: wifi: link up (L0) after us" : "xnuboot: wifi: NO L0 after us", now_us() - t0);
	wdt_pet();
	if (!linked)
		return 0;

	mdelay(3);
	iatu_outbound(0, 0x4, PCIE1_CFG, 0x1000, 1u << 24);    /* CFG0, bus 1 */
	iatu_outbound(1, 0x0, PCIE1_MEM, 0x800000, PCIE1_MEM);  /* memory, 1:1 */
	id = *reg(PCIE1_CFG + 0x00);
	kv("xnuboot: wifi: vendor/device", id);
	if (id != 0x444114e4u)
		return 0;
	*reg(PCIE1_CFG + 0x10) = (u32)PCIE1_BAR0_ADDR;
	*reg(PCIE1_CFG + 0x14) = 0;
	*reg(PCIE1_CFG + 0x18) = (u32)PCIE1_BAR2_ADDR;
	*reg(PCIE1_CFG + 0x1c) = 0;
	*reg(PCIE1_CFG + 0x04) = (*reg(PCIE1_CFG + 0x04) & ~0x7u) | 0x6u;     /* memory, bus master */
	udelay(10);
	return 1;
}

#else

typedef int wifi_pcie_not_built;        /* (an empty file is not C) */

#endif
