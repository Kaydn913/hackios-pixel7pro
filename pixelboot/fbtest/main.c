/*
 * fbtest's report, painted in colours that read the same in RGB and BGR: green, magenta, white, black.
 *
 *   band 1 (top 10%):    the EL we were entered at -- magenta: EL2, green: EL1, white: other
 *   band 2 (next 10%):   green once we run at EL1 (after the EL2 -> EL1 drop, when there was one)
 *   band 3, 4 (5% each): watchdog cl0, cl1 as the bootloader left it -- magenta: running, green: off
 *   the rest:            fills white from the top over RUN_SECS seconds
 *
 * The panel is a command-mode one (an S6E3HC4 on DSIM0): it shows the last frame it was sent, and the
 * bootloader leaves DECON0's trigger masked after its logo (TRIG_CON 0x3070), so nothing drawn in
 * the framebuffer reaches it. fbtest unmasks it the way the Pixel kernel's decon_reg_start() does
 * (cal_9845 decon_reg.c): a shadow update request, then HW_TRIG_EN on and HW_TRIG_MASK_DECON off.
 * From then on DECON0 sends a frame on every TE pulse of the panel. The log (board.c) records
 * DECON0's frame counter before and after, so the result is known even without watching.
 *
 * At the end it stops petting the watchdog: the warm reset keeps the log for Android to show.
 */
#include "board.h"

#ifndef FB_BASE
#define FB_BASE		0xfac00000ul	/* DPP channel 0's source address, as the bootloader leaves it */
#endif
#ifndef FB_SIZE
#define FB_SIZE		0x1123000ul	/* 1440 x 3120 x 4 */
#endif
#ifndef RUN_SECS
#define RUN_SECS	60
#endif

#define GREEN	0xff00ff00u
#define MAGENTA	0xffff00ffu
#define WHITE	0xffffffffu
#define BLACK	0xff000000u

/* DECON0 (cal_9845 regs-decon.h; gs201 is cal_9855, which keeps these) */
#define DECON0			0x1c240000ul
#define FRAME_COUNT		0x0004
#define GLOBAL_CON		0x0020
#define TRIG_CON		0x0030
#define HW_TRIG_MASK_DECON	(1u << 4)
#define HW_TRIG_EN		(1u << 0)
#define SHD_REG_UP_REQ		0x0050
#define SHD_REG_UP_REQ_GLOBAL	(1u << 31)
#define SHD_REG_UP_REQ_CMP	(1u << 20)
#define SHD_REG_UP_REQ_WIN0	(1u << 0)

static const u64 wdt_wtcon[] = { 0x10060000, 0x10070000 };

/* the MMU is off, so this is Device memory: aligned 8-byte stores only, nothing the compiler may merge */
static void
fill(u64 from_permille, u64 to_permille, u32 colour)
{
	volatile u64 *p = (volatile u64 *)(FB_BASE + (FB_SIZE * from_permille / 1000 & ~7ul));
	volatile u64 *end = (volatile u64 *)(FB_BASE + (FB_SIZE * to_permille / 1000 & ~7ul));
	u64 two = (u64)colour << 32 | colour;

	while (p < end)
		*p++ = two;
}

static u64
counter(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static void
decon_log(const char *when)
{
	puts_(when);
	puts_(": frame_count=");
	hex(*reg(DECON0 + FRAME_COUNT), 8);
	puts_(" global_con=");
	hex(*reg(DECON0 + GLOBAL_CON), 8);
	puts_(" trig_con=");
	hex(*reg(DECON0 + TRIG_CON), 8);
	put('\n');
}

static void
decon_unmask(void)
{
	*reg(DECON0 + SHD_REG_UP_REQ) |= SHD_REG_UP_REQ_GLOBAL | SHD_REG_UP_REQ_CMP | SHD_REG_UP_REQ_WIN0;
	u32 trig = *reg(DECON0 + TRIG_CON);
	*reg(DECON0 + TRIG_CON) = (trig | HW_TRIG_EN) & ~HW_TRIG_MASK_DECON;
}

void
at_entry(u64 el, u64 dtb)
{
	(void)dtb;
	log_open();
	puts_("\npixelboot fbtest: entered at EL");
	put('0' + (char)el);
	put('\n');
	fill(0, 1000, BLACK);
	fill(0, 100, el == 2 ? MAGENTA : el == 1 ? GREEN : WHITE);
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	(void)entry_el;
	(void)dtb;
	fill(100, 200, el == 1 ? GREEN : WHITE);
	for (unsigned i = 0; i < 2; i++)
		fill(200 + 50 * i, 250 + 50 * i, *reg(wdt_wtcon[i]) & (1u << 5) ? MAGENTA : GREEN);
	wdt_pet();

	decon_log("decon0 before");
	decon_unmask();
	decon_log("decon0 after unmask");

	u64 freq, start = counter(), shown = 300, logged = 0;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	for (;;) {
		wdt_pet();
		u64 secs = (counter() - start) / freq;
		u64 want = 300 + secs * 700 / RUN_SECS;
		if (want > 1000)
			break;
		if (want > shown) {
			fill(shown, want, WHITE);
			shown = want;
		}
		if (secs >= logged + 10) {
			logged = secs;
			decon_log("decon0 +10 s");
		}
	}
	decon_log("decon0 at the end");
	puts_("fbtest: done; letting the watchdog reset the phone\n");
	hang();
}
