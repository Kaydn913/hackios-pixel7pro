/*
 * wifiprobe4: the Wi-Fi PCIe link, LTSSM to L0, then the BCM4389's config space (WIFI-PLAN.md, W1)
 *
 * wifiprobe3 (3d): with PCIE_PHY_BYPASS set (PMU 0x18063ec4 bit 0, by SMC_CMD_PRIV_REG from EL2 in
 * at_entry -- from EL1 no SMC ever returns) the controller, PHY, PCS, ELBI and DBI all answer. This
 * probe follows Google's exynos_pcie_rc_poweron / exynos_pcie_rc_establish_link
 * (downloads/ref/pcie-exynos-rc.c l. 3072-3420) and exynos_pcie_rc_pcie_phy_config
 * (pcie-exynos-gs201-rc-cal.c l. 154-392) with the dtbo's settings for this phone: 1 lane, Gen2,
 * ep-device-type 1 (BCM Wi-Fi). Left out: the IA sequencer (an L1.2-exit CDR workaround, armed by
 * the link-up interrupt), the udbg debug-dump setup, SysMMU, interrupts, L1SS.
 *
 * Pins (HSI2 pin controller 0x14440000, bank gph2 at offset 0: CON +0x0 4 bits a pin, DAT +0x4,
 * PUD +0x8 4 bits a pin): PERST# gph2[0] output (pinctrl 0x16c), CLKREQ# gph2[1] function 2 pull-up
 * (pinctrl 0x16b), WL_REG_ON gph2[4] output (cfg_wlanen). PERST# is driven low first, as Google's
 * probe does (devm_gpio_request_one at driver probe, long before bcmdhd raises WL_REG_ON).
 * The PHY's supplies (S2MPG13 LDO3S, LDO18S) are regulator-always-on in the dtbo.
 */
#include "board.h"

#define PMU_ISOL        (0x18060000ul + 0x3ec4)
#define CMU_TOP         0x1e080000ul
#define CMU_HSI2        0x14400000ul
#define HSI2_SYSREG     0x14420000ul
#define HSI2_GPIO       0x14440000ul
#define PCIE1_ELBI      0x14520000ul
#define PCIE1_PCS       0x14540000ul
#define PCIE1_PHY       0x14550000ul
#define PCIE1_DBI       0x14800000ul
#define PCIE1_ATU       (PCIE1_DBI + 0x300000ul)        /* iATU, unrolled: outbound region 0 */
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
#define ELBI_HISTORY_CHECK      0xc00
#define ELBI_DBI_L1_EXIT_DIS    0x1078
#define ELBI_PHY_PORT_RST       0x1400
#define ELBI_PHY_CMN_RST        0x1404
#define ELBI_PHY_INIT_RST       0x1408

#define S_L0                    0x11

#define GATE_ON(v)      (((v) | (1u << 20) | (1u << 21)) & ~(1u << 28))

/* microseconds, by the generic timer */
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

/* poll until (reg & mask) == want, at most max_us; logs the outcome either way */
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
	puts_("wifiprobe4: ");
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

static void
show_pins(const char *when)
{
	u32 con = *reg(HSI2_GPIO + GPIO_CON), dat = *reg(HSI2_GPIO + GPIO_DAT);

	puts_("wifiprobe4: gph2 ");
	puts_(when);
	puts_(": con 0x");
	hex(con, 8);
	puts_(" dat 0x");
	hex(dat, 2);
	puts_(" pud 0x");
	hex(*reg(HSI2_GPIO + GPIO_PUD), 8);
	puts_(" -- PERST# fn ");
	hex(con & 0xf, 1);
	puts_(" lvl ");
	hex(dat & 1, 1);
	puts_(", CLKREQ# fn ");
	hex((con >> 4) & 0xf, 1);
	puts_(" lvl ");
	hex((dat >> 1) & 1, 1);
	puts_(", WL_REG_ON fn ");
	hex((con >> 16) & 0xf, 1);
	puts_(" lvl ");
	hex((dat >> 4) & 1, 1);
	put('\n');
}

struct regval {
	u32 off, val;
};

