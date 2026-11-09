/*
 * wifiprobe: the Wi-Fi chip's PCIe link as the bootloader leaves it (WIFI-PLAN.md, W1). Reads only.
 *
 * The BCM4389 (14e4:4441) sits on the second PCIe controller, 14520000.pcie (exynos-pcie-rc, Google's
 * pcie-exynos-rc.c + pcie-exynos-gs201-rc-cal.c in downloads/ref/): one lane, Gen2 on this phone.
 * Its chip is powered by WL_REG_ON (gph2 pin 4) and reset by PERST# (gph2 pin 0), both in HSI2's
 * pin controller at 0x14440000; the PHY's isolation is the PMU's 0x3ec4. What is set already, what
 * is clocked, what answers? The blocks go from the safest to read to the least safe, so that a block
 * that never answers (the watchdog then resets the phone) leaves what came before it in the log.
 */
#include "board.h"

#define PMU             0x18060000ul
#define PCIE_PMU_ISOL   0x3ec4          /* pmu-offset: bit 0 set = the PHY not isolated (PCIE_PHY_BYPASS) */
#define HSI2_GPIO       0x14440000ul    /* gph2 first: CON +0x0 (4 bits a pin), DAT +0x4, PUD +0x8, DRV +0xc */
#define HSI2_SYSREG     0x14420000ul
#define PCIE1_ELBI      0x14520000ul
#define PCIE1_PCS       0x14540000ul
#define PCIE1_PHY       0x14550000ul
#define PCIE1_DBI       0x14800000ul

static u64 entry_el_seen;

void
at_entry(u64 el, u64 dtb)
{
	(void)dtb;
	entry_el_seen = el;
	log_open();
	puts_("\nwifiprobe: the Wi-Fi chip's PCIe link as the bootloader leaves it\n");
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	u32 con, dat;

	(void)el;
	(void)entry_el;
	(void)dtb;
	wdt_pet();
	kv("wifiprobe: pmu pcie1 isolation (0x3ec4)", *reg(PMU + PCIE_PMU_ISOL));
	dump("hsi2 gpio (gph2 first)", HSI2_GPIO, 0x40);
	con = *reg(HSI2_GPIO);
	dat = *reg(HSI2_GPIO + 4);
	kv("wifiprobe: PERST# gph2[0] function", con & 0xf);
	kv("wifiprobe: PERST# gph2[0] level", dat & 1);
	kv("wifiprobe: WL_REG_ON gph2[4] function", (con >> 16) & 0xf);
	kv("wifiprobe: WL_REG_ON gph2[4] level", (dat >> 4) & 1);
	wdt_pet();
	dump("hsi2 sysreg", HSI2_SYSREG, 0x100);
	wdt_pet();
	dump("pcie1 elbi", PCIE1_ELBI, 0x400);
	wdt_pet();
	dump("pcie1 pcs", PCIE1_PCS, 0x200);
	wdt_pet();
	dump("pcie1 phy", PCIE1_PHY, 0x40);
	/* the PHY's lock and calibration status, as establish_link reads them */
	kv("wifiprobe: phy 0x3f0 (bit 3: PLL lock)", *reg(PCIE1_PHY + 0x3f0));
	kv("wifiprobe: phy 0xe0c (bit 2: CDR lock)", *reg(PCIE1_PHY + 0xe0c));
	kv("wifiprobe: phy 0xde8 (0xf: offset calibration done)", *reg(PCIE1_PHY + 0xde8));
	kv("wifiprobe: phy 0xe18", *reg(PCIE1_PHY + 0xe18));
	kv("wifiprobe: phy 0x5fc", *reg(PCIE1_PHY + 0x5fc));
	wdt_pet();
	/* the root port's own configuration space: 144d:eced when the core is clocked */
	dump("pcie1 dbi (root port config)", PCIE1_DBI, 0x40);
	puts_("wifiprobe: done; letting the watchdog reset the phone (warm, so this report survives)\n");
	hang();
}
