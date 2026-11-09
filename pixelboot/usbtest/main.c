/*
 * usbtest: the USB fastboot device (common/usbdev.c) on its own. From the PC:
 *
 *   fastboot getvar product            -> pixelboot
 *   fastboot stage some.bin            -> into RAM at 0x900000000; its sum goes into the log
 *   fastboot oem done                  -> off the bus; the watchdog resets the phone (log kept)
 *
 * The sum is that of the file's little-endian 64-bit words (a tail of fewer than 8 bytes zero-padded),
 * mod 2^64: pixelboot/tools/sum64.py computes the same on the PC.
 */
#include "board.h"
#include "usbdev.h"

#define DEST		0x900000000ul
#define DEST_MAX	0xa0000000u

static u64
dest(u32 size)
{
	return size <= DEST_MAX ? DEST : 0;
}

static void
done(u64 at, u32 size)
{
	u64 sum = 0;
	volatile u64 *p = (volatile u64 *)at;
	u32 words = size / 8;
	for (u32 i = 0; i < words; i++) {
		sum += p[i];
		if ((i & 0xfffff) == 0)
			wdt_pet();
	}
	if (size & 7) {
		u64 tail = 0;
		volatile u8 *b = (volatile u8 *)(at + (u64)words * 8);
		for (u32 i = 0; i < (size & 7); i++)
			tail |= (u64)b[i] << (8 * i);
		sum += tail;
	}
	kv("usbtest: downloaded bytes", size);
	kv("usbtest: sum64", sum);
}

static int
command(const char *cmd, char *resp)
{
	const char *done_cmd = "oem done";
	int i = 0;
	while (done_cmd[i] && cmd[i] == done_cmd[i])
		i++;
	if (!done_cmd[i] && !cmd[i]) {
		resp[0] = 'O', resp[1] = 'K', resp[2] = 'A', resp[3] = 'Y', resp[4] = 0;
		return 1;
	}
	return 0;
}

static const struct fb_ops ops = { command, dest, done };

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
	puts_("\nusbtest: a fastboot device on the phone's USB port\n");
	int r = usb_fastboot(&ops, 120);
	kv("usbtest: usb_fastboot returned", (u64)r);
	puts_("usbtest: letting the watchdog reset the phone\n");
	hang();
}
