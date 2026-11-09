/*
 * wifiprobe3: the Wi-Fi PCIe block out of isolation, then does it answer? (WIFI-PLAN.md, W1)
 *
 * wifiprobe1: the first read of 14520000.pcie's ELBI never came back. wifiprobe2: the clocks were on
 * already (every gate CG_VAL, the user mux from the top) and turning them on by hand changed nothing:
 * the PHY and PCS answer with an external abort, ELBI not at all. What Google's driver does before
 * any of that is PCIE_PHY_BYPASS: bit 0 of the PMU's 0x3ec4 (pmu-offset), which the bootloader leaves 0.
 * Google writes it with rmw_priv_reg (exynos-el3_mon.h: an SMC to EL3, options READ 0, WRITE 1, RMW 2).
 * So, one after the other until the bit reads back set: a plain write; the PMU's set alias (+0xc000,
 * exynos-pmu-if.c); the SMC, SMC_CMD_PRIV_REG 0x82000504 as Samsung's trees have it (not confirmed:
 * an unknown SMC answers -1), as a WRITE of the value with bit 0 set. Clocks as in wifiprobe2.
 */
#include "board.h"

#define CMU_TOP         0x1e080000ul
#define CMU_HSI2        0x14400000ul
#define PCIE1_ELBI      0x14520000ul
#define PCIE1_PHY       0x14550000ul
#define PCIE1_PCS       0x14540000ul
#define PCIE1_DBI       0x14800000ul

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

/* every gate of HSI2 that names a PCIe controller of PCIE_GEN4_1, its PHY or what sits in front of it */
static const u32 hsi2_gates[] = {
	0x2008, 0x200c,                                 /* PHY_REFCLK_IN, 003 and 004 */
	0x2018, 0x201c, 0x2020, 0x2024,                 /* SSMT_PCIE_IA_GEN4A/B_1 */
	0x2050, 0x2054, 0x2058, 0x205c,                 /* 003 (x2): DBI, MSTR, SLV, sub-ctrl APB */
	0x2060, 0x2064, 0x2068, 0x206c,                 /* 004 (x1): DBI, MSTR, SLV, sub-ctrl APB */
	0x2070, 0x2074,                                 /* PCS/PMA: PHY UDBG APB, PIPE PAL APB */
	0x207c, 0x2080,                                 /* PCIE_IA_GEN4A/B_1 */
	0x2094, 0x2098, 0x209c, 0x20a0,                 /* QE_PCIE_GEN4A/B_HSI2 */
	0x20c8, 0x20cc, 0x20d0, 0x20d4, 0x20d8, 0x20dc, 0x20e0, 0x20e4,         /* UASC_PCIE_GEN4A/B_DBI/SLV_1 */
};
#define GATES   (sizeof(hsi2_gates) / sizeof(hsi2_gates[0]))

/*
 * PCIE_PHY_BYPASS by the secure monitor, from EL2 (wifiprobe3c: from EL1 not even PSCI_VERSION came
 * back, though the same SMCs made by xnuboot's EL2 monitor do). SMC_CMD_PRIV_REG 0x82000504 is
 * Google's (include/linux/soc/samsung/exynos-smc.h); x1 the register, x2 the option, x3 the value.
 */
static void
release_isolation_at_el2(u64 el)
{
	const u64 isol = 0x18060000ul + 0x3ec4;
	u64 x0;

	kv("wifiprobe3: at entry, EL", el);
	if (el != 2) {
		return;
	}
	x0 = psci_call(0x84000000, 0, 0, 0);
	kv("wifiprobe3: EL2 PSCI_VERSION answered", x0);
	x0 = psci_call(0x82000504, isol, 0, 0);                 /* PRIV_REG_OPTION_READ */
	kv("wifiprobe3: EL2 PRIV_REG READ of 0x3ec4 answered", x0);
	if ((x0 & ~1ul) == 0) {
		x0 = psci_call(0x82000504, isol, 1, x0 | 1);    /* PRIV_REG_OPTION_WRITE */
		kv("wifiprobe3: EL2 PRIV_REG WRITE answered", x0);
		kv("wifiprobe3: EL2 PRIV_REG READ again", psci_call(0x82000504, isol, 0, 0));
	}
}

