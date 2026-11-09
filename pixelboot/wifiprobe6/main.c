/*
 * wifiprobe6: BCM4389 firmware + NVRAM + RNG seed rTLV download, Cortex-A7 boot,
 * pciedev_shared_t / ring_info_t inspection, and firmware console log dump
 * (WIFI-PLAN.md, Phase W2 first half).
 *
 * Sequence (matching downloads/ref/bcmdhd4389/dhd_pcie.c):
 *   1. Release PHY isolation at EL2, power on WL_REG_ON, train PCIe1 Gen1 x1 link to L0,
 *      assign BAR0 (0x60000000) and BAR2 (0x60400000), enable dm1 (WLAN/ARM) power domain,
 *      set PCIE_CFG_SUBSYSTEM_CONTROL |= 0x300 (coherent BAR access for CA7), and apply
 *      PCIe2 REG_BAR2_CONFIG (0x4e0) R/W WAR.
 *   2. Enter download state (dhdpcie_bus_download_state(bus, TRUE)):
 *      - PCI_BAR1_WIN (0x84) = 0x0
 *      - Reset ARM Cortex-A7 (wrap 0x18120000) with SICF_CPUHALT (0x20)
 *      - Reset SYSMEM (wrap 0x18126000) with 0
 *      - Clear top-of-RAM shared address slot at 0x49fffc (dongle_ram_base + ramsize - 4)
 *   3. Download firmware (downloads/wifi/fw/fw_bcmdhd.bin, 1,701,376 bytes) to SYSMEM at
 *      dongle_ram_base = 0x200000 via BAR2, saving the first 32-bit word as resetinstr.
 *   4. Format NVRAM (downloads/wifi/fw/bcmdhd.cal) with process_nvram_vars, pad and write
 *      varsize bytes to varaddr = 0x49fffc - varsize, and write varsizew to 0x49fffc.
 *   5. Write 128-byte heap ASLR random seed rTLV (DNGL_RTLV_TYPE_RNG_SIGNATURE = 0xFEEDC0DE,
 *      len = 0x80 without checksum) at varaddr - 8 - 128.
 *   6. Exit download state (dhdpcie_bus_download_state(bus, FALSE)):
 *      - Write resetinstr to backplane address 0x0 (BAR2 + 0x0)
 *      - Release ARM Cortex-A7 halt (ai_reset_core(0x18120000, 0, 0, 0))
 *   7. Poll 0x49fffc until the firmware overwrites varsizew with pciedev_shared_t address.
 *   8. Read and log pciedev_shared_t, ring_info_t, and the 5 common ring_mem_t descriptors,
 *      then dump the firmware's CONSOLE: log buffer from TCM.
 */
#include "board.h"

__asm__(
	".section .rodata\n"
	".balign 4\n"
	".global fw_bin_start, fw_bin_end\n"
	"fw_bin_start:\n"
	".incbin \"downloads/wifi/fw/fw_bcmdhd.bin\"\n"
	"fw_bin_end:\n"
	".balign 4\n"
	".global nvram_cal_start, nvram_cal_end\n"
	"nvram_cal_start:\n"
	".incbin \"downloads/wifi/fw/bcmdhd.cal\"\n"
	"nvram_cal_end:\n"
	".text\n"
);

extern const u8 fw_bin_start[], fw_bin_end[];
extern const u8 nvram_cal_start[], nvram_cal_end[];

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

/* ELBI */
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

/* BCM4389 core & memory addresses discovered in wifiprobe5 */
#define CHIPC_BASE              0x18000000u
#define PCIE2_BASE              0x18001000u
#define ARMCA7_BASE             0x18020000u
#define ARMCA7_WRAP             0x18120000u
#define SYSMEM_BASE             0x18026000u
#define SYSMEM_WRAP             0x18126000u

#define SICF_CPUHALT            0x0020u

#define DONGLE_RAM_BASE         0x00200000u
#define DONGLE_RAM_SIZE         0x002a0000u             /* 17 banks = 2,752,512 bytes */
#define DONGLE_RAM_TOP          (DONGLE_RAM_BASE + DONGLE_RAM_SIZE)
#define SHARED_ADDR_SLOT        (DONGLE_RAM_TOP - 4u)   /* 0x0049fffc */

#define DNGL_RTLV_RNG_SIG       0xfeedc0deu
#define RNG_HOST_NBYTES         128u

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
	puts_("wifiprobe6: ");
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

