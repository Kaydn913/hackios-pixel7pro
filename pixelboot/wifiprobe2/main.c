/*
 * wifiprobe2: the Wi-Fi PCIe controller's clocks on, then does it answer? (WIFI-PLAN.md, W1)
 *
 * wifiprobe1 (2026-10-25): the first read of 14520000.pcie's ELBI never came back -- the block is not
 * clocked. HSI2 holds two DesignWare controllers, PCIE_003 (x2) and PCIE_004 (x1) of PCIE_GEN4_1;
 * which is 14520000 is to be seen. Their clocks (Google's gs201 cmucal-sfr.c, downloads/ref): the
 * top's CLKCMU_HSI2_PCIE (CMU_TOP 0x1e080000: gate 0x20c8, mux 0x10a4, div 0x18a0), HSI2's user mux
 * (CMU_HSI2 0x14400000 + 0x620: bit 4 = from the top, bit 16 busy), and HSI2's gates for both. A gate
 * is on with MANUAL (bit 20) and CG_VAL (bit 21) set and automatic gating (bit 28) clear. Nothing of
 * the PHY or the link is touched yet.
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

void
at_entry(u64 el, u64 dtb)
{
	(void)el;
	(void)dtb;
	log_open();
	puts_("\nwifiprobe2: the Wi-Fi PCIe controller's clocks on, then does it answer?\n");
}

static void
show_clocks(const char *when)
{
	puts_("wifiprobe2: clocks ");
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

	/* the PHY first: its reset values (pcie-exynos-gs201-rc-cal.c's reset_table) say it is there and fresh */
	kv("wifiprobe2: phy 0x10 (reset 0x55)", *reg(PCIE1_PHY + 0x10));
	kv("wifiprobe2: phy 0x14 (reset 0x51)", *reg(PCIE1_PHY + 0x14));
	kv("wifiprobe2: phy 0x40 (reset 0x50)", *reg(PCIE1_PHY + 0x40));
	kv("wifiprobe2: phy 0xd8 (reset 0x28)", *reg(PCIE1_PHY + 0xd8));
	kv("wifiprobe2: phy 0x1054 (reset 0x77)", *reg(PCIE1_PHY + 0x1054));
	wdt_pet();
	dump("pcie1 pcs", PCIE1_PCS, 0x40);
	wdt_pet();
	dump("pcie1 elbi", PCIE1_ELBI, 0x400);
	kv("wifiprobe2: elbi RDLH_LINKUP / LTSSM (0x2c8)", *reg(PCIE1_ELBI + 0x2c8));
	wdt_pet();
	dump("pcie1 dbi (root port config)", PCIE1_DBI, 0x40);
	puts_("wifiprobe2: done; letting the watchdog reset the phone\n");
	hang();
}