void
at_entry(u64 el, u64 dtb)
{
	(void)dtb;
	log_open();
	puts_("\nwifiprobe3: the Wi-Fi PCIe controller's clocks on, then does it answer?\n");
	release_isolation_at_el2(el);
}

static void
show_clocks(const char *when)
{
	puts_("wifiprobe3: clocks ");
	puts_(when);
	puts_("\n");
	kv("  top gate CLKCMU_HSI2_PCIE (0x20c8)", *reg(CMU_TOP + 0x20c8));
	kv("  top mux CLKCMU_HSI2_PCIE (0x10a4)", *reg(CMU_TOP + 0x10a4));
	kv("  top div CLKCMU_HSI2_PCIE (0x18a0)", *reg(CMU_TOP + 0x18a0));
	kv("  hsi2 user mux PCIE (0x620)", *reg(CMU_HSI2 + 0x620));
	for (unsigned i = 0; i < GATES; i++) {
		puts_("  hsi2 gate ");
		hex(hsi2_gates[i], 4);
		puts_(" = ");
		hex(*reg(CMU_HSI2 + hsi2_gates[i]), 8);
		put('\n');
	}
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	(void)el;
	(void)entry_el;
	(void)dtb;
	wdt_pet();
	show_clocks("as found");

	*reg(CMU_TOP + 0x20c8) = GATE_ON(*reg(CMU_TOP + 0x20c8));
	*reg(CMU_HSI2 + 0x620) |= 1u << 4;
	for (unsigned i = 0; i < 1000 && (*reg(CMU_HSI2 + 0x620) & (1u << 16)); i++) {
		udelay(1);
	}
	for (unsigned i = 0; i < GATES; i++) {
		*reg(CMU_HSI2 + hsi2_gates[i]) = GATE_ON(*reg(CMU_HSI2 + hsi2_gates[i]));
	}
	udelay(100);
	wdt_pet();
	show_clocks("after turning them on");

	/* PCIE_PHY_BYPASS: PMU 0x3ec4 bit 0 */
	{
		const u64 isol = 0x18060000ul + 0x3ec4;
		u64 x0;

		kv("wifiprobe3: pmu 0x3ec4 as found", *reg(isol));
		*reg(isol) = *reg(isol) | 1;
		kv("wifiprobe3: pmu 0x3ec4 after a plain write", *reg(isol));
		if (!(*reg(isol) & 1)) {
			*reg(0x18060000ul + (0x3ec4 | 0xc000)) = 1;
			kv("wifiprobe3: pmu 0x3ec4 after the set alias", *reg(isol));
		}
		/* (no SMC from EL1: none comes back -- see release_isolation_at_el2) */
		(void)x0;
		udelay(100);
	}
	wdt_pet();

	/* the PHY first: its reset values (pcie-exynos-gs201-rc-cal.c's reset_table) say it is there and fresh */
	kv("wifiprobe3: phy 0x10 (reset 0x55)", *reg(PCIE1_PHY + 0x10));
	kv("wifiprobe3: phy 0x14 (reset 0x51)", *reg(PCIE1_PHY + 0x14));
	kv("wifiprobe3: phy 0x40 (reset 0x50)", *reg(PCIE1_PHY + 0x40));
	kv("wifiprobe3: phy 0xd8 (reset 0x28)", *reg(PCIE1_PHY + 0xd8));
	kv("wifiprobe3: phy 0x1054 (reset 0x77)", *reg(PCIE1_PHY + 0x1054));
	wdt_pet();
	dump("pcie1 pcs", PCIE1_PCS, 0x40);
	wdt_pet();
	dump("pcie1 elbi", PCIE1_ELBI, 0x400);
	kv("wifiprobe3: elbi RDLH_LINKUP / LTSSM (0x2c8)", *reg(PCIE1_ELBI + 0x2c8));
	wdt_pet();
	dump("pcie1 dbi (root port config)", PCIE1_DBI, 0x40);
	puts_("wifiprobe3: done; letting the watchdog reset the phone\n");
	hang();
}
