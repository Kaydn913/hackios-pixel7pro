/*
 * A fastboot device on the phone's own USB port (usbdev.c): the PC's `fastboot` drives it.
 *
 *   command(cmd, resp): any command but download:, with resp to fill ("OKAY...", "FAIL..."); return 1
 *                       to end usb_fastboot() once the reply is out
 *   download_dest(size): where a download: of that size goes (0: refuse it)
 *   download_done(dest, size): it has all arrived
 */
struct fb_ops {
	int (*command)(const char *cmd, char *resp);
	u64 (*download_dest)(u32 size);
	void (*download_done)(u64 dest, u32 size);
};

/* runs until a command says so (1) or nothing is heard for idle_secs (0) */
int usb_fastboot(const struct fb_ops *ops, u32 idle_secs);
