/*
 * The EL2 monitor: what stays of pixelboot at EL2 while a kernel (XNU) runs at EL1.
 *
 * start.S leaves EL2 its vectors and a stack. A program at EL1 starts the monitor with
 * hvc_call(EL2_FN_MONITOR, page, flags): stage 2 is turned on with an identity map of everything
 * except one 4K page, and SMCs are trapped. That page is the kernel's serial port (a PL011 in its
 * device tree): every access to it faults to here, where a write to the data register becomes a
 * character in the log (board.c) -- which survives the watchdog's warm reset into Android's
 * /sys/fs/pstore/console-ramoops-0 -- and a read of the flag register says "transmitter empty,
 * nothing received". Each character also feeds the watchdogs, so the kernel lives as long as it
 * talks. PSCI calls are logged and passed on to EL3, except a reset or power-off: the monitor holds
 * there instead, so that the watchdog's warm reset keeps the log (a PSCI reset is a cold one).
 */
#include "board.h"
#include "touch.h"
#include "acpm.h"

/*
 * EL2_GOVERN: a thermal governor, since nothing of iOS's knows these clocks. At full rate the
 * big cores reached the TMU's hardware trip within a minute of iOS starting (2026-10-23, fast1).
 * Ten times a second (when the monitor has the CPU) the temperatures of the big and middle
 * clusters are read from ACPM (TMU_IPC_READ_TEMP on its TMU channel; Google's
 * drivers/thermal/samsung/exynos_acpm_tmu.c) and both clusters move one step down their rate
 * table at 80 C (three at 90 C), one step up below 70 C -- never above step GOV_TOP: the die
 * heats within a second or two, long before the phone feels warm.
 */
#define ACPM_TMU_CHANNEL	9		/* acpm_tmu's acpm-ipc-channel */
#define ACPM_DVFS_CHANNEL	0		/* cal_if's */
#define TMU_IPC_READ_TEMP	2
#ifndef TZ_BIG
#define TZ_BIG			0
#endif
#ifndef TZ_MID
#define TZ_MID			1
#endif
static const u32 cl2_steps[] = { 2850000, 2630000, 2401000, 2188000, 2048000, 1826000, 1582000, 1426000, 1106000, 851000 };
static const u32 cl1_steps[] = { 2348000, 2130000, 1999000, 1836000, 1663000, 1491000, 1328000, 1197000, 1024000, 799000 };
#define GOV_STEPS	(sizeof(cl2_steps) / sizeof(cl2_steps[0]))
#ifndef GOV_TOP
#define GOV_TOP		2		/* 2401 / 1999 MHz */
#endif
static struct acpm_ch gov_dvfs, gov_tmu;
static int gov_on, gov_step, gov_said;
static u64 gov_last, gov_period;

static int
tmu_read(u32 tz)
{
	u32 m[4] = { 0, TMU_IPC_READ_TEMP | tz << 16, 0, 0 };
	if (acpm_xfer(&gov_tmu, m) || ((m[1] >> 8) & 0xff))
		return -1;			/* no reply, or ret != 0 */
	return (m[1] >> 24) & 0xff;
}

static void
gov_set(int step)
{
	gov_step = step;
	acpm_dvfs_set(&gov_dvfs, ACPM_DVFS_CPUCL2, cl2_steps[step]);
	acpm_dvfs_set(&gov_dvfs, ACPM_DVFS_CPUCL1, cl1_steps[step]);
}

static void
govern(void)
{
	u64 now;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
	if (now - gov_last < gov_period)
		return;
	gov_last = now;
	int big = tmu_read(TZ_BIG), mid = tmu_read(TZ_MID), t = big > mid ? big : mid;
	if (t < 0) {
		gov_on = 0;
		puts_("\n[el2: governor: no temperature from ACPM; clocks left]\n");
		return;
	}
	int step = gov_step;
	if (t >= 90)
		step += 3;
	else if (t >= 80)
		step += 1;
	else if (t < 70)
		step -= 1;
	if (step < GOV_TOP)
		step = GOV_TOP;
	if (step > (int)GOV_STEPS - 1)
		step = GOV_STEPS - 1;
	if (step != gov_step) {
		gov_set(step);
		if (gov_said++ < 60) {
			puts_("\n[el2: temp ");
			hex((u64)big, 2);
			put('/');
			hex((u64)mid, 2);
			puts_(" C -> big ");
			hex(cl2_steps[step] / 1000, 4);
			puts_(" mid ");
			hex(cl1_steps[step] / 1000, 4);
			puts_(" MHz]\n");
		}
	}
}