/* exynos_pcie_rc_pcie_phy_config, pma_setting: common block (rc-cal.c l. 180-218), in order */
static const struct regval phy_common[] = {
	{ 0x018, 0x50 }, { 0x048, 0x33 }, { 0x068, 0x01 }, { 0x070, 0x12 }, { 0x08c, 0x00 },
	{ 0x090, 0x21 }, { 0x0b0, 0x14 }, { 0x0b8, 0x50 }, { 0x0e0, 0x51 }, { 0x100, 0x00 },
	{ 0x104, 0x80 }, { 0x140, 0x38 }, { 0x180, 0xa4 }, { 0x188, 0x03 }, { 0x2a8, 0x38 },
	{ 0x2e4, 0x12 },
	/* PMA enable, aggregation mode */
	{ 0x400, 0x80 }, { 0x408, 0x20 }, { 0x550, 0x00 }, { 0x5a8, 0x00 }, { 0x5ec, 0xff },
	/* PCIe_TYPE: RC -- 100 MHz CLK on, REFCLK source, L1.2 entry delay */
	{ 0x458, 0x02 }, { 0x5b0, 0x34 }, { 0x450, 0x20 },
	/* PHY INPUT CLK 38.4 */
	{ 0x0ac, 0x34 }, { 0x0b4, 0x34 }, { 0x0e0, 0x50 }, { 0x0f4, 0x00 }, { 0x0f8, 0x08 },
	{ 0x104, 0xa0 }, { 0x11c, 0x19 }, { 0x124, 0x17 }, { 0x220, 0x41 },
};

/* ... per lane (lane 0 only: num_lanes 1), l. 223-319, in order (some offsets written twice) */
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
	/* RX tuning */
	{ 0x8fc, 0x80 }, { 0x914, 0xf4 }, { 0x91c, 0xd3 }, { 0x920, 0xca }, { 0x928, 0x3d },
	{ 0x92c, 0xb8 }, { 0x930, 0x41 }, { 0x934, 0x17 }, { 0x93c, 0x4c }, { 0x948, 0x73 },
	{ 0x94c, 0xfc }, { 0x96c, 0x55 }, { 0x988, 0x78 }, { 0x994, 0x3b }, { 0x9b4, 0xf6 },
	{ 0x9c4, 0xff }, { 0x9c8, 0x20 }, { 0xa08, 0x2f }, { 0xb9c, 0x3f },
	/* GEN4 TX termination resistor */
	{ 0x9cc, 0x00 }, { 0xcd8, 0xff }, { 0xcdc, 0x6e },
	/* TX idrv for termination resistor */
	{ 0x82c, 0x0f }, { 0x830, 0x60 }, { 0x834, 0x7e },
	/* Auto FBB */
	{ 0xc08, 0x00 }, { 0xc10, 0x09 }, { 0xc40, 0x04 }, { 0xc70, 0x00 },
	/* DFE */
	{ 0x9b4, 0x76 },
};

/* ... and PHY INPUT CLK 38.4 for both halves (+0, +0x800) whatever the lanes, l. 323-332 */
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

/* PHY reset values (rc-cal.c reset_table) */
static const struct regval phy_reset_table[] = {
	{ 0x0010, 0x55 }, { 0x0014, 0x51 }, { 0x0040, 0x50 }, { 0x0044, 0x0c }, { 0x00d8, 0x28 },
	{ 0x1054, 0x77 },
};

