/*
 * gpuprobe: wake the Mali-G710 (gs201's G3D) and ask it who it is -- phase 1 of GPU acceleration.
 *
 * From the live device tree (mali@28000000, 0x480000 of registers; interrupts GPU 328, JOB 329,
 * MMU 330) and Google's cal-if tables (gs201/flexpmu_cal_local_gs201.h):
 *   power: two PMU domains, "top" pd-g3d (CONFIGURATION 0x18061e00, STATUS +4) and "cores"
 *          pd-embedded_g3d (0x18062000, STATUS +4); on = write 1, wait for status bit 0.
 *   clock: ACPM DVFS domains 5 and 6 (gpu1_cmu_cal_id 0x0b040005, gpu0_cmu_cal_id 0x0b040006).
 * Then the GPU's own registers (Linux panthor_gpu_regs.h): GPU_ID 0x0, L2/CORE_FEATURES 0x4/0x8,
 * AS_PRESENT 0x18, CSF_ID 0x1c, GPU_STATUS 0x34, SHADER_PRESENT 0x100, L2_PRESENT 0x120,
 * L2_READY 0x160, L2_PWRON 0x1a0, L2_PWRTRANS 0x220, MCU_STATUS 0x704 -- and the L2 is powered
 * up, the first thing a driver does, to see the GPU act on what it is told.
 * The log is synced before every step that could hang the bus, so the last line tells.
 */
#include "board.h"
#include "acpm.h"

#define PMU		0x18060000ul
#define PD_G3D		(PMU + 0x1e00)
#define PD_EMB_G3D	(PMU + 0x2000)
#define GPU		0x28000000ul

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

static int
wait_bits(u64 addr, u32 mask, u32 want, u64 ms)
{
	for (u64 t = ticks(); ticks() - t < freq() / 1000 * ms;)
		if ((*reg(addr) & mask) == want)
			return 0;
	return -1;
}

static void
domain_on(const char *name, u64 base)
{
	puts_("gpuprobe: ");
	puts_(name);
	puts_(" status ");
	hex(*reg(base + 4), 8);
	log_sync();
	*reg(base) = 1;
	int r = wait_bits(base + 4, 1, 1, 50);
	puts_(r ? " -> TIMEOUT, status " : " -> on, status ");
	hex(*reg(base + 4), 8);
	put('\n');
	log_sync();
}

static void
gpu_reg(const char *name, u64 off)
{
	puts_("gpuprobe: ");
	puts_(name);
	puts_(" = ");
	log_sync();
	hex(*reg(GPU + off), 8);
	put('\n');
}

void
at_entry(u64 el, u64 dtb)
{
	(void)el, (void)dtb;
	log_open();
	wdt_pet();
	puts_("\ngpuprobe: waking the Mali-G710\n");

	domain_on("pd-g3d (top)", PD_G3D);
	domain_on("pd-embedded_g3d (cores)", PD_EMB_G3D);

	struct acpm_ch c;
	if (acpm_open(0, &c) == 0) {
		for (u32 d = 5; d <= 6; d++) {
			puts_("gpuprobe: ACPM G3D domain ");
			hex(d, 1);
			puts_(" kHz ");
			hex(acpm_dvfs_get(&c, d), 8);
			put('\n');
		}
	}
	log_sync();

	gpu_reg("GPU_ID", 0x0);
	gpu_reg("L2_FEATURES", 0x4);
	gpu_reg("CORE_FEATURES", 0x8);
	gpu_reg("AS_PRESENT", 0x18);
	gpu_reg("CSF_ID", 0x1c);
	gpu_reg("GPU_STATUS", 0x34);
	gpu_reg("SHADER_PRESENT lo", 0x100);
	gpu_reg("L2_PRESENT lo", 0x120);
	gpu_reg("L2_READY lo", 0x160);
	gpu_reg("MCU_STATUS", 0x704);

	u32 l2 = *reg(GPU + 0x120);
	if (l2 && l2 != 0xffffffff) {
		puts_("gpuprobe: powering the L2 up\n");
		log_sync();
		*reg(GPU + 0x1a0) = l2;
		*reg(GPU + 0x1a4) = 0;
		int r = wait_bits(GPU + 0x160, l2, l2, 100);
		puts_(r ? "gpuprobe: L2 did not come up; " : "gpuprobe: L2 READY; ");
		puts_("L2_READY ");
		hex(*reg(GPU + 0x160), 8);
		puts_(" L2_PWRTRANS ");
		hex(*reg(GPU + 0x220), 8);
		put('\n');
	}
	puts_("gpuprobe: done; the watchdog resets the phone\n");
	log_sync();
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	(void)el, (void)entry_el, (void)dtb;
	hang();
}
