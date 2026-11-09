/*
 * rbtest: can a bare-metal program send the phone back to fastboot, with RAM kept? The question
 * behind it: the 2.5 GB iOS system image does not fit one `fastboot boot`, but it would fit in
 * chunks if each run could leave its chunk in RAM and come back to fastboot for the next one.
 *
 *   RB_STAGE=1: write a pattern into high RAM (XNU's window), ask for "bootloader" as the reboot
 *               mode, and let the watchdog reset the phone (warm). Expected: fastboot, not Android.
 *   RB_STAGE=2: (booted from that fastboot) check the pattern, ask for a normal boot, and let the
 *               watchdog reset the phone: Android then shows both runs in pstore.
 *
 * The reboot mode is the PMU's SYSIP_DAT0 (0x18060810; "pixel-reboot" in the device tree,
 * reboot-cmd-offset 0x810), "bootloader" being 0xfc as in upstream Linux's gs101 reboot-mode. PMU
 * registers are written through EL3 on Tensor (Linux's tensor_sec_reg_write: SMC 0x82000504 with
 * the address, 1 = write, the value); they read back directly.
 */
#include "board.h"

#ifndef RB_STAGE
#error "RB_STAGE=1 or 2"
#endif

#define PMU_SYSIP_DAT0		0x18060810ul
#define REBOOT_MODE_BOOTLOADER	0xfcu
#define REBOOT_MODE_NORMAL	0x00u
#define SMC_PMU_SEC_REG		0x82000504ul
#define PMUREG_WRITE		1

/*
 * First run: 8 MB at 0x900000000 did not survive (0 words intact), though the ramoops zone at
 * 0xfd3ff000 always does. So: 1 MB in many places -- every high bank, and free stretches of the
 * low one (between the firmware's carve-outs) -- and what each holds afterwards (zero: wiped).
 */
static const u64 spots[] = {
	0x880000000, 0x900000000, 0x980000000, 0xa00000000, 0xa80000000, 0xaff000000,
	0x88000000, 0xc4000000, 0xd0000000, 0xe4000000, 0xf0000000, 0xfc000000,
};
#define NSPOTS			(sizeof(spots) / sizeof(spots[0]))
#define PATTERN_WORDS		(1u << 17)	/* 1 MB each */

static u64
pattern(u64 i)
{
	return 0x5049584c00000000ul ^ (i * 0x9e3779b97f4a7c15ul);	/* "PIXL", varied */
}

static void
set_reboot_mode(u32 mode)
{
	u64 r = psci_call(SMC_PMU_SEC_REG, PMU_SYSIP_DAT0, PMUREG_WRITE, mode);
	kv("rbtest: pmu write smc returned", r);
	kv("rbtest: sysip_dat0 now", *reg(PMU_SYSIP_DAT0));
}

void
at_entry(u64 el, u64 dtb)
{
	(void)el, (void)dtb;
	log_open();
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	(void)el, (void)entry_el, (void)dtb;

	wdt_pet();
	puts_("\nrbtest: stage ");
	put('0' + RB_STAGE);
	put('\n');
	kv("rbtest: sysip_dat0 at entry", *reg(PMU_SYSIP_DAT0));

	if (RB_STAGE == 1) {
		for (unsigned s = 0; s < NSPOTS; s++) {
			volatile u64 *p = (volatile u64 *)spots[s];
			for (u64 i = 0; i < PATTERN_WORDS; i++)
				p[i] = pattern(i) ^ spots[s];
			wdt_pet();
		}
		puts_("rbtest: patterns written (1 MB at each spot)\n");
		set_reboot_mode(REBOOT_MODE_BOOTLOADER);
		/*
		 * Second run: after the watchdog's reset every spot had decayed (mostly zeros, bits
		 * flipped: not refreshed), only the firmware's debug regions (ramoops) kept. A warm
		 * reboot -- PSCI 1.1 SYSTEM_RESET2, type 0 -- may keep all of RAM refreshed.
		 */
		u64 feat = psci_call(0x8400000a, 0xc4000012, 0, 0);	/* PSCI_FEATURES(SYSTEM_RESET2_64) */
		kv("rbtest: psci features of system_reset2", feat);
		if ((s64)feat >= 0) {
			puts_("rbtest: reboot mode bootloader; PSCI SYSTEM_RESET2 warm -- expect fastboot\n");
			log_sync();
			u64 r = psci_call(0xc4000012, 0, 0, 0);
			kv("rbtest: system_reset2 returned", r);
		}
		puts_("rbtest: letting the watchdog reset the phone -- expect fastboot\n");
	} else {
		for (unsigned s = 0; s < NSPOTS; s++) {
			volatile u64 *p = (volatile u64 *)spots[s];
			u64 good = 0, zero = 0;
			for (u64 i = 0; i < PATTERN_WORDS; i++) {
				u64 v = p[i];
				good += v == (pattern(i) ^ spots[s]);
				zero += v == 0;
			}
			puts_("rbtest: spot ");
			hex(spots[s], 10);
			puts_(" intact ");
			hex(good, 5);
			puts_(" zero ");
			hex(zero, 5);
			puts_(" of 20000; first word ");
			hex(p[0], 16);
			put('\n');
			wdt_pet();
		}
		set_reboot_mode(REBOOT_MODE_NORMAL);
		puts_("rbtest: reboot mode normal; letting the watchdog reset the phone -- expect Android\n");
	}
	hang();
}
