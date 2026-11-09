/*
 * The phone's USB port as a fastboot device, so that the PC can send what does not fit a boot image
 * (iOS's 2.5 GB system image) straight into RAM while our program runs.
 *
 * The controller is a Synopsys DWC_usb31 1.10a (usb@11210000, GSNPSID 0x33313130). The bootloader's
 * fastboot leaves it powered, clocked, in device mode and halted (probe, 2026-10-23), with the PHY
 * set up -- so this is the device side only, after the programming model of Linux's dwc3 driver:
 * a core soft reset (the PHY settings kept), an event ring that is polled, endpoint 0's control
 * transfers (enumeration), and two bulk endpoints carrying the fastboot protocol. It runs at high
 * speed (480 Mbit/s). It is the same device to the PC as the bootloader's fastboot (Google 18d1:4ee0,
 * the phone's serial), so Windows takes the driver it already has.
 *
 * Memory: the MMU is off, so everything is Device memory -- uncached, and what the controller writes
 * by DMA is seen as it is. Addresses are physical (the bootloader's event ring sat at 0xf8cec000).
 */
#include "board.h"
#include "usbdev.h"

#define DWC3			0x11210000ul
#define GSBUSCFG0		0xc100
#define GSBUSCFG1		0xc104
#define GCTL			0xc110
#define GUSB2PHYCFG		0xc200
#define GUSB3PIPECTL		0xc2c0
#define GEVNTADRLO		0xc400
#define GEVNTADRHI		0xc404
#define GEVNTSIZ		0xc408
#define GEVNTCOUNT		0xc40c
#define DCFG			0xc700
#define DCTL			0xc704
#define DEVTEN			0xc708
#define DSTS			0xc70c
#define DALEPENA		0xc720
#define DEPCMDPAR2(n)		(0xc800 + (n) * 0x10)
#define DEPCMDPAR1(n)		(0xc804 + (n) * 0x10)
#define DEPCMDPAR0(n)		(0xc808 + (n) * 0x10)
#define DEPCMD(n)		(0xc80c + (n) * 0x10)

#define DCTL_RUN_STOP		(1u << 31)
#define DCTL_CSFTRST		(1u << 30)
#define DSTS_DEVCTRLHLT		(1u << 22)
#define DCFG_SPEED_HS		0
#define DEVTEN_DISCONN		(1u << 0)
#define DEVTEN_USBRST		(1u << 1)
#define DEVTEN_CONNDONE		(1u << 2)

#define CMD_SETEPCONFIG		1
#define CMD_SETTRANSFRESOURCE	2
#define CMD_SETSTALL		4
#define CMD_CLEARSTALL		5
#define CMD_STARTTRANSFER	6
#define CMD_DEPSTARTCFG		9
#define CMD_ACT			(1u << 10)
#define CMD_PARAM(x)		((x) << 16)

/* physical endpoints: number * 2 + direction */
#define EP0_OUT			0
#define EP0_IN			1
#define EP1_OUT			2
#define EP1_IN			3

struct trb { u32 bpl, bph, size, ctrl; };
#define TRB_HWO			(1u << 0)
#define TRB_LST			(1u << 1)
#define TRB_IOC			(1u << 11)
#define TRB_ISP_IMI		(1u << 10)
#define TRBCTL_NORMAL		(1u << 4)
#define TRBCTL_SETUP		(2u << 4)
#define TRBCTL_STATUS2		(3u << 4)
#define TRBCTL_STATUS3		(4u << 4)
#define TRBCTL_DATA		(5u << 4)

#define EVBUF_SIZE		4096
static u32 evbuf[EVBUF_SIZE / 4] __attribute__((aligned(4096)));
static struct trb ep_trb[4] __attribute__((aligned(64)));	/* one in flight per physical endpoint */
static u8 setup_buf[8] __attribute__((aligned(64)));
static u8 ep0_buf[512] __attribute__((aligned(64)));
static u8 cmd_buf[512] __attribute__((aligned(64)));
static u8 resp_buf[64] __attribute__((aligned(64)));
static u32 evpos;

static u32 rd(u32 off) { return *reg(DWC3 + off); }
static void wr(u32 off, u32 v) { *reg(DWC3 + off) = v; }

static void
copy(u8 *d, const u8 *s, u32 n)
{
	while (n--)
		*d++ = *s++;
}

