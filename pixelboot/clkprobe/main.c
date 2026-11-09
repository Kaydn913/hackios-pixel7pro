/*
 * clkprobe: how fast each core runs -- as the bootloader leaves them, and after asking ACPM (the
 * power-management firmware, acpm.c) for more. Each core counts its cycles (the PMU's cycle
 * counter) over 100 ms of the system timer, at EL2 with the MMU off; the others are started
 * with PSCI CPU_ON and measure again when told to.
 * Cores 0-3 are Cortex-A55 (part d05), 4-5 A78 (d41), 6-7 X1 (d44).
 * Build with DEFINES="ACPM_DVFS_CH=<n>" (acpm-ipc-channel of the DVFS node in the device tree).
 * Rates are asked for only if what ACPM reports first matches what the cores were measured at.
 */
#include "board.h"
#include "acpm.h"

#ifndef ACPM_DVFS_CH
#error "ACPM_DVFS_CH: the DVFS channel"
#endif
#ifndef CL0_KHZ
#define CL0_KHZ 1803000
#endif
#ifndef CL1_KHZ
#define CL1_KHZ 2348000
#endif
#ifndef CL2_KHZ
#define CL2_KHZ 2850000
#endif

static volatile u64 go, mhz[2][8], part[8], done[2][8];
__attribute__((used)) static u8 stacks[8][4096] __attribute__((aligned(16)));

static u64
ticks(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static u64
freq(void)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}

static u64
measure(void)
{
	u64 f = freq(), t0, t, c0, c1, el, v;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	if ((el >> 2) == 2) {			/* cycles are counted at EL2 too (MDCR_EL2.HCCD clear) */
		__asm__ volatile("mrs %0, mdcr_el2" : "=r"(v));
		__asm__ volatile("msr mdcr_el2, %0; isb" : : "r"(v & ~(1ul << 23)));
	}
	__asm__ volatile("msr pmccfiltr_el0, %0" : : "r"(1ul << 27));	/* NSH: and at EL2 */
	__asm__ volatile("msr pmcntenset_el0, %0" : : "r"(1ul << 31));
	__asm__ volatile("mrs %0, pmcr_el0" : "=r"(v));
	__asm__ volatile("msr pmcr_el0, %0; isb" : : "r"(v | 1 | 4 | 64));	/* E, reset C, 64-bit */
	__asm__ volatile("isb; mrs %0, cntvct_el0; mrs %1, pmccntr_el0" : "=r"(t0), "=r"(c0));
	do
		t = ticks();
	while (t - t0 < f / 10);
	__asm__ volatile("isb; mrs %0, pmccntr_el0" : "=r"(c1));
	return (c1 - c0) * (f / 1000) / (t - t0) / 1000;
}

void
sec_main(u64 core)
{
	u64 midr;
	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
	part[core] = (midr >> 4) & 0xfff;
	for (u64 r = 0; r < 2; r++) {
		while (go < r)
			;
		mhz[r][core] = measure();
		done[r][core] = 1;
	}
}

/* where CPU_ON starts a core: x0 = its number */
__attribute__((naked)) static void
sec_entry(void)
{
	__asm__ volatile(
		"msr spsel, #1\n"
		"adrp x1, stacks\n"
		"add x1, x1, :lo12:stacks\n"
		"add x2, x0, #1\n"
		"lsl x2, x2, #12\n"
		"add sp, x1, x2\n"
		"bl sec_main\n"
		"1: wfe\n"
		"b 1b\n");
}

static void
wait_round(u64 r)
{
	for (u64 t0 = ticks(); ticks() - t0 < 2 * freq();) {
		u64 n = 0;
		for (u64 c = 0; c < 8; c++)
			n += done[r][c];
		if (n == 8)
			return;
	}
}

static void
pause_ms(u64 n)
{
	for (u64 t0 = ticks(); ticks() - t0 < freq() / 1000 * n;)
		;
}

static int
near(u64 khz, u64 mhz_measured)
{
	u64 m = mhz_measured * 1000;
	return khz > m * 85 / 100 && khz < m * 115 / 100;
}

static void
raise_clocks(void)
{
	struct acpm_ch c;
	acpm_dump();
	if (acpm_open(ACPM_DVFS_CH, &c)) {
		kv("clkprobe: no usable ACPM channel", ACPM_DVFS_CH);
		return;
	}
	u32 khz[5];
	for (u32 d = 0; d < 5; d++) {
		khz[d] = acpm_dvfs_get(&c, d);
		puts_("clkprobe: ACPM domain ");
		hex(d, 1);
		puts_(" kHz ");
		hex(khz[d], 8);
		put('\n');
	}
	if (!near(khz[ACPM_DVFS_CPUCL0], mhz[0][0]) || !near(khz[ACPM_DVFS_CPUCL1], mhz[0][4]) ||
	    !near(khz[ACPM_DVFS_CPUCL2], mhz[0][6])) {
		puts_("clkprobe: ACPM's rates do not match the measured ones: nothing asked\n");
		return;
	}
	kv("clkprobe: set cl0", (u64)acpm_dvfs_set(&c, ACPM_DVFS_CPUCL0, CL0_KHZ));
	kv("clkprobe: set cl1", (u64)acpm_dvfs_set(&c, ACPM_DVFS_CPUCL1, CL1_KHZ));
	kv("clkprobe: set cl2", (u64)acpm_dvfs_set(&c, ACPM_DVFS_CPUCL2, CL2_KHZ));
	pause_ms(20);
	for (u32 d = ACPM_DVFS_CPUCL0; d <= ACPM_DVFS_CPUCL2; d++) {
		puts_("clkprobe: now domain ");
		hex(d, 1);
		puts_(" kHz ");
		hex(acpm_dvfs_get(&c, d), 8);
		put('\n');
	}
}

void
at_entry(u64 el, u64 dtb)
{
	(void)dtb;
	log_open();
	wdt_pet();
	puts_("\nclkprobe: core clocks, then asking ACPM for more\n");
	kv("clkprobe: entry EL", el);
	u64 midr;
	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
	part[0] = (midr >> 4) & 0xfff;
	mhz[0][0] = measure();
	done[0][0] = 1;
	for (u64 c = 1; c < 8; c++) {
		u64 r = psci_call(0xc4000003, c << 8, (u64)sec_entry, c);
		if (r)
			kv("clkprobe: CPU_ON failed, core", c), kv("clkprobe:   result", r);
	}
	wait_round(0);
	log_sync();
	raise_clocks();
	log_sync();
	go = 1;
	__asm__ volatile("dsb sy; sev" ::: "memory");
	mhz[1][0] = measure();
	done[1][0] = 1;
	wait_round(1);
	for (u64 c = 0; c < 8; c++) {
		puts_("clkprobe: core ");
		hex(c, 1);
		puts_(" part ");
		hex(part[c], 3);
		puts_(" MHz before ");
		hex(mhz[0][c], 4);
		puts_(" after ");
		hex(mhz[1][c], 4);
		put('\n');
	}
	puts_("clkprobe: (MHz in hex) done; the watchdog resets the phone\n");
	log_sync();
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	(void)el, (void)entry_el, (void)dtb;
	hang();
}