static void
gov_start(void)
{
	u64 freq;
	if (acpm_open(ACPM_DVFS_CHANNEL, &gov_dvfs) || acpm_open(ACPM_TMU_CHANNEL, &gov_tmu)) {
		puts_("el2: governor: no ACPM channels\n");
		return;
	}
	int big = tmu_read(TZ_BIG), mid = tmu_read(TZ_MID);
	kv("el2: governor: big C", (u64)big);
	kv("el2: governor: mid C", (u64)mid);
	if (big < 0 || mid < 0 || big > 125 || mid > 125)
		return;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	gov_period = freq / 10;
	gov_set(5);				/* big 1826, middle 1491 MHz to start; up from there while cool */
	gov_on = 1;
}

struct el2_frame { u64 x[31]; u64 elr; };

/*
 * The touchscreen, for HackiOS's virtio-input driver (bsd/dev/redwood_virtio_input.c, boot-arg
 * virtio_input=1): it looks for virtio-mmio devices at 0x0a000000 (32 slots of 0x200, QEMU's place),
 * sets up one event queue of 64 eight-byte buffers and looks at it 125 times a second -- no interrupt.
 * Here slot 0 is a tablet: the four pages are trapped at stage 2 and answered below, and whenever the
 * monitor has the CPU (the kernel idles: trapped WFIs) it reads the Synaptics chip (touch.c) and
 * writes events into the queue: ABS_X, ABS_Y (0..0x7fff), BTN_TOUCH, SYN. The kernel reads the queue
 * with its caches on and this runs with the MMU off, so every line is cleaned and invalidated around.
 */
#define VIRTIO_BASE	0x0a000000ul
#define EV_SYN		0
#define EV_KEY		1
#define EV_ABS		3
#define ABS_X		0
#define ABS_Y		1
#define BTN_TOUCH	0x14a
#define PANEL_W		1440
#define PANEL_H		3120

static u64 s2_l3v[512] __attribute__((aligned(4096)));
/* EL2_IDLE: CNTHCTL_EL2.EVNTEN, on bit 9 of the counter (24.576 MHz): an event every ~42 us */
#define EVENT_STREAM	((1ul << 2) | (9ul << 4))
static int idle_on;
static int touch_on, touch_down, touch_logged;
static u64 touch_last, touch_period;
static u32 v_status, v_qsel, v_qready, v_featsel;
static u64 v_desc, v_avail, v_used;
static u16 v_last_avail, v_used_idx;
static u64 v_drops, t_reports, t_downs, t_ups, t_odd, t_bad, t_other, t_last_other, t_said, t_said_sum;