static u64
now_ticks(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static u64
ticks_per_sec(void)
{
	u64 v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static int
ep_cmd(u32 ep, u32 cmd, u32 p0, u32 p1, u32 p2)
{
	wr(DEPCMDPAR0(ep), p0);
	wr(DEPCMDPAR1(ep), p1);
	wr(DEPCMDPAR2(ep), p2);
	wr(DEPCMD(ep), cmd | CMD_ACT);
	for (u32 i = 0; i < 1000000; i++) {
		u32 v = rd(DEPCMD(ep));
		if (!(v & CMD_ACT))
			return (v >> 12) & 0xf;
	}
	puts_("usb: endpoint command timed out\n");
	return -1;
}

static void
start_trb(u32 ep, u64 buf, u32 len, u32 ctrl)
{
	struct trb *t = &ep_trb[ep];
	t->bpl = (u32)buf;
	t->bph = (u32)(buf >> 32);
	t->size = len;
	t->ctrl = ctrl | TRB_LST | TRB_IOC | TRB_HWO;
	__asm__ volatile("dsb sy" ::: "memory");
	int st = ep_cmd(ep, CMD_STARTTRANSFER, (u32)((u64)t >> 32), (u32)(u64)t, 0);
	if (st) {
		puts_("usb: start transfer on ep ");
		hex(ep, 1);
		puts_(" failed, status ");
		hex((u64)st, 2);
		put('\n');
	}
}

/* what is left of a finished transfer's buffer (OUT: what did not arrive) */
static u32
trb_left(u32 ep)
{
	return ep_trb[ep].size & 0xffffff;
}

/* ---- descriptors: the bootloader's fastboot device, as Windows already knows it ---- */

static const u8 dev_desc[18] = {
	18, 1, 0x00, 0x02, 0, 0, 0, 64,		/* USB 2.0, control packets of 64 */
	0xd1, 0x18, 0xe0, 0x4e, 0x00, 0x01,	/* Google 18d1:4ee0 (fastboot), 1.00 */
	1, 2, 3, 1,
};

static const u8 cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 250,	/* one interface, bus powered, 500 mA */
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,	/* fastboot: class ff, subclass 42, protocol 3 */
	7, 5, 0x81, 2, 0x00, 0x02, 0,		/* bulk IN 1, 512 */
	7, 5, 0x01, 2, 0x00, 0x02, 0,		/* bulk OUT 1, 512 */
};

/*
 * The phone's own serial number (`fastboot devices`), so that the PC sees our device where it saw the
 * bootloader's fastboot and tools/run.sh finds it with `fastboot -s`: build with DEFINES=PHONE_SERIAL=...
 * (tools/ios-run.sh does, from tools/serial.sh).
 */
#ifndef PHONE_SERIAL
#define PHONE_SERIAL pixelboot
#endif
#define SERIAL_STR2(x) #x
#define SERIAL_STR(x) SERIAL_STR2(x)

static const char *const strings[] = { 0, "Google", "pixelboot", SERIAL_STR(PHONE_SERIAL), "fastboot" };

/*
 * Microsoft OS 1.0 descriptors, as the bootloader's fastboot has them: Windows remembers per
 * 18d1:4ee0 1.00 that the device has them with vendor code 0x47 (usbflags\18D14EE00100 osvc 01 47)
 * and asks for them at every connection without looking at string 0xee again. Unanswered, it took
 * the device's WinUSB binding and Android interface GUID away, and then fastboot no longer saw the
 * bootloader either (2026-10-30: the record had to be removed by hand). So: string 0xee
 * ("MSFT100", vendor code 0x47); vendor request 0x47, index 4, the extended compat ID WINUSB for
 * interface 0; index 5, the extended property DeviceInterfaceGUIDs = Android's USB class GUID,
 * {F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}, which fastboot.exe looks devices up by.
 */
#define MS_VENDOR_CODE	0x47

static const u8 ms_os_string[18] = {
	18, 3, 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0, MS_VENDOR_CODE, 0,
};

static const u8 ms_compat_id[40] = {
	40, 0, 0, 0, 0x00, 0x01, 0x04, 0x00, 1, 0, 0, 0, 0, 0, 0, 0,	/* length, 1.00, index 4, one function */
	0, 1, 'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0,	/* interface 0: WINUSB */
	0, 0, 0, 0, 0, 0, 0, 0,
};

/* the extended properties descriptor (146 bytes), built at its first use */
static u32
ms_properties(u8 *out)
{
	static const char name[] = "DeviceInterfaceGUIDs", guid[] = "{F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}";
	u32 name_bytes = 2 * (sizeof(name)), data_bytes = 2 * (sizeof(guid) + 1), n = 10, size;

	size = 4 + 4 + 2 + name_bytes + 4 + data_bytes;
	out[n++] = (u8)size, out[n++] = (u8)(size >> 8), out[n++] = 0, out[n++] = 0;
	out[n++] = 7, out[n++] = 0, out[n++] = 0, out[n++] = 0;	/* REG_MULTI_SZ */
	out[n++] = (u8)name_bytes, out[n++] = 0;
	for (u32 i = 0; i < sizeof(name); i++)
		out[n++] = (u8)name[i], out[n++] = 0;
	out[n++] = (u8)data_bytes, out[n++] = 0, out[n++] = 0, out[n++] = 0;
	for (u32 i = 0; i < sizeof(guid); i++)
		out[n++] = (u8)guid[i], out[n++] = 0;
	out[n++] = 0, out[n++] = 0;					/* the multi-string's second NUL */
	out[0] = (u8)n, out[1] = (u8)(n >> 8), out[2] = 0, out[3] = 0;	/* header: length, 1.00, index 5, one */
	out[4] = 0x00, out[5] = 0x01, out[6] = 0x05, out[7] = 0x00, out[8] = 1, out[9] = 0;
	return n;
}

static u32
string_desc(u32 index, u8 *out)
{
	if (index == 0) {
		out[0] = 4, out[1] = 3, out[2] = 0x09, out[3] = 0x04;	/* English (US) */
		return 4;
	}
	if (index >= sizeof(strings) / sizeof(strings[0]))
		return 0;
	u32 n = 2;
	for (const char *s = strings[index]; *s && n < 62; s++) {
		out[n++] = (u8)*s;
		out[n++] = 0;
	}
	out[0] = (u8)n, out[1] = 3;
	return n;
}

/* ---- endpoint 0 ---- */

enum { EP0_IDLE, EP0_SETUP, EP0_DATA, EP0_STATUS };
static int ep0_state, ep0_in, ep0_three;
static u32 ep0_len;
static int configured;

static void
ep0_start_setup(void)
{
	ep0_state = EP0_SETUP;
	start_trb(EP0_OUT, (u64)setup_buf, 8, TRBCTL_SETUP);
}

static void
ep0_stall(void)
{
	ep_cmd(EP0_OUT, CMD_SETSTALL, 0, 0, 0);
	ep0_start_setup();
}

static void
bulk_out_arm(u64 buf, u32 len)
{
	/* (a short packet ends it: commands are, and so is a download's last packet, mostly) */
	start_trb(EP1_OUT, buf, len, TRBCTL_NORMAL | TRB_ISP_IMI);
}

static void
configure_bulk(void)
{
	/* a new configuration: transfer resources from 2 up (databook: DEPSTARTCFG, XferRscIdx 2) */
	ep_cmd(EP0_OUT, CMD_DEPSTARTCFG | CMD_PARAM(2), 0, 0, 0);
	/* bulk (type 2), 512; IN in FIFO 1; events: transfer complete; physical number in PAR1 */
	ep_cmd(EP1_OUT, CMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((u32)EP1_OUT << 25), 0);
	ep_cmd(EP1_OUT, CMD_SETTRANSFRESOURCE, 1, 0, 0);
	ep_cmd(EP1_IN, CMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((u32)EP1_IN << 25), 0);
	ep_cmd(EP1_IN, CMD_SETTRANSFRESOURCE, 1, 0, 0);
	wr(DALEPENA, rd(DALEPENA) | (1u << EP1_OUT) | (1u << EP1_IN));
	configured = 1;
	bulk_out_arm((u64)cmd_buf, sizeof(cmd_buf));
	puts_("usb: configured\n");
}

static void
ep0_setup(void)
{
	u8 type = setup_buf[0], req = setup_buf[1];
	u32 value = setup_buf[2] | setup_buf[3] << 8, index = setup_buf[4] | setup_buf[5] << 8;
	u32 length = setup_buf[6] | setup_buf[7] << 8, n = 0;
	int ok = 1;

	ep0_in = (type & 0x80) != 0;
	if (type == 0x80 && req == 6) {			/* GET_DESCRIPTOR */
		u32 kind = value >> 8;
		if (kind == 1) {
			copy(ep0_buf, dev_desc, n = sizeof(dev_desc));
		} else if (kind == 2) {
			copy(ep0_buf, cfg_desc, n = sizeof(cfg_desc));
		} else if (kind == 3 && (value & 0xff) == 0xee) {
			copy(ep0_buf, ms_os_string, n = sizeof(ms_os_string));
		} else if (kind == 3) {
			n = string_desc(value & 0xff, ep0_buf);
			ok = n != 0;
		} else {
			ok = 0;				/* device qualifier, BOS: not here */
		}
	} else if ((type == 0xc0 || type == 0xc1) && req == MS_VENDOR_CODE && index == 4) {
		copy(ep0_buf, ms_compat_id, n = sizeof(ms_compat_id));
	} else if ((type == 0xc0 || type == 0xc1) && req == MS_VENDOR_CODE && index == 5) {
		n = ms_properties(ep0_buf);
	} else if (type == 0x00 && req == 5) {		/* SET_ADDRESS */
		wr(DCFG, (rd(DCFG) & ~(0x7fu << 3)) | ((value & 0x7f) << 3));
		puts_("usb: address ");
		hex(value & 0x7f, 2);
		put('\n');
	} else if (type == 0x00 && req == 9) {		/* SET_CONFIGURATION */
		if (value == 1 && !configured)
			configure_bulk();
	} else if (type == 0x80 && req == 8) {		/* GET_CONFIGURATION */
		ep0_buf[0] = (u8)configured, n = 1;
	} else if ((type & 0x9c) == 0x80 && req == 0) {	/* GET_STATUS */
		ep0_buf[0] = ep0_buf[1] = 0, n = 2;
	} else if ((type == 0x02 && (req == 1 || req == 3)) || (type == 0x01 && req == 11) || (type == 0x00 && (req == 1 || req == 3))) {
		/* CLEAR/SET_FEATURE, SET_INTERFACE: nothing to do (a cleared halt: the endpoint is fine) */
		(void)index;
	} else {
		ok = 0;
	}
	if (!ok) {
		ep0_stall();
		return;
	}
	if (n > length)
		n = length;
	ep0_len = n;
	ep0_three = length != 0;
	ep0_state = ep0_three ? EP0_DATA : EP0_STATUS;
	/* the data and status stages start on the controller's XferNotReady (ep0_not_ready) */
}

static void
ep0_not_ready(u32 ep, u32 status)
{
	if (ep0_state == EP0_DATA && status == 1 && (ep == EP0_IN) == ep0_in)
		start_trb(ep, (u64)ep0_buf, ep0_in ? ep0_len : 64, TRBCTL_DATA);
	else if (ep0_state == EP0_STATUS && status == 2)
		start_trb(ep, (u64)ep0_buf, 0, ep0_three ? TRBCTL_STATUS3 : TRBCTL_STATUS2);
}

static void
ep0_complete(u32 ep)
{
	(void)ep;
	if (ep0_state == EP0_SETUP)
		ep0_setup();
	else if (ep0_state == EP0_DATA)
		ep0_state = EP0_STATUS;
	else if (ep0_state == EP0_STATUS)
		ep0_start_setup();
}

/* ---- fastboot on the bulk endpoints ---- */

static const struct fb_ops *fb;
static int in_busy, fb_done, fb_finish;
static u64 dl_dest;
static u32 dl_size, dl_got, dl_chunk;
static u64 last_heard;

#define DL_CHUNK_MAX		(8u << 20)		/* a TRB takes at most 16 MB - 1 */

/* a reply waiting for the one before it to go out (replies come from event handling: no waiting there) */
static char pending[65];
static int have_pending;

static void
send_now(const char *s)
{
	u32 n = 0;
	while (s[n] && n < sizeof(resp_buf))
		resp_buf[n] = (u8)s[n], n++;
	in_busy = 1;
	start_trb(EP1_IN, (u64)resp_buf, n, TRBCTL_NORMAL);
}

static void
respond(const char *s)
{
	if (!in_busy) {
		send_now(s);
		return;
	}
	u32 n = 0;
	while (s[n] && n < 64)
		pending[n] = s[n], n++;
	pending[n] = 0;
	have_pending = 1;
}

static void
hex8(char *out, u32 v)
{
	for (int i = 7; i >= 0; i--, v >>= 4)
		out[i] = "0123456789abcdef"[v & 0xf];
}

static int
starts(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

static u32
parse_hex(const char *s)
{
	u32 v = 0;
	for (; *s; s++) {
		u32 d = *s >= '0' && *s <= '9' ? (u32)(*s - '0') : *s >= 'a' && *s <= 'f' ? (u32)(*s - 'a' + 10) :
			*s >= 'A' && *s <= 'F' ? (u32)(*s - 'A' + 10) : 16;
		if (d == 16)
			break;
		v = v * 16 + d;
	}
	return v;
}

static void
dl_next(void)
{
	u32 left = dl_size - dl_got;
	dl_chunk = left > DL_CHUNK_MAX ? DL_CHUNK_MAX : (left + 511) & ~511u;
	bulk_out_arm(dl_dest + dl_got, dl_chunk);
}

static void
fb_command(u32 len)
{
	char cmd[65], resp[65];
	if (len > 64)
		len = 64;
	for (u32 i = 0; i < len; i++)
		cmd[i] = (char)cmd_buf[i];
	cmd[len] = 0;
	puts_("usb: fastboot: ");
	puts_(cmd);
	put('\n');

	if (starts(cmd, "download:")) {
		dl_size = parse_hex(cmd + 9);
		dl_dest = dl_size ? fb->download_dest(dl_size) : 0;
		if (!dl_dest) {
			dl_size = 0;
			respond("FAILno room for that");
			bulk_out_arm((u64)cmd_buf, sizeof(cmd_buf));
			return;
		}
		dl_got = 0;
		dl_next();				/* the data's first TRB is out before the host may send */
		char r[13] = "DATA";
		hex8(r + 4, dl_size);
		r[12] = 0;
		respond(r);
		return;
	}
	if (starts(cmd, "getvar:max-download-size")) {
		respond("OKAY0xa0000000");		/* 2.5 GB: the system image in one go */
	} else if (starts(cmd, "getvar:product")) {
		respond("OKAYpixelboot");
	} else if (starts(cmd, "getvar:serialno")) {
		respond("OKAY" SERIAL_STR(PHONE_SERIAL));
	} else if (starts(cmd, "getvar:is-userspace")) {
		respond("OKAYno");
	} else {
		resp[0] = 0;
		fb_finish = fb->command(cmd, resp);
		respond(resp[0] ? resp : "FAILunknown command");
	}
	if (!fb_finish)
		bulk_out_arm((u64)cmd_buf, sizeof(cmd_buf));
}

static void
bulk_out_complete(void)
{
	if (!dl_size) {
		fb_command(sizeof(cmd_buf) - trb_left(EP1_OUT));
		return;
	}
	u32 got = dl_chunk - trb_left(EP1_OUT);
	dl_got += got;
	if ((dl_got & ((256u << 20) - 1)) < got) {
		puts_("usb: downloaded ");
		hex(dl_got >> 20, 4);
		puts_(" MB (hex)\n");
	}
	if (dl_got < dl_size && got == dl_chunk) {
		dl_next();
		return;
	}
	fb->download_done(dl_dest, dl_got);
	respond(dl_got >= dl_size ? "OKAY" : "FAILthe download came up short");
	dl_size = 0;
	bulk_out_arm((u64)cmd_buf, sizeof(cmd_buf));
}

/* ---- events ---- */

static void
device_event(u32 ev)
{
	u32 type = (ev >> 8) & 0xf;
	if (type == 1) {				/* USB reset */
		wr(DCFG, rd(DCFG) & ~(0x7fu << 3));
		puts_("usb: reset\n");
	} else if (type == 2) {				/* connect done */
		u32 speed = rd(DSTS) & 7;
		puts_("usb: connected, speed ");
		hex(speed, 1);
		put('\n');
	} else if (type == 0) {
		puts_("usb: disconnected\n");
		configured = 0;
	}
}

static void
endpoint_event(u32 ev)
{
	u32 ep = (ev >> 1) & 0x1f, type = (ev >> 6) & 0xf, status = (ev >> 12) & 0xf;
	if (ep <= EP0_IN) {
		if (type == 1)
			ep0_complete(ep);
		else if (type == 3)
			ep0_not_ready(ep, status & 3);
		return;
	}
	if (type != 1)
		return;
	last_heard = now_ticks();
	if (ep == EP1_IN) {
		in_busy = 0;
		if (have_pending) {
			have_pending = 0;
			send_now(pending);
		} else if (fb_finish) {
			fb_done = 1;
		}
	} else if (ep == EP1_OUT) {
		bulk_out_complete();
	}
}

static void
poll_events(void)
{
	u32 count = rd(GEVNTCOUNT) & 0xffff;
	while (count >= 4) {
		u32 ev = evbuf[evpos / 4];
		evpos = (evpos + 4) % EVBUF_SIZE;
		wr(GEVNTCOUNT, 4);
		count -= 4;
		if (ev & 1) {
			if (((ev >> 1) & 0x7f) == 0)
				device_event(ev);
		} else {
			endpoint_event(ev);
		}
	}
}

/* ---- bring-up ---- */

static int
controller_start(void)
{
	/* a soft reset of the device core, keeping what the bootloader set up for the PHY and the bus */
	u32 phy2 = rd(GUSB2PHYCFG), phy3 = rd(GUSB3PIPECTL), bus0 = rd(GSBUSCFG0), bus1 = rd(GSBUSCFG1), gctl = rd(GCTL);
	wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);
	for (u32 i = 0; i < 1000000 && !(rd(DSTS) & DSTS_DEVCTRLHLT); i++)
		;
	wr(DCTL, DCTL_CSFTRST);
	u32 i;
	for (i = 0; i < 10000000 && (rd(DCTL) & DCTL_CSFTRST); i++)
		;
	if (rd(DCTL) & DCTL_CSFTRST) {
		puts_("usb: soft reset did not finish\n");
		return 0;
	}
	for (u64 t = now_ticks(); now_ticks() - t < ticks_per_sec() / 20;)	/* 50 ms to settle (as Linux) */
		;
	wr(GUSB2PHYCFG, phy2);
	wr(GUSB3PIPECTL, phy3);
	wr(GSBUSCFG0, bus0);
	wr(GSBUSCFG1, bus1);
	wr(GCTL, gctl);

	evpos = 0;
	wr(GEVNTADRLO, (u32)(u64)evbuf);
	wr(GEVNTADRHI, (u32)((u64)evbuf >> 32));
	wr(GEVNTSIZ, EVBUF_SIZE | (1u << 31));		/* (polled: no interrupt) */
	wr(GEVNTCOUNT, rd(GEVNTCOUNT) & 0xffff);	/* (written counts are subtracted) */

	wr(DCFG, (rd(DCFG) & ~(7u | (0x7fu << 3))) | DCFG_SPEED_HS);
	wr(DEVTEN, DEVTEN_DISCONN | DEVTEN_USBRST | DEVTEN_CONNDONE);

	/* endpoint 0: control, 64-byte packets; events: transfer complete and not ready */
	ep_cmd(EP0_OUT, CMD_DEPSTARTCFG | CMD_PARAM(0), 0, 0, 0);
	ep_cmd(EP0_OUT, CMD_SETEPCONFIG, 64u << 3, (1u << 8) | (1u << 10) | ((u32)EP0_OUT << 25), 0);
	ep_cmd(EP0_IN, CMD_SETEPCONFIG, 64u << 3, (1u << 8) | (1u << 10) | ((u32)EP0_IN << 25), 0);
	ep_cmd(EP0_OUT, CMD_SETTRANSFRESOURCE, 1, 0, 0);
	ep_cmd(EP0_IN, CMD_SETTRANSFRESOURCE, 1, 0, 0);
	wr(DALEPENA, (1u << EP0_OUT) | (1u << EP0_IN));
	configured = 0;
	ep0_start_setup();

	wr(DCTL, rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 1000000 && (rd(DSTS) & DSTS_DEVCTRLHLT); i++)
		;
	kv("usb: running, dsts", rd(DSTS));
	return 1;
}

int
usb_fastboot(const struct fb_ops *ops, u32 idle_secs)
{
	fb = ops;
	fb_done = fb_finish = in_busy = have_pending = 0;
	dl_size = 0;
	kv("usb: dwc3 gsnpsid", rd(0xc120));
	if (!controller_start())
		return 0;
	last_heard = now_ticks();
	u64 idle = ticks_per_sec() * idle_secs;
	while (!fb_done) {
		poll_events();
		wdt_pet();
		if (now_ticks() - last_heard > idle) {
			puts_("usb: nothing heard for a while: giving up\n");
			break;
		}
	}
	if (fb_done) {
		wr(DCTL, rd(DCTL) & ~DCTL_RUN_STOP);	/* off the bus before what comes next */
		puts_("usb: done\n");
	}
	log_sync();
	return fb_done;
}