/* exynos_pcie_rc_pcie_phy_config (rc-cal.c l. 154-392), num_lanes 1, without the udbg dump setup */
static void
phy_config(void)
{
	u32 v;

	for (unsigned i = 0; i < N(phy_reset_table); i++) {
		v = *reg(PCIE1_PHY + phy_reset_table[i].off);
		puts_("wifiprobe4: phy reset value 0x");
		hex(phy_reset_table[i].off, 4);
		puts_(" = 0x");
		hex(v, 2);
		puts_(v == phy_reset_table[i].val ? " (as expected)\n" : " (EXPECTED OTHER)\n");
	}

	/* init. input clk path */
	*reg(PCIE1_PHY + 0xd8) = 0x28;
	/* PCS MUX glitch W/A */
	*reg(PCIE1_PCS + 0x008) |= 1u << 7;

	/* PHY CMN_RST, INIT_RST, PORT_RST assert */
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 0;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 0;
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 0;
	udelay(10);
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 1;
	udelay(10);
	puts_("wifiprobe4: phy resets asserted, CMN_RST released\n");

	write_table(PCIE1_PHY, phy_common, N(phy_common));
	write_table(PCIE1_PHY, phy_lane, N(phy_lane));
	write_table(PCIE1_PHY, phy_clk, N(phy_clk));
	write_table(PCIE1_PHY + 0x800, phy_clk, N(phy_clk));
	puts_("wifiprobe4: pma settings written\n");
	wdt_pet();

	/* PCS: aggregation mode (bifurcation off); RC; L2 entry and power; L2 power-down delay */
	*reg(PCIE1_PCS + 0x004) = 0;
	*reg(PCIE1_PCS + 0x154) = 0x700d5;
	*reg(PCIE1_PCS + 0x150) = 0x300ff;
	*reg(PCIE1_PCS + 0x170) = 0x40;
	/* L1.2: ERIO CLK gating */
	v = *reg(PCIE1_PCS + 0x008);
	v |= (1u << 4) | (1u << 5);
	*reg(PCIE1_PCS + 0x008) = v;
	/* PLL & BIAS EN off delay */
	*reg(PCIE1_PCS + 0x190) = 0x100b0808;
	puts_("wifiprobe4: pcs settings written\n");

	/* PHY CMN_RST, PORT_RST release */
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 1;

	/* additional PMA: XO clock configuration */
	v = *reg(PCIE1_PHY + 0x5d0);
	v |= 1u << 4;
	v &= ~(1u << 3);
	*reg(PCIE1_PHY + 0x5d0) = v;
	kv("wifiprobe4: phy 0x5d0 (XO clock config)", *reg(PCIE1_PHY + 0x5d0));
	/* AFC cal mode: restart from the initial value */
	*reg(PCIE1_PHY + 0xbf4) = 4;
	puts_("wifiprobe4: phy_config done\n");
}

/* exynos_pcie_rc_phy_all_pwrdn_clear (rc-cal.c l. 76-107) */
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
	/* reset rate change and powerdown value */
	*reg(PCIE1_PCS + 0x184) = 0;
	puts_("wifiprobe4: phy_all_pwrdn_clear done\n");
}

static void
dump_failure_state(void)
{
	puts_("wifiprobe4: state for the record\n");
	kv("  elbi RDLH_LINKUP/LTSSM (0x2c8)", *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP));
	kv("  elbi CXPL_DEBUG_INFO_H (0x2cc)", *reg(PCIE1_ELBI + 0x2cc));
	kv("  elbi PM_DSTATE (0x2e8)", *reg(PCIE1_ELBI + ELBI_PM_DSTATE));
	kv("  elbi IRQ0 (0x0)", *reg(PCIE1_ELBI + 0x0));
	kv("  elbi IRQ1 (0x4)", *reg(PCIE1_ELBI + 0x4));
	kv("  elbi IRQ2 (0x8)", *reg(PCIE1_ELBI + 0x8));
	kv("  pcs PM_POWER_STATE (0x188)", *reg(PCIE1_PCS + 0x188));
	kv("  phy 0x3f0 (PLL lock, bit 3)", *reg(PCIE1_PHY + 0x3f0));
	kv("  phy 0xe0c (CDR lock, bit 2)", *reg(PCIE1_PHY + 0xe0c));
	kv("  phy 0xde8 (OC done, 0xf)", *reg(PCIE1_PHY + 0xde8));
	kv("  phy 0xe18 (bit 7: OC after soft reset)", *reg(PCIE1_PHY + 0xe18));
	kv("  phy 0x760", *reg(PCIE1_PHY + 0x760));
	kv("  phy 0xfc0", *reg(PCIE1_PHY + 0xfc0));
	kv("  phy 0x5fc", *reg(PCIE1_PHY + 0x5fc));
	kv("  dbi link ctrl/status (0x80)", *reg(PCIE1_DBI + 0x80));
	dump("pcie1 elbi state history (0xc00..)", PCIE1_ELBI + ELBI_HISTORY_CHECK, 0x90);
	show_pins("at the end");
}