static void
dc_civac(u64 a, u64 n)
{
	for (u64 x = a & ~63ul; x < a + n; x += 64)
		__asm__ volatile("dc civac, %0" : : "r"(x) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static u32
virtio_mmio(u64 off, int write, u32 val)
{
	if (off >= 0x200)
		return 0;			/* slots 1-31: nothing there */
	if (write) {
		switch (off) {
		case 0x014: v_featsel = val; break;
		case 0x030: v_qsel = val; break;
		case 0x044: if (!v_qsel) v_qready = val; break;
		case 0x070:
			v_status = val;
			kv("\n[el2: virtio-input status", val);
			if (!val)
				v_qready = 0, v_last_avail = v_used_idx = 0;
			break;
		case 0x080: if (!v_qsel) v_desc = (v_desc & ~0xffffffffull) | val; break;
		case 0x084: if (!v_qsel) v_desc = (v_desc & 0xffffffffull) | (u64)val << 32; break;
		case 0x090: if (!v_qsel) v_avail = (v_avail & ~0xffffffffull) | val; break;
		case 0x094: if (!v_qsel) v_avail = (v_avail & 0xffffffffull) | (u64)val << 32; break;
		case 0x0a0: if (!v_qsel) v_used = (v_used & ~0xffffffffull) | val; break;
		case 0x0a4: if (!v_qsel) v_used = (v_used & 0xffffffffull) | (u64)val << 32; break;
		}
		return 0;
	}
	switch (off) {
	case 0x000: return 0x74726976;		/* "virt" */
	case 0x004: return 2;			/* virtio-mmio version 2 */
	case 0x008: return 18;			/* input */
	case 0x00c: return 0x554d4551;
	case 0x010: return v_featsel == 1 ? 1 : 0;	/* VIRTIO_F_VERSION_1 */
	case 0x034: return v_qsel ? 0 : 64;
	case 0x044: return v_qsel ? 0 : v_qready;
	case 0x070: return v_status;
	default: return 0;
	}
}

static void
vq_push(u16 type, u16 code, u32 value)
{
	if (!(v_status & 4) || !v_qready || !v_desc || !v_avail || !v_used)
		return;
	dc_civac(v_avail, 4 + 2 * 64);
	if (v_last_avail == *(volatile u16 *)(v_avail + 2)) {
		v_drops++;			/* no buffer given back yet: dropped */
		return;
	}
	u16 id = *(volatile u16 *)(v_avail + 4 + 2 * (v_last_avail % 64)) % 64;
	v_last_avail++;
	dc_civac(v_desc + id * 16, 16);
	u64 buf = *(volatile u64 *)(v_desc + id * 16);
	u64 slot = v_used + 4 + 8 * (v_used_idx % 64);
	dc_civac(buf, 8);
	dc_civac(slot, 8);
	*(volatile u32 *)buf = type | (u32)code << 16;
	*(volatile u32 *)(buf + 4) = value;
	*(volatile u32 *)slot = id;
	*(volatile u32 *)(slot + 4) = 8;
	__asm__ volatile("dsb sy" ::: "memory");
	v_used_idx++;
	dc_civac(v_used, 4);
	*(volatile u16 *)(v_used + 2) = v_used_idx;
	__asm__ volatile("dsb sy" ::: "memory");
	dc_civac(buf, 8);
	dc_civac(slot, 8);
	dc_civac(v_used, 4);
}

/* a touch report (0x11): 9 bytes, then 1, the number of fingers, 12 bytes each: index/class, X, Y (LE16 pixels), ... */
static void
touch_service(void)
{
	u64 now;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
	if (now - touch_last < touch_period)
		return;
	touch_last = now;
	if (!touch_irq())
		return;
	u8 code = 0, b[160];
	int n = tcm_read(&code, b, sizeof(b));
	if (n < 0)
		t_bad++;
	else if (code != TCM_TOUCH)
		t_other++, t_last_other = code;
	if (now - t_said > 10 * touch_period * 1000 && t_reports + t_bad + t_other != t_said_sum) {
		t_said = now, t_said_sum = t_reports + t_bad + t_other;
		puts_("\n[el2: touch reports ");
		hex(t_reports, 5);
		puts_(" downs ");
		hex(t_downs, 4);
		puts_(" ups ");
		hex(t_ups, 4);
		puts_(" not-finger ");
		hex(t_odd, 4);
		puts_(" bad ");
		hex(t_bad, 4);
		puts_(" other ");
		hex(t_other, 4);
		put('/');
		hex(t_last_other, 2);
		puts_(" dropped ");
		hex(v_drops, 4);
		puts_("]\n");
	}
	if (code != TCM_TOUCH || n < 11)
		return;
	t_reports++;
	/* the first finger (class 1, or 2: a glove) among the objects; the rest (palms, hovering, ...) are not touches */
	int f = -1;
	for (int i = 0; i < b[10] && 11 + 12 * i + 5 <= n; i++) {
		u8 cls = b[11 + 12 * i] >> 4;
		if (cls == 1 || cls == 2) {
			f = 11 + 12 * i;
			break;
		}
		if (t_odd++ < 6) {
			puts_("\n[el2: touch object ");
			hex(b[11 + 12 * i], 2);
			puts_("]\n");
		}
	}
	if (f >= 0) {
		u32 x = b[f + 1] | (u32)b[f + 2] << 8, y = b[f + 3] | (u32)b[f + 4] << 8;
		if (x >= PANEL_W)
			x = PANEL_W - 1;
		if (y >= PANEL_H)
			y = PANEL_H - 1;
		if (!touch_down) {
			t_downs++;
			if (touch_logged < 30) {
				touch_logged++;
				puts_("\n[el2: touch ");
				hex(x, 4);
				put(' ');
				hex(y, 4);
				puts_("]\n");
			}
		}
		vq_push(EV_ABS, ABS_X, x * 0x7fff / (PANEL_W - 1));
		vq_push(EV_ABS, ABS_Y, y * 0x7fff / (PANEL_H - 1));
		if (!touch_down)
			vq_push(EV_KEY, BTN_TOUCH, 1);
		vq_push(EV_SYN, 0, 0);
		touch_down = 1;
	} else if (touch_down) {
		t_ups++;
		vq_push(EV_KEY, BTN_TOUCH, 0);
		vq_push(EV_SYN, 0, 0);
		touch_down = 0;
	}
}

/* stage 2, 4K granule: level 1 has 1 GB blocks, level 2 the 2 MB blocks of the fake page's GB,
   level 3 the 4K pages of its 2 MB */
static u64 s2_l1[512] __attribute__((aligned(4096)));
static u64 s2_l2[512] __attribute__((aligned(4096)));
static u64 s2_l3[512] __attribute__((aligned(4096)));

#define S2_BLOCK	1ul
#define S2_TABLE	3ul
#define S2_PAGE		3ul
#define S2_AF		(1ul << 10)
#define S2_AP_RW	(3ul << 6)
#define S2_NORMAL	((0xful << 2) | (3ul << 8))	/* write-back, inner shareable */
#define S2_DEVICE	(0x1ul << 2)			/* Device-nGnRE */

#define HCR_VM		(1ul << 0)
#define HCR_TWI		(1ul << 13)
#define HCR_TSC		(1ul << 19)
#define HCR_RW		(1ul << 31)

#define PL011_DR	0x00
#define PL011_FR	0x18
#define PL011_FR_IDLE	0x90				/* TXFE | RXFE */

#define PSCI_CPU_ON_64	0xc4000003ul
#define PSCI_CPU_ON_32	0x84000003ul
#define MAX_CORES	8

static u64 uart_page;

/*
 * The run's end: the monitor feeds the watchdogs (on every character, and on every WFI with
 * EL2_TRAP_WFI) only until then, so that a kernel that idles along -- or hangs in an idle loop --
 * still ends in the watchdog's warm reset, which keeps the log. 0: no end.
 */
static u64 feed_until;

static void
feed(void)
{
	u64 now;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
	if (!feed_until || now < feed_until)
		wdt_pet();
}

/*
 * More cores: the kernel starts each with PSCI CPU_ON, and the firmware brings it up at EL2 -- the
 * highest level below it -- where the kernel would fail. So the monitor swaps the entry for
 * start.S's el2_secondary, keeping the kernel's entry and context here, and the new core sets up its
 * EL2 like the boot core's (el2_secondary_setup) before it goes on at EL1 where the kernel wanted it.
 */
static u64 saved_vtcr, saved_vttbr, saved_hcr;
static u64 cpu_entry[MAX_CORES], cpu_context[MAX_CORES];
extern char el2_vectors[], el2_secondary[];
static u64 psci_calls;

static u64
s2_attr(u64 pa)
{
	/* RAM is from 2 GB up on gs201; below it, the SoC's registers */
	return (pa >= 0x80000000ul ? S2_NORMAL : S2_DEVICE) | S2_AF | S2_AP_RW;
}

static void
monitor_start(u64 page, u64 flags, u64 secs)
{
	u64 gb = page >> 30, two_mb = page & ~0x1ffffful;

	if (secs) {
		u64 now, freq;
		__asm__ volatile("isb; mrs %0, cntvct_el0; mrs %1, cntfrq_el0" : "=r"(now), "=r"(freq));
		feed_until = now + secs * freq;
	}

	uart_page = page & ~0xffful;
	for (u64 i = 0; i < 512; i++) {
		s2_l1[i] = (i << 30) | s2_attr(i << 30) | S2_BLOCK;
		s2_l2[i] = (gb << 30) + (i << 21);
		s2_l2[i] |= s2_attr(s2_l2[i]) | S2_BLOCK;
		s2_l3[i] = two_mb + (i << 12);
		s2_l3[i] |= s2_attr(s2_l3[i]) | S2_PAGE;
	}
	s2_l3[(uart_page >> 12) & 511] = 0;			/* the fake serial port: every access faults here */
	s2_l2[(page >> 21) & 511] = (u64)s2_l3 | S2_TABLE;
	if (flags & EL2_TOUCH) {			/* the virtio slots' four pages (same GB, another 2 MB) */
		u64 v2mb = VIRTIO_BASE & ~0x1ffffful;
		for (u64 i = 0; i < 512; i++)
			s2_l3v[i] = (v2mb + (i << 12)) | s2_attr(v2mb) | S2_PAGE;
		for (u64 i = 0; i < 4; i++)
			s2_l3v[((VIRTIO_BASE >> 12) & 511) + i] = 0;
		s2_l2[(VIRTIO_BASE >> 21) & 511] = (u64)s2_l3v | S2_TABLE;
		u64 freq;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
		touch_period = freq / 1000;
		touch_on = 1;
	}
	s2_l1[gb] = (u64)s2_l2 | S2_TABLE;

	/* 39-bit IPAs from level 1, non-cacheable walks (the tables were written with the MMU off) */
	u64 mmfr0, ps;
	__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
	ps = mmfr0 & 7;
	u64 vtcr = (1ul << 31) | (ps << 16) | (1ul << 6) | 25;
	__asm__ volatile("msr vtcr_el2, %0; msr vttbr_el2, %1; isb; dsb ish; tlbi vmalls12e1; dsb ish; isb"
			 : : "r"(vtcr), "r"((u64)s2_l1) : "memory");
	if ((flags & EL2_IDLE) && (flags & EL2_TRAP_WFI)) {
		u64 v;
		__asm__ volatile("mrs %0, cnthctl_el2" : "=r"(v));
		__asm__ volatile("msr cnthctl_el2, %0; isb" : : "r"(v | EVENT_STREAM));
		idle_on = 1;
	}
	u64 hcr = HCR_RW | HCR_VM | HCR_TSC | ((flags & EL2_TRAP_WFI) ? HCR_TWI : 0);
	__asm__ volatile("msr hcr_el2, %0; isb" : : "r"(hcr));
	saved_vtcr = vtcr, saved_vttbr = (u64)s2_l1, saved_hcr = hcr;
	kv("el2: monitor on; fake serial port", uart_page);
	if (flags & EL2_GOVERN)
		gov_start();
}

static void
smc_forward(struct el2_frame *f)
{
	register u64 x0 __asm__("x0") = f->x[0], x1 __asm__("x1") = f->x[1], x2 __asm__("x2") = f->x[2],
		     x3 __asm__("x3") = f->x[3], x4 __asm__("x4") = f->x[4], x5 __asm__("x5") = f->x[5],
		     x6 __asm__("x6") = f->x[6], x7 __asm__("x7") = f->x[7];
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : "r"(x4), "r"(x5), "r"(x6), "r"(x7)
			 : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	f->x[0] = x0, f->x[1] = x1, f->x[2] = x2, f->x[3] = x3;
}

static void __attribute__((noreturn))
el2_stop(const char *why, u64 esr, struct el2_frame *f)
{
	u64 far;
	__asm__ volatile("mrs %0, far_el2" : "=r"(far));
	puts_("\n!! el2: ");
	puts_(why);
	puts_(" esr=");
	hex(esr, 8);
	puts_(" elr=");
	hex(f->elr, 16);
	puts_(" far=");
	hex(far, 16);
	puts_(" -- stopping\n");
	hang();
}

/*
 * A new core, at EL2 (start.S el2_secondary): the same EL2 as the boot core (start.S's EL2 -> EL1
 * setup, then the monitor's stage 2 and traps), the kernel's entry in ELR_EL2; returns the kernel's
 * context, which el2_secondary hands it in x0 by its eret.
 */
u64
el2_secondary_setup(u64 core)
{
	u64 v;
	__asm__ volatile("msr vbar_el2, %0" : : "r"((u64)el2_vectors));
	__asm__ volatile("msr hcr_el2, %0; isb" : : "r"(HCR_RW));
	__asm__ volatile("mrs %0, cnthctl_el2" : "=r"(v));
	__asm__ volatile("msr cnthctl_el2, %0; msr cntvoff_el2, xzr" : : "r"(v | 3 | (idle_on ? EVENT_STREAM : 0)));
	__asm__ volatile("msr cptr_el2, %0; msr hstr_el2, xzr" : : "r"(0x33fful));
	__asm__ volatile("mrs %0, midr_el1; msr vpidr_el2, %0" : "=&r"(v));
	__asm__ volatile("mrs %0, mpidr_el1; msr vmpidr_el2, %0" : "=&r"(v));
	__asm__ volatile("mrs %0, S3_4_C12_C9_5" : "=r"(v));	/* ICC_SRE_EL2: SRE, Enable */
	__asm__ volatile("msr S3_4_C12_C9_5, %0; isb" : : "r"(v | 9));
	__asm__ volatile("msr sctlr_el1, %0" : : "r"(0x30d00800ul));
	__asm__ volatile("msr vtcr_el2, %0; msr vttbr_el2, %1; isb; dsb ish; tlbi vmalls12e1; dsb ish; isb"
			 : : "r"(saved_vtcr), "r"(saved_vttbr) : "memory");
	__asm__ volatile("msr hcr_el2, %0; isb" : : "r"(saved_hcr));
	__asm__ volatile("msr spsr_el2, %0; msr elr_el2, %1" : : "r"(0x3c5ul), "r"(cpu_entry[core]));
	return cpu_context[core];
}

/*
 * One core at a time in the monitor: the log (board.c) is not safe against two writers -- with
 * cores 4-7 up, a lost update made the header say start > size and the whole log was thrown away.
 * With the MMU off at EL2 memory is Device memory, where exclusives (ldxr/stxr) need not work, so
 * this is Lamport's bakery: plain loads and stores, ordered by barriers.
 */
static volatile u32 bakery_choosing[MAX_CORES], bakery_number[MAX_CORES];

static u32
this_core(void)
{
	u64 mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	return (mpidr >> 8) & (MAX_CORES - 1);
}

static void
monitor_lock(u32 me)
{
	u32 max = 0;
	bakery_choosing[me] = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	for (u32 j = 0; j < MAX_CORES; j++)
		if (bakery_number[j] > max)
			max = bakery_number[j];
	bakery_number[me] = max + 1;
	__asm__ volatile("dsb sy" ::: "memory");
	bakery_choosing[me] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
	for (u32 j = 0; j < MAX_CORES; j++) {
		while (bakery_choosing[j])
			;
		while (bakery_number[j] &&
		       (bakery_number[j] < bakery_number[me] || (bakery_number[j] == bakery_number[me] && j < me)))
			;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
monitor_unlock(u32 me)
{
	__asm__ volatile("dsb sy" ::: "memory");
	bakery_number[me] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void el2_handle(u64 kind, struct el2_frame *f);

void
el2_exception(u64 kind, struct el2_frame *f)
{
	u32 me = this_core();
	u64 esr;
	__asm__ volatile("mrs %0, esr_el2" : "=r"(esr));
	monitor_lock(me);
	el2_handle(kind, f);
	if (touch_on)
		touch_service();
	if (gov_on)
		govern();
	monitor_unlock(me);
	/*
	 * The kernel idles: wait here, outside the lock, until an event -- the event stream's tick
	 * at the latest. (A WFI here would wait for an interrupt, and one meant for EL1 may not wake
	 * EL2; with the lock held it hung the kernel.) The kernel then looks again and idles again.
	 */
	if (idle_on && kind == 8 && (esr >> 26) == 0x01)
		__asm__ volatile("wfe");
}

static void
el2_handle(u64 kind, struct el2_frame *f)
{
	u64 esr;
	__asm__ volatile("mrs %0, esr_el2" : "=r"(esr));
	u32 ec = (esr >> 26) & 0x3f;

	if (kind != 8)				/* only synchronous exceptions from AArch64 EL1 are expected */
		el2_stop("unexpected exception", esr, f);

	switch (ec) {
	case 0x16:				/* HVC: from the program at EL1 */
		if (f->x[0] == EL2_FN_MONITOR) {
			monitor_start(f->x[1], f->x[2], f->x[3]);
			f->x[0] = 0;
		} else {
			f->x[0] = ~0ul;
		}
		return;				/* (ELR is past the HVC already) */

	case 0x24: {				/* a data abort at stage 2: the fake serial port */
		u64 hpfar, far;
		__asm__ volatile("mrs %0, hpfar_el2; mrs %1, far_el2" : "=r"(hpfar), "=r"(far));
		u64 ipa = ((hpfar >> 4) << 12) | (far & 0xfff);
		if (!(esr & (1u << 24)))
			el2_stop("stage-2 fault not decodable", esr, f);
		u32 srt = (esr >> 16) & 31, off = ipa & 0xfff;
		if (touch_on && ipa >= VIRTIO_BASE && ipa < VIRTIO_BASE + 0x4000) {
			int write = (esr >> 6) & 1;
			u32 r = virtio_mmio(ipa - VIRTIO_BASE, write, srt == 31 ? 0 : (u32)f->x[srt]);
			if (!write && srt != 31)
				f->x[srt] = r;
			f->elr += 4;
			return;
		}
		if ((ipa & ~0xffful) != uart_page)
			el2_stop("stage-2 fault outside the fake devices", esr, f);
		if (esr & (1u << 6)) {		/* a write */
			if (off == PL011_DR) {
				put((char)(srt == 31 ? 0 : f->x[srt]));
				feed();
			}
		} else if (srt != 31) {
			f->x[srt] = off == PL011_FR ? PL011_FR_IDLE : 0;
		}
		f->elr += 4;
		return;
	}

	case 0x17: {				/* SMC: PSCI */
		u64 fid = f->x[0] & 0xffffffff;
		if (psci_calls++ < 32) {
			puts_("\n[el2: psci ");
			hex(fid, 8);
			puts_("]\n");
		}
		if (fid == 0x84000008 || fid == 0x84000009 || fid == 0x84000012 || fid == 0xc4000012) {
			puts_("\n[el2: the kernel asked to power off or reset; holding until the watchdog's warm reset]\n");
			hang();
		}
		u64 core = (f->x[1] >> 8) & 0xff;	/* MPIDR Aff1: the core's number on gs201 */
		if ((fid == PSCI_CPU_ON_64 || fid == PSCI_CPU_ON_32) && saved_hcr && core < MAX_CORES) {
			cpu_entry[core] = f->x[2];
			cpu_context[core] = f->x[3];
			__asm__ volatile("dsb sy" ::: "memory");
			f->x[2] = (u64)el2_secondary;
			f->x[3] = core;
			smc_forward(f);
			puts_("[el2: cpu_on core ");
			hex(core, 1);
			puts_(" -> ");
			hex(f->x[0], 8);
			puts_("]\n");
		} else {
			smc_forward(f);
		}
		f->elr += 4;			/* (a trapped SMC returns to itself) */
		return;
	}

	case 0x01:				/* WFI (EL2_TRAP_WFI): an idle kernel is alive */
		/*
		 * Not slept here: a WFI at EL2 did not wake for the kernel's interrupts (routed to EL1),
		 * and the kernel stopped after its first line. Back at once: its idle loop spins instead.
		 */
		feed();
		f->elr += 4;
		return;

	default:
		el2_stop("unexpected trap", esr, f);
	}
}