/* Read/write any 32-bit backplane address via BAR0 window (config 0x80) */
static u32 cur_bar0_win = 0xffffffffu;

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

/* Read/write any 32-bit TCM (SYSMEM) address via BAR2 window (PCI_BAR1_WIN = config 0x84) */
static u32 cur_bar2_win = 0xffffffffu;

static void
tcm_set_win(u32 win)
{
	if (win != cur_bar2_win) {
		*reg(PCIE1_CFG + 0x84) = win;
		(void)*reg(PCIE1_CFG + 0x84);
		cur_bar2_win = win;
	}
}

static u32
tcm_read32(u32 addr)
{
	tcm_set_win(addr & ~0x3fffffu);
	return *reg(PCIE1_BAR2_ADDR + (addr & 0x3fffffu));
}

static void
tcm_write32(u32 addr, u32 val)
{
	tcm_set_win(addr & ~0x3fffffu);
	*reg(PCIE1_BAR2_ADDR + (addr & 0x3fffffu)) = val;
}

static u8
tcm_read8(u32 addr)
{
	u32 w = tcm_read32(addr & ~3u);
	return (u8)((w >> (8 * (addr & 3u))) & 0xffu);
}

static void
ai_reset_core(u32 wrap_base, u32 pre, u32 rst, u32 post)
{
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

/*
 * Format NVRAM text file into NUL-separated key=value strings, matching
 * process_nvram_vars() in downloads/ref/bcmdhd4389/bcmutils.c + dhdpcie_download_nvram().
 */
static u8 nvram_buf[32768] __attribute__((aligned(4)));

static u32
format_nvram(const u8 *src, u32 src_len)
{
	u32 n, dp = 0, column = 0;
	int find_nl = 0;

	if (src_len > 0)
		src_len--; /* matching bufp[len-1] = 0 in dhdpcie_download_nvram */

	for (n = 0; n < src_len && dp < sizeof(nvram_buf) - 8; n++) {
		u8 c = src[n];
		if (c == '\r')
			continue;
		if (find_nl && c != '\n')
			continue;
		find_nl = 0;
		if (c == '#') {
			find_nl = 1;
			continue;
		}
		if (c == '\n') {
			if (column == 0)
				continue;
			nvram_buf[dp++] = 0;
			column = 0;
			continue;
		}
		nvram_buf[dp++] = c;
		column++;
	}

	/* Pad to 4-byte multiple, then append trailing NUL and round up to 4 bytes */
	while (dp & 3u)
		nvram_buf[dp++] = 0;
	nvram_buf[dp++] = 0;
	while (dp & 3u)
		nvram_buf[dp++] = 0;
	return dp;
}

static void
dump_fw_console(u32 console_addr)
{
	/*
	 * hnd_cons_t (downloads/ref/bcmdhd4389/include/hnd_cons.h):
	 *   +0x00: vcons_in
	 *   +0x04: vcons_out
	 *   +0x08: log.buf       (TCM address of circular char buffer)
	 *   +0x0c: log.buf_size  (size in bytes)
	 *   +0x10: log.idx       (next write index)
	 *   +0x14: log.out_idx
	 */
	u32 log_buf = tcm_read32(console_addr + 0x08);
	u32 log_size = tcm_read32(console_addr + 0x0c);
	u32 log_idx = tcm_read32(console_addr + 0x10);

	kv("wifiprobe6: console log.buf", log_buf);
	kv("wifiprobe6: console log.buf_size", log_size);
	kv("wifiprobe6: console log.idx", log_idx);

	if (log_buf < DONGLE_RAM_BASE || log_buf >= DONGLE_RAM_TOP ||
	    log_size == 0 || log_size > 0x40000u) {
		puts_("wifiprobe6: console log buffer out of range\n");
		return;
	}

	u32 n = log_idx < log_size ? log_idx : log_size;
	puts_("wifiprobe6: --- FIRMWARE CONSOLE (");
	hex(n, 4);
	puts_(" bytes) ---\n");
	for (u32 i = 0; i < n; i++) {
		u8 c = tcm_read8(log_buf + i);
		if (c == '\n' || (c >= 0x20 && c < 0x7f) || c == '\t')
			put((char)c);
		if ((i & 0x3ff) == 0)
			wdt_pet();
	}
	puts_("\nwifiprobe6: --- END FIRMWARE CONSOLE ---\n");
}

void
at_entry(u64 el, u64 dtb)
{
	u64 x0;

	(void)dtb;
	log_open();
	puts_("\nwifiprobe6: BCM4389 firmware + NVRAM download, CA7 boot, shared struct + console\n");
	kv("wifiprobe6: at entry, EL", el);
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
	u64 i, t0;
	int linked = 0;

	(void)el;
	(void)entry_el;
	(void)dtb;
	wdt_pet();

	kv("wifiprobe6: pmu 0x3ec4 (PHY isolation bypass)", *reg(PMU_ISOL));
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

	/* RC setup */
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

	/* LTSSM enable, poll for L0 */
	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 1;
	t0 = now_us();
	last = 0xffffffff;
	for (i = 0; ; i++) {
		v = *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP);
		s = v & 0x3f;
		if (v != last)
			last = v;
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
	kv(linked ? "wifiprobe6: LINK UP (L0) after us" : "wifiprobe6: NO L0", now_us() - t0);
	wdt_pet();

	if (linked) {
		mdelay(3);
		iatu_outbound(0, 0x4, PCIE1_CFG, 0x1000, 1u << 24);
		iatu_outbound(1, 0x0, PCIE1_MEM, 0x800000, PCIE1_MEM);

		v = *reg(PCIE1_CFG + 0x0);
		kv("wifiprobe6: bus 1 vendor/device", v);

		/* Assign BAR0 = 0x60000000, BAR2 = 0x60400000, enable MEM + MASTER */
		*reg(PCIE1_CFG + 0x10) = (u32)PCIE1_BAR0_ADDR;
		*reg(PCIE1_CFG + 0x14) = 0;
		*reg(PCIE1_CFG + 0x18) = (u32)PCIE1_BAR2_ADDR;
		*reg(PCIE1_CFG + 0x1c) = 0;
		*reg(PCIE1_CFG + 0x04) = (*reg(PCIE1_CFG + 0x04) & ~0x7u) | 0x6u;
		udelay(10);

		/* Point BAR0 window at ChipCommon 0x18000000 */
		bp_read32(CHIPC_BASE);
		kv("wifiprobe6: BAR0+0x00 ChipCommon ID", *reg(PCIE1_BAR0_ADDR + 0x00));

		/* Enable dm1 (WLAN/ARM) power domain + coherent BAR access for CA7 + clear PME */
		*reg(PCIE1_BAR0_ADDR + 0x21e8) |= (3u << 8);
		bp_write32(0x180001e8u, bp_read32(0x180001e8u) | (3u << 8));
		*reg(PCIE1_BAR0_ADDR + 0x2a0c) |= (2u << 8);
		poll("pcie2 powerctl DMN1 status (bit 17)", PCIE1_BAR0_ADDR + 0x21e8, 2u << 16, 2u << 16, 20000);
		*reg(PCIE1_CFG + 0x88) |= 0x300u;
		*reg(PCIE1_CFG + 0x4c) |= 0x8000u;

		/* PCIe2 REG_BAR2_CONFIG (0x4e0) R/W WAR */
		bp_write32(PCIE2_BASE + 0x120, 0x4e0u);
		u32 bar2cfg = bp_read32(PCIE2_BASE + 0x124);
		bp_write32(PCIE2_BASE + 0x124, bar2cfg);
		kv("wifiprobe6: pcie2 REG_BAR2_CONFIG (0x4e0)", bar2cfg);
		wdt_pet();

		/*
		 * Step 2: Enter download state (dhdpcie_bus_download_state(bus, TRUE))
		 *   - Set PCI_BAR1_WIN (0x84) = 0x0
		 *   - Put ARM Cortex-A7 into reset with SICF_CPUHALT (0x20)
		 *   - Reset SYSMEM with 0
		 *   - Clear top-of-RAM shared address slot (0x49fffc)
		 */
		puts_("wifiprobe6: entering download state (halt CA7, reset SYSMEM)\n");
		tcm_set_win(0x00000000u);
		ai_reset_core(ARMCA7_WRAP, SICF_CPUHALT, SICF_CPUHALT, SICF_CPUHALT);
		ai_reset_core(SYSMEM_WRAP, 0, 0, 0);
		kv("wifiprobe6: CA7 ioctrl (expect 0x21)", bp_read32(ARMCA7_WRAP + 0x408));
		kv("wifiprobe6: CA7 resetctrl (expect 0x0)", bp_read32(ARMCA7_WRAP + 0x800));
		kv("wifiprobe6: SYSMEM ioctrl (expect 0x1)", bp_read32(SYSMEM_WRAP + 0x408));
		kv("wifiprobe6: SYSMEM resetctrl (expect 0x0)", bp_read32(SYSMEM_WRAP + 0x800));

		/* Verify SYSMEM bank count with extended mask 0x1f0 (17 banks = 0x2a0000) */
		u32 coreinfo = bp_read32(SYSMEM_BASE + 0x00);
		u32 nb = (coreinfo & 0x1f0u) >> 4;
		u32 memsize = 0;
		for (u32 idx = 0; idx < nb; idx++) {
			bp_write32(SYSMEM_BASE + 0x10, idx);
			u32 bankinfo = bp_read32(SYSMEM_BASE + 0x40);
			memsize += ((bankinfo & 0x7fu) + 1u) * 8192u;
		}
		kv("wifiprobe6: SYSMEM num banks (mask 0x1f0)", nb);
		kv("wifiprobe6: SYSMEM total RAM size", memsize);

		tcm_write32(SHARED_ADDR_SLOT, 0);
		kv("wifiprobe6: cleared 0x49fffc", tcm_read32(SHARED_ADDR_SLOT));
		wdt_pet();

		/*
		 * Step 3: Download firmware (fw_bcmdhd.bin) to DONGLE_RAM_BASE (0x200000)
		 */
		u32 fw_len = (u32)(fw_bin_end - fw_bin_start);
		const u32 *fw_words = (const u32 *)fw_bin_start;
		u32 resetinstr = fw_words[0];
		kv("wifiprobe6: fw_bcmdhd.bin size (bytes)", fw_len);
		kv("wifiprobe6: fw resetinstr (word 0)", resetinstr);

		t0 = now_us();
		tcm_set_win(0x00000000u);
		volatile u32 *dst = (volatile u32 *)(PCIE1_BAR2_ADDR + DONGLE_RAM_BASE);
		u32 nwords = fw_len / 4;
		for (u32 w = 0; w < nwords; w++) {
			dst[w] = fw_words[w];
			if ((w & 0xffffu) == 0)
				wdt_pet();
		}
		kv("wifiprobe6: fw download time (us)", now_us() - t0);
		kv("wifiprobe6: fw verify first word at 0x200000", tcm_read32(DONGLE_RAM_BASE));
		kv("wifiprobe6: fw verify last word", tcm_read32(DONGLE_RAM_BASE + (nwords - 1) * 4));
		wdt_pet();

		/*
		 * Step 4: Format NVRAM (bcmdhd.cal) and write to top of RAM (0x49fffc - varsize),
		 *         then write varsizew to 0x49fffc.
		 */
		u32 raw_nv_len = (u32)(nvram_cal_end - nvram_cal_start);
		u32 varsize = format_nvram(nvram_cal_start, raw_nv_len);
		u32 varaddr = SHARED_ADDR_SLOT - varsize;
		u32 varsizew = ((~(varsize / 4u) << 16) & 0xffff0000u) | ((varsize / 4u) & 0xffffu);

		kv("wifiprobe6: raw bcmdhd.cal size", raw_nv_len);
		kv("wifiprobe6: formatted NVRAM varsize", varsize);
		kv("wifiprobe6: NVRAM varaddr", varaddr);
		kv("wifiprobe6: NVRAM varsizew (nvram_csm)", varsizew);

		const u32 *nv_words = (const u32 *)nvram_buf;
		for (u32 off = 0; off < varsize; off += 4)
			tcm_write32(varaddr + off, nv_words[off / 4]);
		tcm_write32(SHARED_ADDR_SLOT, varsizew);
		kv("wifiprobe6: 0x49fffc readback after NVRAM write", tcm_read32(SHARED_ADDR_SLOT));
		wdt_pet();

		/*
		 * Step 5: Write 128-byte heap ASLR random seed rTLV at varaddr - 8 - 128
		 *         (dhdpcie_wrt_rnd / dhdpcie_download_rtlv in dhd_pcie.c)
		 */
		u32 rtlv_addr = varaddr - 8u - RNG_HOST_NBYTES;
		u32 prng = (u32)now_us() ^ 0xa5a55a5au;
		for (u32 off = 0; off < RNG_HOST_NBYTES; off += 4) {
			/* xorshift32 non-zero entropy */
			prng ^= prng << 13;
			prng ^= prng >> 17;
			prng ^= prng << 5;
			if ((prng & 0xffu) == 0) prng |= 0x01u;
			if ((prng & 0xff00u) == 0) prng |= 0x0100u;
			if ((prng & 0xff0000u) == 0) prng |= 0x010000u;
			if ((prng & 0xff000000u) == 0) prng |= 0x01000000u;
			tcm_write32(rtlv_addr + off, prng);
		}
		tcm_write32(rtlv_addr + RNG_HOST_NBYTES, RNG_HOST_NBYTES);      /* len = 0x80, no csum */
		tcm_write32(rtlv_addr + RNG_HOST_NBYTES + 4u, DNGL_RTLV_RNG_SIG); /* 0xFEEDC0DE */
		kv("wifiprobe6: RNG rTLV addr", rtlv_addr);
		kv("wifiprobe6: RNG rTLV len word", tcm_read32(rtlv_addr + RNG_HOST_NBYTES));
		kv("wifiprobe6: RNG rTLV sig word", tcm_read32(rtlv_addr + RNG_HOST_NBYTES + 4u));
		wdt_pet();

		/*
		 * Step 6: Exit download state & boot Cortex-A7 (dhdpcie_bus_download_state(bus, FALSE))
		 *   - Write resetinstr to backplane address 0x0 (BAR2 + 0x0)
		 *   - Release ARMCA7 halt: ai_reset_core(ARMCA7_WRAP, 0, 0, 0)
		 */
		kv("wifiprobe6: TCM 0x0 before resetinstr write", tcm_read32(0x00000000u));
		tcm_write32(0x00000000u, resetinstr);
		kv("wifiprobe6: TCM 0x0 after resetinstr write", tcm_read32(0x00000000u));

		puts_("wifiprobe6: releasing Cortex-A7 reset/halt!\n");
		t0 = now_us();
		ai_reset_core(ARMCA7_WRAP, 0, 0, 0);
		kv("wifiprobe6: CA7 ioctrl after release", bp_read32(ARMCA7_WRAP + 0x408));
		kv("wifiprobe6: CA7 resetctrl after release", bp_read32(ARMCA7_WRAP + 0x800));

		/*
		 * Step 7: Poll 0x49fffc until firmware overwrites varsizew with pciedev_shared_t addr
		 */
		u32 shared_addr = varsizew;
		for (i = 0; i < 2000000; i++) {
			shared_addr = tcm_read32(SHARED_ADDR_SLOT);
			if (shared_addr != varsizew && shared_addr != 0 &&
			    shared_addr != 0xffffffffu)
				break;
			udelay(1);
			if ((i & 0x3fff) == 0)
				wdt_pet();
		}
		u64 boot_us = now_us() - t0;
		kv("wifiprobe6: 0x49fffc poll elapsed (us)", boot_us);
		kv("wifiprobe6: 0x49fffc (shared_addr)", shared_addr);
		wdt_pet();

		/*
		 * Step 8: Read pciedev_shared_t, ring_info_t, ring_mem_t, and firmware console
		 */
		if (shared_addr >= DONGLE_RAM_BASE && shared_addr < SHARED_ADDR_SLOT) {
			/* Let firmware finish printing its banner to the console buffer */
			mdelay(50);
			wdt_pet();

			u32 flags          = tcm_read32(shared_addr + 0x00);
			u32 trap_addr      = tcm_read32(shared_addr + 0x04);
			u32 assert_exp     = tcm_read32(shared_addr + 0x08);
			u32 assert_file    = tcm_read32(shared_addr + 0x0c);
			u32 assert_line    = tcm_read32(shared_addr + 0x10);
			u32 console_addr   = tcm_read32(shared_addr + 0x14);
			u32 msgtrace_addr  = tcm_read32(shared_addr + 0x18);
			u32 fwid           = tcm_read32(shared_addr + 0x1c);
			u32 lfrag_rxbufs   = tcm_read32(shared_addr + 0x20);
			u32 dma_rxoffset   = tcm_read32(shared_addr + 0x24);
			u32 h2d_mb_ptr     = tcm_read32(shared_addr + 0x28);
			u32 d2h_mb_ptr     = tcm_read32(shared_addr + 0x2c);
			u32 rings_info_ptr = tcm_read32(shared_addr + 0x30);
			u32 dma_scratch_len = tcm_read32(shared_addr + 0x34);
			u32 flags2         = tcm_read32(shared_addr + 0x50);

			puts_("wifiprobe6: === pciedev_shared_t ===\n");
			kv("wifiprobe6:   flags (low8 = ver)", flags);
			kv("wifiprobe6:   trap_addr", trap_addr);
			kv("wifiprobe6:   assert_exp_addr", assert_exp);
			kv("wifiprobe6:   assert_file_addr", assert_file);
			kv("wifiprobe6:   assert_line", assert_line);
			kv("wifiprobe6:   console_addr", console_addr);
			kv("wifiprobe6:   msgtrace_addr", msgtrace_addr);
			kv("wifiprobe6:   fwid", fwid);
			kv("wifiprobe6:   total_lfrag / max_host_rxbufs", lfrag_rxbufs);
			kv("wifiprobe6:   dma_rxoffset", dma_rxoffset);
			kv("wifiprobe6:   h2d_mb_data_ptr", h2d_mb_ptr);
			kv("wifiprobe6:   d2h_mb_data_ptr", d2h_mb_ptr);
			kv("wifiprobe6:   rings_info_ptr", rings_info_ptr);
			kv("wifiprobe6:   host_dma_scratch_buffer_len", dma_scratch_len);
			kv("wifiprobe6:   flags2", flags2);
			wdt_pet();

			if (rings_info_ptr >= DONGLE_RAM_BASE && rings_info_ptr < SHARED_ADDR_SLOT) {
				u32 ringmem_ptr   = tcm_read32(rings_info_ptr + 0x00);
				u32 h2d_w_idx_ptr = tcm_read32(rings_info_ptr + 0x04);
				u32 h2d_r_idx_ptr = tcm_read32(rings_info_ptr + 0x08);
				u32 d2h_w_idx_ptr = tcm_read32(rings_info_ptr + 0x0c);
				u32 d2h_r_idx_ptr = tcm_read32(rings_info_ptr + 0x10);
				u32 max_rings0    = tcm_read32(rings_info_ptr + 0x38);
				u32 max_rings1    = tcm_read32(rings_info_ptr + 0x3c);

				puts_("wifiprobe6: === ring_info_t ===\n");
				kv("wifiprobe6:   ringmem_ptr", ringmem_ptr);
				kv("wifiprobe6:   h2d_w_idx_ptr", h2d_w_idx_ptr);
				kv("wifiprobe6:   h2d_r_idx_ptr", h2d_r_idx_ptr);
				kv("wifiprobe6:   d2h_w_idx_ptr", d2h_w_idx_ptr);
				kv("wifiprobe6:   d2h_r_idx_ptr", d2h_r_idx_ptr);
				kv("wifiprobe6:   max_sub_queues / max_tx_flowrings", max_rings0);
				kv("wifiprobe6:   max_vdevs / max_cmpl_rings", max_rings1);

				if (ringmem_ptr >= DONGLE_RAM_BASE && ringmem_ptr < SHARED_ADDR_SLOT) {
					for (u32 r = 0; r < 5; r++) {
						u32 rm = ringmem_ptr + r * 16u;
						puts_("  ring_mem[");
						hex(r, 1);
						puts_("] idx_type_sz=0x");
						hex(tcm_read32(rm + 0x00), 8);
						puts_(" max_items_len=0x");
						hex(tcm_read32(rm + 0x04), 8);
						puts_(" base_addr=0x");
						hex(tcm_read32(rm + 0x0c), 8);
						hex(tcm_read32(rm + 0x08), 8);
						put('\n');
					}
				}
			}

			if (console_addr >= DONGLE_RAM_BASE && console_addr < SHARED_ADDR_SLOT)
				dump_fw_console(console_addr);
		} else {
			puts_("wifiprobe6: firmware did not publish valid shared_addr\n");
		}

		kv("wifiprobe6: elbi IRQ2 after run (bit 24 = CTO)", *reg(PCIE1_ELBI + 0x8));
	}

	/* leave the chip off and in reset */
	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 0;
	pin_set(PIN_PERST, 0);
	pin_set(PIN_WL_REG_ON, 0);
	kv("wifiprobe6: total us", now_us() - t_start);
	puts_("wifiprobe6: done; letting the watchdog reset the phone\n");
	hang();
}