void
at_entry(u64 el, u64 dtb)
{
	u64 x0;

	(void)dtb;
	log_open();
	puts_("\nwifiprobe4: the Wi-Fi PCIe link (BCM4389 on PCIe1), LTSSM to L0, then its config space\n");
	kv("wifiprobe4: at entry, EL", el);
	if (el != 2) {
		puts_("wifiprobe4: not at EL2: no SMC possible, the PHY stays isolated\n");
		return;
	}
	/* PCIE_PHY_BYPASS: SMC_CMD_PRIV_REG, x1 the register, x2 READ 0 / WRITE 1, x3 the value */
	x0 = psci_call(0x82000504, PMU_ISOL, 0, 0);
	kv("wifiprobe4: EL2 PRIV_REG READ of pmu 0x3ec4", x0);
	if ((x0 & ~1ul) == 0) {
		x0 = psci_call(0x82000504, PMU_ISOL, 1, x0 | 1);
		kv("wifiprobe4: EL2 PRIV_REG WRITE answered", x0);
		kv("wifiprobe4: EL2 PRIV_REG READ again", psci_call(0x82000504, PMU_ISOL, 0, 0));
	}
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

	/* 0. as found; clocks forced on as in wifiprobe3 (they were on already; harmless) */
	kv("wifiprobe4: pmu 0x3ec4 (PHY isolation bypass, want 1)", *reg(PMU_ISOL));
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
	kv("wifiprobe4: hsi2 user mux PCIE (0x620)", *reg(CMU_HSI2 + 0x620));
	kv("wifiprobe4: elbi 0x2c8 before anything (LTSSM)", *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP));
	kv("wifiprobe4: elbi 0x80 device type", *reg(PCIE1_ELBI + ELBI_DEVICE_TYPE));
	dump("hsi2 sysreg", HSI2_SYSREG, 0x10);
	show_pins("as found");
	wdt_pet();

	/* 1. PERST# (gph2[0]) an output, low -- before the chip has power, as Google's probe does */
	pin_field(GPIO_PUD, PIN_PERST, 0);
	pin_set(PIN_PERST, 0);
	pin_field(GPIO_CON, PIN_PERST, 1);
	pin_set(PIN_PERST, 0);
	/* CLKREQ# (gph2[1]): function 2, pull-up (pinctrl pcie1_clkreq) */
	pin_field(GPIO_PUD, PIN_CLKREQ, 3);
	pin_field(GPIO_CON, PIN_CLKREQ, 2);
	show_pins("PERST# out low, CLKREQ# fn 2");

	/* 2. WL_REG_ON (gph2[4]) high, then the chip's power-on time */
	pin_field(GPIO_PUD, PIN_WL_REG_ON, 0);
	pin_set(PIN_WL_REG_ON, 1);
	pin_field(GPIO_CON, PIN_WL_REG_ON, 1);
	pin_set(PIN_WL_REG_ON, 1);
	show_pins("WL_REG_ON high");
	mdelay(200);
	show_pins("200 ms later");
	wdt_pet();

	/* 3. phy all power down clear (in poweron, before establish_link) */
	phy_all_pwrdn_clear();
	kv("wifiprobe4: phy 0x5fc (OC initial status)", *reg(PCIE1_PHY + 0x5fc));

	/* 4. PHY + PCS config */
	phy_config();
	wdt_pet();

	/* 5. locks: PLL, CDR, offset calibration (Google: 10000 x 1 us each, logs and goes on) */
	poll("PLL lock (phy 0x3f0 bit 3)", PCIE1_PHY + 0x3f0, 1u << 3, 1u << 3, 10000);
	poll("CDR lock (phy 0xe0c bit 2)", PCIE1_PHY + 0xe0c, 1u << 2, 1u << 2, 10000);
	poll("OC done (phy 0xde8 = 0xf)", PCIE1_PHY + 0xde8, 0xf, 0xf, 10000);
	wdt_pet();

	/* 6. soft power reset */
	v = *reg(PCIE1_ELBI + ELBI_SOFT_RESET);
	kv("wifiprobe4: elbi SOFT_RESET (0x3a4) before", v);
	v &= ~SOFT_PWR_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	mdelay(1);
	v |= SOFT_PWR_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	v = *reg(PCIE1_PHY + 0xe18);
	kv("wifiprobe4: phy 0xe18 after soft power reset (bit 7 = OC ok)", v);

	/* 7. device type RC */
	*reg(PCIE1_ELBI + ELBI_DEVICE_TYPE) = 4;

	/* non-sticky reset */
	v = *reg(PCIE1_ELBI + ELBI_SOFT_RESET);
	v |= SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	udelay(10);
	v &= ~SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	mdelay(1);
	v |= SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	kv("wifiprobe4: elbi SOFT_RESET after the resets", *reg(PCIE1_ELBI + ELBI_SOFT_RESET));
	kv("wifiprobe4: elbi device type after", *reg(PCIE1_ELBI + ELBI_DEVICE_TYPE));

	/* 8. EQ off (DBI GEN3_RELATED 0x890) */
	*reg(PCIE1_DBI + 0x890) = 0x12000;
	kv("wifiprobe4: dbi 0x890 (EQ off, want 0x12000)", *reg(PCIE1_DBI + 0x890));
	wdt_pet();

	/* 9. PERST# high, then 20 ms for BCM Wi-Fi */
	pin_set(PIN_PERST, 1);
	show_pins("PERST# high");
	mdelay(20);

	/* 10. L1 exit / NAK master, link-down reset manual, Q-channel (ip-ver 0x984500: gating off), NAK, DBI L1 exit */
	v = *reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1);
	v |= 1u | (1u << 4);
	*reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1) = v;
	*reg(PCIE1_ELBI + ELBI_LINKDOWN_RST_CTRL) = 1u << 1;
	v = *reg(PCIE1_ELBI + ELBI_QCH_SEL);
	v &= ~((0xfu << 8) | (0xfu << 4) | 0xfu);
	*reg(PCIE1_ELBI + ELBI_QCH_SEL) = v;
	*reg(PCIE1_ELBI + ELBI_MSTR_PEND_SEL_NAK) = 1;
	*reg(PCIE1_ELBI + ELBI_DBI_L1_EXIT_DIS) = 1;
	kv("wifiprobe4: elbi 0x3bc (L1 exit)", *reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1));
	kv("wifiprobe4: elbi 0x3a8 (Q-channel)", *reg(PCIE1_ELBI + ELBI_QCH_SEL));

	/*
	 * 11. what dw_pcie_setup_rc + exynos_pcie_setup_rc do before the LTSSM, the parts that matter
	 * for a first link: read-only writes on, 1 lane, Gen2 target, bus numbers 0/1/ff, memory +
	 * bus master on.
	 */
	*reg(PCIE1_DBI + 0x8bc) = 1;                            /* MISC_CONTROL_1: DBI_RO_WR_EN */
	v = *reg(PCIE1_DBI + 0x710);                            /* PORT_LINK_CONTROL: LINK_CAPABLE x1 */
	v = (v & ~(0x3fu << 16)) | (0x1u << 16);
	*reg(PCIE1_DBI + 0x710) = v;
	v = *reg(PCIE1_DBI + 0x80c);                            /* GEN2_CTRL: NUM_OF_LANES 1 */
	v = (v & ~(0x1fu << 8)) | (0x1u << 8);
	*reg(PCIE1_DBI + 0x80c) = v;
	v = *reg(PCIE1_DBI + 0x70 + 0x0c);                      /* LNKCAP: L1 exit 64 us, max speed 3 */
	v &= ~((0x7u << 15) | 0xfu);
	v |= (0x7u << 15) | 0x3u;
	*reg(PCIE1_DBI + 0x70 + 0x0c) = v;
	*reg(PCIE1_DBI + 0x40 + 0x04) = 0;                      /* PM control/status cleared */
	v = *reg(PCIE1_DBI + 0x70 + 0x30);                      /* LNKCTL2: target speed Gen2 (DT) */
	v = (v & ~0xfu) | 0x2u;
	*reg(PCIE1_DBI + 0x70 + 0x30) = v;
	*reg(PCIE1_DBI + 0x18) = 0x00ff0100;                    /* primary 0, secondary 1, subordinate ff */
	*reg(PCIE1_DBI + 0x04) |= 0x6;                          /* memory space, bus master */
	kv("wifiprobe4: dbi 0x00 (root port id)", *reg(PCIE1_DBI + 0x00));
	kv("wifiprobe4: dbi 0x7c (LNKCAP)", *reg(PCIE1_DBI + 0x7c));
	kv("wifiprobe4: dbi 0xa0 (LNKCTL2)", *reg(PCIE1_DBI + 0xa0));
	kv("wifiprobe4: dbi 0x710 (PORT_LINK_CONTROL)", *reg(PCIE1_DBI + 0x710));
	kv("wifiprobe4: dbi 0x80c (GEN2_CTRL)", *reg(PCIE1_DBI + 0x80c));
	kv("wifiprobe4: elbi 0x2e8 D state", *reg(PCIE1_ELBI + ELBI_PM_DSTATE));
	kv("wifiprobe4: elbi 0x2c8 before LTSSM enable", *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP));
	wdt_pet();

	/* 12. LTSSM enable, poll for L0 -- every state change logged, 500 ms at most (Google: ~50 ms) */
	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 1;
	t0 = now_us();
	last = 0xffffffff;
	for (i = 0; ; i++) {
		v = *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP);
		s = v & 0x3f;
		if (v != last) {
			puts_("wifiprobe4: ltssm +");
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
	kv(linked ? "wifiprobe4: LINK UP (L0) after us" : "wifiprobe4: NO L0 -- gave up after us",
	   now_us() - t0);
	wdt_pet();

	if (!linked) {
		dump_failure_state();
	} else {
		mdelay(3);                                      /* Google: room for the speed change */
		v = *reg(PCIE1_DBI + 0x80);
		kv("wifiprobe4: dbi link ctrl/status (0x80)", v);
		kv("wifiprobe4: link speed (Gen)", (v >> 16) & 0xf);
		kv("wifiprobe4: link width (x)", (v >> 20) & 0x3f);
		kv("wifiprobe4: elbi 0x2c8 now", *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP));

		/* 13. iATU outbound region 0: CFG0 to bus 1 dev 0 fn 0 through the config window */
		*reg(PCIE1_ATU + 0x08) = (u32)PCIE1_CFG;                /* lower base */
		*reg(PCIE1_ATU + 0x0c) = 0;                             /* upper base */
		*reg(PCIE1_ATU + 0x10) = (u32)PCIE1_CFG + 0xfff;        /* limit */
		*reg(PCIE1_ATU + 0x14) = 1u << 24;                      /* target: bus 1, dev 0, fn 0 */
		*reg(PCIE1_ATU + 0x18) = 0;
		*reg(PCIE1_ATU + 0x00) = 0x4;                           /* CR1: type CFG0 */
		*reg(PCIE1_ATU + 0x04) = 1u << 31;                      /* CR2: enable */
		udelay(10);
		kv("wifiprobe4: iATU out0 CR1", *reg(PCIE1_ATU + 0x00));
		kv("wifiprobe4: iATU out0 CR2 (bit 31 enabled)", *reg(PCIE1_ATU + 0x04));
		kv("wifiprobe4: iATU out0 base", *reg(PCIE1_ATU + 0x08));
		kv("wifiprobe4: iATU out0 limit", *reg(PCIE1_ATU + 0x10));
		kv("wifiprobe4: iATU out0 target", *reg(PCIE1_ATU + 0x14));

		v = *reg(PCIE1_CFG + 0x0);
		kv("wifiprobe4: BUS 1 VENDOR/DEVICE ID (want 0x444114e4)", v);
		if (v == 0x444114e4)
			puts_("wifiprobe4: *** IT IS THE BCM4389 (14e4:4441) ***\n");
		kv("wifiprobe4: bus 1 class/revision (0x08)", *reg(PCIE1_CFG + 0x8));
		dump("bus 1 dev 0 config space", PCIE1_CFG, 0x40);
		kv("wifiprobe4: elbi 0x2c8 after the reads", *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP));
		kv("wifiprobe4: elbi IRQ2 (0x8; bit 24 completion timeout)", *reg(PCIE1_ELBI + 0x8));
	}

	/* leave the chip off and in reset: LTSSM off, PERST# low, WL_REG_ON low */
	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 0;
	pin_set(PIN_PERST, 0);
	pin_set(PIN_WL_REG_ON, 0);
	show_pins("off again");
	kv("wifiprobe4: total us", now_us() - t_start);
	puts_("wifiprobe4: done; letting the watchdog reset the phone\n");
	hang();
}
