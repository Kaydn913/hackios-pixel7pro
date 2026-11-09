/*
 * probe: write down what the bootloader left behind -- where we were entered, the watchdogs, the
 * display pipeline's registers (DECON0, the DPP DMA channels, DSIM0) and the bootloader's own log --
 * into the log (board.c), then let the watchdog reset the phone so Android can show it.
 *
 * First run's findings (2026-10-23): entered at EL2 at 0x80000000 with the DTB at 0xb4000000; DPP
 * channel 0 scans 1440x3120 from 0xfac00000; DECON0 is in command mode with its trigger masked
 * (TRIG_CON 0x3070); the display SysMMUs' registers abort on read, so they are left out.
 */
#include "board.h"

#define BLDR_LOG	0xfd800000ul
#define BLDR_LOG_SIZE	0x100000ul

/* the bootloader's log, as text: printable bytes and newlines, runs of anything else dropped */
static void
copy_bldr_log(void)
{
	puts_("== bootloader log @0xfd800000\n");
	volatile u8 *p = (volatile u8 *)BLDR_LOG;
	int gap = 0;
	for (u64 i = 0; i < BLDR_LOG_SIZE; i++) {
		u8 c = p[i];
		if (c == '\n' || (c >= 32 && c < 127)) {
			if (gap > 64)
				puts_("\n[...]\n");
			gap = 0;
			put((char)c);
		} else {
			gap++;
		}
		if ((i & 0xffff) == 0)
			wdt_pet();
	}
	puts_("\n== end of bootloader log\n");
}

static u64 entry_el_seen, dtb_seen;

void
at_entry(u64 el, u64 dtb)
{
	entry_el_seen = el;
	dtb_seen = dtb;
	log_open();
	puts_("\npixelboot probe: hello from the bare Pixel 7 Pro\n");
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	u64 v;
	(void)entry_el;
	(void)dtb;
	wdt_pet();
	kv("entry_el", entry_el_seen);
	kv("now_el", el);
	kv("dtb", dtb_seen);
	__asm__ volatile("adr %0, _start" : "=r"(v));
	kv("image_base", v);
	__asm__ volatile("mrs %0, midr_el1" : "=r"(v));
	kv("midr", v);
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
	kv("mpidr", v);
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	kv("cntfrq", v);

	wdt_log();
#ifdef PROBE_DISPLAY
	dump("decon0 main", 0x1c240000, 0x400);
	dump("decon0 win", 0x1c250000, 0x200);
	dump("decon0 sub", 0x1c260000, 0x100);
	dump("decon0 wincon", 0x1c270000, 0x200);
	static const u64 dma[] = { 0x1c0b0000, 0x1c0b1000, 0x1c0b2000, 0x1c0b3000, 0x1c0b4000, 0x1c0b5000,
				   0x1c0b8000, 0x1c0b9000, 0x1c0bc000 };
	for (unsigned i = 0; i < sizeof(dma) / sizeof(dma[0]); i++)
		dump("dpp dma", dma[i], 0x100);
	dump("dsim0", 0x1c2c0000, 0x300);
#endif
	/*
	 * USB as the bootloader's fastboot leaves it: is the DWC3 (usb@11210000, "synopsys,dwc3",
	 * peripheral) still powered and clocked? GSNPSID at +0xc120 names it when it is.
	 */
	kv("dwc3 gsnpsid", *reg(0x11210000 + 0xc120));
	dump("dwc3 global", 0x11210000 + 0xc100, 0x100);
	dump("dwc3 phycfg", 0x11210000 + 0xc200, 0x100);
	dump("dwc3 events", 0x11210000 + 0xc400, 0x20);
	dump("dwc3 device", 0x11210000 + 0xc700, 0x40);
	dump("dwc3 ep0/1 cmd", 0x11210000 + 0xc800, 0x40);
	dump("usbdrd phy", 0x11200000, 0x100);
	dump("usb31 link/phy ctrl", 0x110f0000, 0x100);
	copy_bldr_log();
	puts_("probe: done; letting the watchdog reset the phone (warm, so this report survives)\n");
	hang();
}
