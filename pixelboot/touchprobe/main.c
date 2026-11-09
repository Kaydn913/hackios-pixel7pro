/*
 * touchprobe: is the touchscreen's chip powered when our code runs? (Synaptics TCM on spi@10D10000;
 * vdd = S2MPG12 LDO25, avdd = LDO26 -- behind the PMIC's firmware, hard to switch on ourselves.)
 * A powered chip answers a reset by pulling its interrupt line low (to report who it is), so:
 * log the pins, pulse reset (active low, 20 ms -- the device tree's reset-active-ms), then watch
 * the interrupt line (active low) for half a second. The pins (board overlay 12, fixups):
 *
 *   irq   gpa7-0   pinctrl@180E0000 bank 1   reset  gpp23-2  pinctrl@10C40000 bank 3
 *   tbn   gph1-0 (AP to AOC), gpa8-7 (AOC to AP): Google's switch of the touch bus to the AOC
 *
 * Exynos GPIO banks: 0x20 apart; CON (4 bits a pin: 0 input, 1 output, f interrupt) at +0, DAT +4,
 * PUD +8.
 */
#include "board.h"

#define GPA7	(0x180e0000ul + 1 * 0x20)
#define GPA8	(0x180e0000ul + 2 * 0x20)
#define GPH1	(0x11840000ul + 1 * 0x20)
#define GPP23	(0x10c40000ul + 3 * 0x20)
#define CON	0x0
#define DAT	0x4
#define PUD	0x8

static u64
ticks(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static void
delay_us(u64 us)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	for (u64 t = ticks(); ticks() - t < f / 1000000 * us;)
		;
}

static void
bank_log(const char *name, u64 b)
{
	puts_(name);
	puts_(": con ");
	hex(*reg(b + CON), 8);
	puts_(" dat ");
	hex(*reg(b + DAT), 2);
	puts_(" pud ");
	hex(*reg(b + PUD), 8);
	put('\n');
}

static void
pin_mode(u64 b, u32 pin, u32 mode)
{
	*reg(b + CON) = (*reg(b + CON) & ~(0xfu << (4 * pin))) | (mode << (4 * pin));
}

static void
pin_set(u64 b, u32 pin, u32 v)
{
	*reg(b + DAT) = (*reg(b + DAT) & ~(1u << pin)) | (v << pin);
}

static u32
pin_get(u64 b, u32 pin)
{
	return (*reg(b + DAT) >> pin) & 1;
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
	puts_("\ntouchprobe: is the touch chip powered?\n");
	bank_log("gpa7 (irq is pin 0)", GPA7);
	bank_log("gpp23 (reset is pin 2)", GPP23);
	bank_log("gph1 (tbn ap2aoc pin 0)", GPH1);
	bank_log("gpa8 (tbn aoc2ap pin 7)", GPA8);

	u32 irq_con = *reg(GPA7 + CON), rst_con = *reg(GPP23 + CON);
	pin_mode(GPA7, 0, 0);				/* irq: input, to read it */
	kv("touchprobe: irq level before reset", pin_get(GPA7, 0));

	pin_set(GPP23, 2, 1);
	pin_mode(GPP23, 2, 1);				/* reset: output, released */
	delay_us(1000);
	pin_set(GPP23, 2, 0);				/* assert (active low) */
	delay_us(20000);
	pin_set(GPP23, 2, 1);				/* release */
	u64 t0 = ticks(), f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	u32 last = pin_get(GPA7, 0), changes = 0;
	puts_("touchprobe: irq after reset (ms:level):");
	for (u32 ms = 0; ms < 500; ms++) {
		u32 v = pin_get(GPA7, 0);
		if (v != last && changes < 24) {
			put(' ');
			hex((ticks() - t0) * 1000 / f, 3);
			put(':');
			put('0' + (char)v);
			changes++;
		}
		last = v;
		delay_us(1000);
		wdt_pet();
	}
	put('\n');
	kv("touchprobe: irq level at the end", last);
	kv("touchprobe: changes", changes);
	*reg(GPA7 + CON) = irq_con;
	*reg(GPP23 + CON) = rst_con;
	puts_("touchprobe: done; letting the watchdog reset the phone\n");
	hang();
}
