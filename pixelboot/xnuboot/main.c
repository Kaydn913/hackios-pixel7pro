/*
 * xnuboot: boot HackiOS's XNU on the bare Pixel 7 Pro (cheetah, gs201), from `fastboot boot`.
 *
 * Adapted from avfboot (dylanneve1/hackios-xnu-kernel, avf/shim/avfboot.c), which boots the same
 * kernel as the "Linux kernel" of an AVF VM; that in turn does what xnuloader/loader.c does under
 * UEFI. Here the machine is the real phone:
 *
 *   - common/start.S has dropped us from EL2 to EL1; board.c gives the log (read back in Android as
 *     /sys/fs/pstore/console-ramoops-0 after the watchdog's warm reset) and the watchdogs
 *   - the payload (xnuloader/mkpayload.py: kernel=, ramdisk=, cmdline) sits behind this program at
 *     _end (tools/append_payload.py): the bootloader's own ramdisks come first in its initrd
 *   - XNU's window is the biggest stretch of RAM free of reservations: the banks above 4 GB, where
 *     nothing of the firmware's lives (the low 2 GB is full of TrustZone, modem and firmware
 *     carve-outs). The panic log goes right behind the window, so it survives a warm reset too
 *   - the screen is the framebuffer the bootloader scans out (DPP channel 0: 1440x3120 at
 *     0xfac00000). The panel is a command-mode one and the bootloader leaves DECON0's trigger masked,
 *     so it is unmasked here, as fbtest does: from then on every TE pulse sends a frame
 *   - one processor. Cores that XNU starts itself with PSCI CPU_ON would come up at EL2, which it
 *     does not expect: more cores need an EL2 stub that catches CPU_ON (later)
 *
 * The watchdogs are left running: XNU does not feed them, so a run lasts about a minute and ends in a
 * warm reset, with this program's log and any panic log kept for the next look.
 */
#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "usbdev.h"
#include "fdtx.h"
#include "identity.h"		/* xnuloader/identity.h: who iOS shall think we are (an iPhone X) */
#include "touch.h"
#include "acpm.h"

#ifdef WIFI_PCIE
void wifi_isolation(u64 el);	/* wifi.c */
int wifi_pcie_up(void);
#endif

/*
 * The clocks: the bootloader leaves the CPU clusters at 1.1-1.2 GHz, the memory interface at
 * 421 MHz and the internal bus at 200 MHz, and nothing of iOS's raises them. ACPM (acpm.c) is
 * asked for Android's top rates (cpufreq/devfreq maxima of cheetah), and it sets the voltages
 * with them. A domain whose rate ACPM does not report sensibly first is left alone.
 * clkprobe, 2026-10-23: 1196 -> 1802, 1196 -> 2347, 1105 -> 2850 MHz, as measured by the cores.
 */
#ifndef MIF_KHZ
#define MIF_KHZ 3172000
#endif
#ifndef INT_KHZ
#define INT_KHZ 533000
#endif
#ifndef CL0_KHZ
#define CL0_KHZ 1803000
#endif
#ifndef CL1_KHZ
#define CL1_KHZ 1836000	/* not 2348000 nor CL2 2850000: with nothing to throttle them the TMU tripped in a minute */
#endif
#ifndef CL2_KHZ
#define CL2_KHZ 1826000
#endif
/*
 * The GPU (Mali-G710): two domains, its top level and its shader cores (which is which is not
 * known; both are 151 MHz after the bootloader). Android's table (dvfs_table of the Mali driver)
 * runs the top at 302-996 MHz and the shaders at 202-848; 572 MHz is a middle level of the
 * shaders' and near one of the top's, cool enough with nothing to throttle the GPU yet.
 */
#ifndef G3D_KHZ
#define G3D_KHZ 572000
#endif
#define ACPM_DVFS_CHANNEL	0	/* cal_if's acpm-ipc-channel */

static void
clocks_up(void)
{
	static const char *const names[7] = { "mif", "int", "cl0", "cl1", "cl2", "g3d5", "g3d6" };
	static const u32 want[7] = { MIF_KHZ, INT_KHZ, CL0_KHZ, CL1_KHZ, CL2_KHZ, G3D_KHZ, G3D_KHZ };
	struct acpm_ch c;

	if (acpm_open(ACPM_DVFS_CHANNEL, &c)) {
		puts_("xnuboot: no ACPM DVFS channel; the clocks stay as the bootloader left them\n");
		return;
	}
	for (u32 d = 0; d < 7; d++) {
		u32 before = acpm_dvfs_get(&c, d);
		puts_("xnuboot: clock ");
		puts_(names[d]);
		puts_(" kHz ");
		hex(before, 8);
		if (!want[d] || before < 50000 || before > 4000000) {
			puts_(" (left alone)\n");
			continue;
		}
		int r = acpm_dvfs_set(&c, d, want[d]);
		puts_(r ? " -> no reply, now " : " -> ");
		hex(acpm_dvfs_get(&c, d), 8);
		put('\n');
	}
}

/*
 * The touchscreen: reset the Synaptics chip, take its identify report, and turn off the big 0xc3
 * reports it sends along with every touch report, so that the EL2 monitor (which hands touches to
 * the kernel as a virtio tablet) only has the small ones to read. 1: the chip is there.
 */
static u64 touch_flag;
#ifdef NO_IDLE
#define IDLE_FLAG 0
#else
#define IDLE_FLAG EL2_IDLE
#endif
#ifdef NO_GOVERN
#define GOVERN_FLAG 0
#else
#define GOVERN_FLAG EL2_GOVERN
#endif

static int
touch_init(void)
{
	u8 code = 0, b[64];
	touch_pins();
	if (!touch_reset()) {
		puts_("xnuboot: touch chip silent after reset; no touch\n");
		return 0;
	}
	int got = tcm_read(&code, b, sizeof(b));
	kv("xnuboot: touch, first message code", code);
	kv("xnuboot: touch, its length", (u64)got);
	u8 off = 0xc3;
	tcm_command(TCM_DISABLE_REPORT, &off, 1);
	for (int i = 0; i < 4; i++) {		/* the reply (and any reports queued before it) */
		u64 t, f;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
		for (u64 now = t; now - t < f / 20 && !touch_irq();)
			__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
		if (!touch_irq())
			break;
		got = tcm_read(&code, b, sizeof(b));
		kv("xnuboot: touch, then code", code);
	}
	return 1;
}

#define PAYLOAD_MAGIC		0x50554E58u	/* "XNUP" */
#define KERNEL_VIRT_BASE	0xFFFFFFF004000000ULL
#define KERNEL_LINK_OFFSET	0x03000000ULL	/* the kernel's text sits 48 MB above the window's base */
#define BASE_ALIGN		0x02000000ULL	/* 32 MB: L2 block with 16K pages */
#define WIN_MAX			0x180000000ULL	/* 6 GB: the most the kernel takes */
/*
 * iOS's system image (HackiOS's system.apfs, 2.5 GB) does not fit a boot image: it comes over USB
 * (common/usbdev.c) once this program runs -- `fastboot stage system.apfs`, `fastboot oem boot` --
 * to where HackiOS's loader keeps it resident, and is the root, md0 ("RAMDisk"); the boot image's
 * RAM disk (root.apfs) is then md1 ("RAMDisk1"). As xnuloader/loader.c.
 */
#define RESIDENT_RD_OFFSET	0x08000000ULL
#define RESIDENT_RD_RESERVE	0x30000000ULL	/* RAM that must remain for the kernel itself */
#define USB_WAIT_SECS		180		/* no word from the PC for this long: boot without */
#ifndef SESSION_SECS
#define SESSION_SECS		600		/* the EL2 monitor feeds the watchdogs this long */
#endif
#define PAGE_SIZE_MAX		0x4000ULL
#define PANIC_LOG_SIZE		0x80000u
#define NVRAM_BANK_SIZE		0x2000u
#define DT_MAX			0x10000ULL
#define BOOT_LINE_LENGTH	608
#define MAX_BANKS		16
#define MAX_RESV		96

/* the display as the bootloader leaves it (probe), and its one peripheral window */
#define FB_BASE			0xfac00000ULL
#define FB_WIDTH		1440u
#define FB_HEIGHT		3120u
#define FB_STRIDE		(FB_WIDTH * 4)
#define DECON0			0x1c240000ULL
#define DECON_TRIG_CON		0x0030
#define DECON_SHD_REG_UP_REQ	0x0050
#ifndef VIDEO_SCALE
#define VIDEO_SCALE		2		/* the text console's scale (boot_args video v_depth) */
#endif
#define IO_BASE			0x00400000ULL	/* non-zero: XNU takes 0 for "no SoC" */
#define IO_SIZE			0x3FC00000ULL
/* the kernel's serial port: no hardware, a page the EL2 monitor (common/el2.c) catches -- its log */
#define FAKE_UART		0x3ffff000ULL

#define LC_SEGMENT_64		0x19
#define LC_UNIXTHREAD		0x5
#define MH_MAGIC_64		0xFEEDFACFu

struct payload_header { uint32_t magic, size; };
struct payload_entry { char name[16]; uint32_t size, reserved; };

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t revision, version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, commandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

extern char _start[], _end[];

/* ---- small libc: the MMU is off, so everything is Device memory and must be aligned ---- */

void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	if (!((uintptr_t)d & 7) && !c) {
		for (; n >= 8; n -= 8, d += 8)
			*(volatile uint64_t *)d = 0;
	}
	while (n--)
		*(volatile uint8_t *)d++ = (uint8_t)c;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (!(((uintptr_t)d | (uintptr_t)s) & 7)) {
		for (; n >= 8; n -= 8, d += 8, s += 8)
			*(volatile uint64_t *)d = *(volatile const uint64_t *)s;
	}
	while (n--)
		*(volatile uint8_t *)d++ = *(volatile const uint8_t *)s++;
	return dst;
}

static size_t str_len(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int starts_with(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

/* is `word` (e.g. "rd=") the start of one of the space-separated arguments in s? */
static int has_arg(const char *s, const char *word)
{
	for (const char *c = s; *c; c++)
		if ((c == s || c[-1] == ' ') && starts_with(c, word))
			return 1;
	return 0;
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void range_log(const char *what, uint64_t lo, uint64_t hi)
{
	puts_(what);
	puts_(" 0x");
	hex(lo, 10);
	puts_("-0x");
	hex(hi, 10);
	put('\n');
}

static const char *stage = "start";
static u64 entry_el_seen;	/* 2: EL2 is ours, and with it the monitor and the serial log */

static void __attribute__((noreturn)) die(const char *why)
{
	puts_("xnuboot: FATAL during ");
	puts_(stage);
	puts_(": ");
	puts_(why);
	put('\n');
	hang();
}

/* ---- the machine, from the bootloader's FDT ---- */

static struct {
	uint64_t bank_lo[MAX_BANKS], bank_hi[MAX_BANKS];
	int nbanks;
	uint64_t resv_lo[MAX_RESV], resv_hi[MAX_RESV];
	int nresv;
	uint64_t gicd, gicd_size, gicr, gicr_size;
	uint32_t vtimer_intid;
	int psci_hvc;
	uint64_t self;
	int ncpus;
	uint64_t mpidr[8];
	uint32_t capacity[8], max_capacity;	/* capacity-dmips-mhz: A55 0xfa, A78 0x39f, X1 0x400 */
} m;

static int cells_of(const struct fdt *f, int node, const char *name, int dflt)
{
	uint32_t len;
	uintptr_t v = fdt_prop(f, node, name, &len);
	return v && len == 4 ? (int)fdt_be32(v) : dflt;
}

static int mem_streq(uintptr_t a, const char *b)
{
	while (*(volatile char *)a && *(volatile char *)a == *b)
		a++, b++;
	return *(volatile char *)a == *b;
}

/* every <base size> of a node's reg, with the parent's cell counts */
static int reg_entries(const struct fdt *f, int node, int ac, int sc, uint64_t *lo, uint64_t *hi, int max)
{
	uint32_t len;
	uintptr_t v = fdt_prop(f, node, "reg", &len);
	uint32_t stride = 4U * (ac + sc);
	int n = 0;
	if (!v)
		return 0;
	for (uint32_t off = 0; off + stride <= len && n < max; off += stride, n++) {
		lo[n] = fdt_cells(v + off, ac);
		hi[n] = lo[n] + fdt_cells(v + off + 4 * ac, sc);
	}
	return n;
}

static void read_fdt(const struct fdt *f)
{
	int ac = cells_of(f, 0, "#address-cells", 2), sc = cells_of(f, 0, "#size-cells", 1);
	uint32_t len;
	uintptr_t v;
	int n;

	stage = "fdt: memory";
	for (n = fdt_first_child(f, 0); n >= 0; n = fdt_next_sibling(f, n)) {
		uintptr_t dt = fdt_prop(f, n, "device_type", &len);
		if (dt && mem_streq(dt, "memory"))
			m.nbanks += reg_entries(f, n, ac, sc, m.bank_lo + m.nbanks, m.bank_hi + m.nbanks, MAX_BANKS - m.nbanks);
	}
	if (!m.nbanks)
		die("no memory node in the FDT");
	for (int i = 0; i < m.nbanks; i++)
		range_log("xnuboot: RAM", m.bank_lo[i], m.bank_hi[i]);

	/* the firmware's carve-outs with a fixed place (dynamic pools have no reg and are not ours to keep) */
	stage = "fdt: reserved-memory";
	int rm = fdt_path(f, "/reserved-memory");
	if (rm >= 0) {
		int rac = cells_of(f, rm, "#address-cells", ac), rsc = cells_of(f, rm, "#size-cells", sc);
		for (n = fdt_first_child(f, rm); n >= 0 && m.nresv < MAX_RESV; n = fdt_next_sibling(f, n))
			m.nresv += reg_entries(f, n, rac, rsc, m.resv_lo + m.nresv, m.resv_hi + m.nresv, MAX_RESV - m.nresv);
	}
	puts_("xnuboot: reserved regions: ");
	hex((uint64_t)m.nresv, 2);
	put('\n');

	stage = "fdt: gic";
	uint64_t lo[2], hi[2];
	if ((n = fdt_find_compatible(f, -1, "arm,gic-v3")) < 0 || reg_entries(f, n, ac, sc, lo, hi, 2) < 2)
		die("no GICv3 in the FDT");
	m.gicd = lo[0], m.gicd_size = hi[0] - lo[0];
	m.gicr = lo[1], m.gicr_size = hi[1] - lo[1];

	stage = "fdt: timer";
	m.vtimer_intid = 27;	/* the architected default, PPI 11 */
	if ((n = fdt_find_compatible(f, -1, "arm,armv8-timer")) >= 0 &&
	    (v = fdt_prop(f, n, "interrupts", &len)) && len >= 36) {
		/* secure phys, phys, virt, hyp; <type num flags> each */
		uint32_t type = fdt_be32(v + 24), num = fdt_be32(v + 28);
		if (type == 1 && num < 16)
			m.vtimer_intid = 16 + num;
	}

	stage = "fdt: psci";
	m.psci_hvc = 0;		/* the phone: EL3 firmware, SMC */
	if ((n = fdt_find_compatible(f, -1, "arm,psci-1.0")) >= 0 ||
	    (n = fdt_find_compatible(f, -1, "arm,psci-0.2")) >= 0 ||
	    (n = fdt_find_compatible(f, -1, "arm,psci")) >= 0) {
		if ((v = fdt_prop(f, n, "method", &len)))
			m.psci_hvc = !mem_streq(v, "smc");
	}
	m.self = rsr("mpidr_el1") & 0xff00ffffffULL;

	/*
	 * The cores: this one first (the kernel starts on the first), then the rest biggest first --
	 * with cpus=N the kernel takes the first N of the list (as avfboot orders them).
	 */
	stage = "fdt: cpus";
	int cpus = fdt_path(f, "/cpus");
	int cac = cells_of(f, cpus, "#address-cells", 1);
	m.mpidr[m.ncpus++] = m.self;
	for (n = fdt_first_child(f, cpus); n >= 0 && m.ncpus < 8; n = fdt_next_sibling(f, n)) {
		uintptr_t dt = fdt_prop(f, n, "device_type", &len);
		uintptr_t r = fdt_prop(f, n, "reg", &len);
		uint32_t clen = 0;
		uintptr_t cap = fdt_prop(f, n, "capacity-dmips-mhz", &clen);
		if (!dt || !mem_streq(dt, "cpu") || !r || len < 4U * cac)
			continue;
		uint64_t id = fdt_cells(r, cac) & 0xff00ffffffULL;
		int at = id == m.self ? 0 : m.ncpus++;
		m.mpidr[at] = id;
		m.capacity[at] = cap && clen == 4 ? fdt_be32(cap) : 0;
		if (m.capacity[at] > m.max_capacity)
			m.max_capacity = m.capacity[at];
	}
	for (int i = 2; i < m.ncpus; i++)	/* (insertion sort, past the boot core) */
		for (int j = i; j > 1 && m.capacity[j] > m.capacity[j - 1]; j--) {
			uint64_t t = m.mpidr[j];
			uint32_t c = m.capacity[j];
			m.mpidr[j] = m.mpidr[j - 1], m.capacity[j] = m.capacity[j - 1];
			m.mpidr[j - 1] = t, m.capacity[j - 1] = c;
		}
	puts_("xnuboot: cpus (mpidr:capacity):");
	for (int i = 0; i < m.ncpus; i++) {
		puts_(" ");
		hex(m.mpidr[i], 4);
		put(':');
		hex(m.capacity[i], 3);
	}
	put('\n');

	kv("xnuboot: gicd", m.gicd);
	kv("xnuboot: gicr", m.gicr);
	kv("xnuboot: gicr_size", m.gicr_size);
	kv("xnuboot: vtimer_intid", m.vtimer_intid);
	kv("xnuboot: psci_hvc", (uint64_t)m.psci_hvc);
	kv("xnuboot: self_mpidr", m.self);
}

/*
 * XNU's window: the biggest stretch of RAM (banks merged where they touch) that no reservation
 * cuts into, 32 MB aligned, at most WIN_MAX, with room for the panic log right behind it.
 */
static void choose_window(uint64_t *base, uint64_t *size, uint64_t *pram)
{
	uint64_t best_lo = 0, best_hi = 0;

	stage = "window";
	for (int i = 0; i < m.nbanks; i++) {
		uint64_t lo = m.bank_lo[i], hi = m.bank_hi[i];
		/* grow the run while another bank starts where it ends */
		for (int grew = 1; grew;) {
			grew = 0;
			for (int j = 0; j < m.nbanks; j++)
				if (m.bank_lo[j] == hi)
					hi = m.bank_hi[j], grew = 1;
		}
		/* sweep [lo, hi): the gaps between the reservations met in address order are free */
		uint64_t at = lo;
		for (;;) {
			int first = -1;		/* the reservation that starts next at or after `at` -- or covers it */
			for (int r = 0; r < m.nresv; r++)
				if (m.resv_hi[r] > at && m.resv_lo[r] < hi &&
				    (first < 0 || m.resv_lo[r] < m.resv_lo[first]))
					first = r;
			uint64_t gap_end = first < 0 ? hi : (m.resv_lo[first] > at ? m.resv_lo[first] : at);
			if (gap_end - at > best_hi - best_lo)
				best_lo = at, best_hi = gap_end;
			if (first < 0)
				break;
			at = m.resv_hi[first];
			if (at >= hi)
				break;
		}
	}
	uint64_t lo = align_up(best_lo, BASE_ALIGN);
	uint64_t hi = best_hi & ~(BASE_ALIGN - 1);
	if (hi <= lo + BASE_ALIGN)
		die("no free stretch of RAM for XNU");
	if (hi - lo - BASE_ALIGN > WIN_MAX)
		hi = lo + WIN_MAX + BASE_ALIGN;
	*base = lo;
	*size = hi - lo - BASE_ALIGN;		/* the last 32 MB: the panic log and spare */
	*pram = lo + *size;
	range_log("xnuboot: biggest free stretch", best_lo, best_hi);
}

/* ---- Apple device tree writer (as xnuloader/loader.c) ----
 * node := u32 nProperties, u32 nChildren, props[], children[]
 * prop := char name[32], u32 length, value padded to 4 bytes */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16];
	int depth;
};

static void dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(volatile uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

static void dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		/* the screen here is sRGB, not the iPhone X's P3 (see loader.c) */
		if (str_eq(props[i].name, "artwork-display-gamut")) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

static void dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0)
		w->counts[w->depth - 1][1]++;
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* iBoot's NVRAM copy in CHRP format: a valid header and one empty "common" partition */
static void nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;
	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	sum = h[0];
	for (int i = 2; i < 16; i++)
		sum += h[i];
	while (sum > 0xff)
		sum = (sum & 0xff) + (sum >> 8);
	h[1] = (uint8_t)sum;
}

static uint8_t nvram_image[NVRAM_BANK_SIZE];

static void build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;
	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ---- the payload ---- */

struct payload_info {
	const uint8_t *kernel, *ramdisk;
	uint32_t kernel_size, ramdisk_size;
	const char *cmdline;
};

static int parse_payload(struct payload_info *pi, uintptr_t base)
{
	const struct payload_header *ph = (const struct payload_header *)base;
	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC)
		return 0;
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const uint8_t *data = p + sizeof(*e);
		if (e->name[0] == 0)
			break;
		puts_("xnuboot: payload entry ");
		puts_(e->name);
		kv(" size", e->size);
		if (str_eq(e->name, "kernel")) {
			pi->kernel = data;
			pi->kernel_size = e->size;
		} else if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = data;
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = (const char *)data;
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return pi->kernel != NULL;
}

static void cmd_append(char *cl, uint32_t *n, const char *s)
{
	while (*s && *n < BOOT_LINE_LENGTH - 1)
		cl[(*n)++] = *s++;
}

/* clean and invalidate to the point of coherency what XNU will read with its caches on */
static void dcache_clean_inval(uint64_t lo, uint64_t hi);

#ifdef WIFI_PCIE
/*
 * Memory for the Wi-Fi chip's DMA (rings, indices, buffers): 1 MB of RAM below 4 GB, where the
 * probes did all their DMA, and which XNU never has (its window is above 4 GB). Taken from the top
 * of the bank at 0x80000000 down, clear of the firmware's reservations and of what we are (this
 * program, its payload, the FDT: all below 0xc0000000). Zeroed; the kernel maps it uncached
 * (bcmdma=, bsd/dev/redwood_bcm4389.c), so it needs no cache maintenance.
 */
#define WIFI_DMA_SIZE	0x100000ULL

static uint64_t
wifi_dma_block(void)
{
	for (uint64_t lo = 0xff000000ULL; lo >= 0xc0000000ULL; lo -= WIFI_DMA_SIZE) {
		int clear = 0;
		for (int b = 0; b < m.nbanks; b++)
			if (m.bank_lo[b] <= lo && lo + WIFI_DMA_SIZE <= m.bank_hi[b])
				clear = 1;
		for (int r = 0; r < m.nresv && clear; r++)
			if (m.resv_lo[r] < lo + WIFI_DMA_SIZE && m.resv_hi[r] > lo)
				clear = 0;
		if (!clear)
			continue;
		memset((void *)(uintptr_t)lo, 0, WIFI_DMA_SIZE);
		dcache_clean_inval(lo, lo + WIFI_DMA_SIZE);
		range_log("xnuboot: wifi: DMA memory", lo, lo + WIFI_DMA_SIZE);
		return lo;
	}
	puts_("xnuboot: wifi: no free MB below 4 GB for DMA\n");
	return 0;
}
#endif

static void dcache_clean_inval(uint64_t lo, uint64_t hi)
{
	for (uint64_t a = lo & ~63ULL; a < hi; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* the screen: black, then DECON0's trigger unmasked so the panel follows the framebuffer (fbtest) */
static void screen_on(void)
{
	memset((void *)FB_BASE, 0, (uint64_t)FB_STRIDE * FB_HEIGHT);
	*reg(DECON0 + DECON_SHD_REG_UP_REQ) |= (1u << 31) | (1u << 20) | (1u << 0);
	*reg(DECON0 + DECON_TRIG_CON) = (*reg(DECON0 + DECON_TRIG_CON) | 1u) & ~(1u << 4);
	kv("xnuboot: decon0 trig_con", *reg(DECON0 + DECON_TRIG_CON));
}

/* ---- the system image over USB ---- */

static uint64_t resident_base, resident_room, resident_size;

/*
 * The CPU caches and the system image: the controller writes it into RAM by DMA, past the caches.
 * Lines of that RAM still in a cache from before (the bootloader's) would be stale -- or, dirty, be
 * written back over it: the first run read "apfs: block 2538: bad checksum" and found no dyld cache,
 * though the file was whole. So its lines are dropped (invalidated, never cleaned) before and after.
 */
static void
dcache_inval(uint64_t lo, uint64_t hi)
{
	for (uint64_t a = lo & ~63ULL; a < hi; a += 64)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static u64
usb_dest(u32 size)
{
	return size <= resident_room ? resident_base : 0;
}

/* (its sum, as pixelboot/tools/sum64.py, matched the file's on 2026-10-23: the transfer is sound) */
static void
usb_done(u64 at, u32 size)
{
	(void)at;
	resident_size = size;
	kv("xnuboot: system image over USB, bytes", size);
}

static int
usb_command(const char *cmd, char *resp)
{
	if (str_eq(cmd, "oem boot") || str_eq(cmd, "continue")) {
		resp[0] = 'O', resp[1] = 'K', resp[2] = 'A', resp[3] = 'Y', resp[4] = 0;
		return 1;
	}
	return 0;
}

static const struct fb_ops usb_ops = { usb_command, usb_dest, usb_done };

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

static void xnuboot(uint64_t fdt_pa)
{
	struct fdt f;
	struct payload_info pi;

	stage = "fdt";
	if (!fdt_open(&f, fdt_pa))
		die("x0 is not an FDT");
	read_fdt(&f);

	stage = "payload";
	if (!parse_payload(&pi, (uintptr_t)_end))
		die("no XNUP payload with a kernel behind the program (tools/append_payload.py)");

	/* --- kernel extent --- */
	const struct mach_header_64 *mh = (const struct mach_header_64 *)pi.kernel;
	if (((uintptr_t)mh & 7) || mh->magic != MH_MAGIC_64)
		die("the kernel entry is not a 64-bit Mach-O");
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (!sg->vmsize)
				continue;
			if (sg->vmaddr < vm_lo)
				vm_lo = sg->vmaddr;
			if (sg->vmaddr + sg->vmsize > vm_hi)
				vm_hi = sg->vmaddr + sg->vmsize;
		} else if (lc->cmd == LC_UNIXTHREAD) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || !entry_va)
		die("unexpected kernel layout (not a HackiOS kernel?)");

	/* --- XNU's window --- */
	uint64_t phys_base, win_size, pram;
	choose_window(&phys_base, &win_size, &pram);
	uint64_t slide = phys_base - KERNEL_VIRT_BASE;	/* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = align_up(vm_hi + slide, PAGE_SIZE_MAX);
	uint64_t rd_pa = kern_hi, rd_size = align_up(pi.ramdisk_size, PAGE_SIZE_MAX);
	uint64_t dt_pa = rd_pa + rd_size;
	uint64_t ba_pa = dt_pa + DT_MAX;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;
	if (top + 0x4000000ULL > phys_base + win_size)
		die("not enough RAM for the kernel");
	range_log("xnuboot: window", phys_base, phys_base + win_size);
	range_log("xnuboot: kernel", kern_lo, kern_hi);
	kv("xnuboot: entry", entry_va + slide);
	kv("xnuboot: pram", pram);
	wdt_pet();

	/* --- copy --- */
	stage = "copy kernel";
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || !sg->vmsize)
			continue;
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), pi.kernel + sg->fileoff, sg->filesize);
		wdt_pet();
	}
	/* --- the system image, over USB, where HackiOS keeps it resident --- */
	uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET, res_size = 0;
	if (entry_el_seen == 2 && win_size > RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
		stage = "usb";
		if (top > res_pa)
			die("the kernel reaches into the resident image's place");
		resident_base = res_pa;
		resident_room = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
		if (resident_room > 0xffffffffULL)
			resident_room = 0xffffffffULL;
		kv("xnuboot: room for a system image", resident_room);
		puts_("xnuboot: waiting on USB: fastboot stage system.apfs; fastboot oem boot\n");
		usb_fastboot(&usb_ops, USB_WAIT_SECS);
		res_size = resident_size;
		if (!res_size)
			puts_("xnuboot: no system image: booting with the boot image's RAM disk as the root\n");
	}
	if (res_size) {
		/*
		 * As xnuloader/loader.c: the boot image's RAM disk, the device tree and boot_args go behind
		 * the system image, so that topOfKernelData is past it. XNU takes all below that as in use
		 * and the rest as free RAM: with them in front, it used the image's pages ("apfs: block
		 * 2538: bad checksum", though the image had arrived whole -- its sum matched).
		 */
		rd_pa = align_up(res_pa + res_size, PAGE_SIZE_MAX);
		dt_pa = rd_pa + rd_size;
		ba_pa = dt_pa + DT_MAX;
		top = ba_pa + PAGE_SIZE_MAX;
		if (top + RESIDENT_RD_RESERVE > phys_base + win_size)
			die("not enough RAM behind the system image");
	}

	if (rd_size) {
		stage = "copy ramdisk";
		memset((void *)(rd_pa + pi.ramdisk_size), 0, rd_size - pi.ramdisk_size);
		memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		range_log("xnuboot: ramdisk", rd_pa, rd_pa + rd_size);
	}
	wdt_pet();

	/* --- boot_args --- */
	stage = "boot_args";
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, PAGE_SIZE_MAX);
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* v_display is how iBoot says "graphics mode": the kernel keeps its text off it; -v wants the text */
	ba->video.v_baseAddr = FB_BASE;
	ba->video.v_display = has_arg(pi.cmdline, "-v") ? 0 : 1;
	ba->video.v_rowBytes = FB_STRIDE;
	ba->video.v_width = FB_WIDTH;
	ba->video.v_height = FB_HEIGHT;
	/*
	 * v_depth carries more than the depth (pexpert/arm/pe_init.c): bits 8-15 the rotation, bits 16-23
	 * the scale less one. At 1x the 8x16 console font is 8 of the panel's 1440 pixels -- too small
	 * to read off a photo; VIDEO_SCALE=2 makes it 16.
	 */
	ba->video.v_depth = 32 | ((VIDEO_SCALE - 1) << 16);
	char *cl = ba->commandLine;
	uint32_t n = 0;
	/* serial=3: the console goes to the (fake) serial port, i.e. into our log */
	cmd_append(cl, &n, entry_el_seen == 2 ? "debug=0x8 serial=3 " : "debug=0x8 ");
	/* every core when the EL2 monitor is there to catch CPU_ON; else only this one */
	int ncpus = entry_el_seen == 2 ? m.ncpus : 1;
	if (!has_arg(pi.cmdline, "cpus=") && !has_arg(pi.cmdline, "cpumask=")) {
		char arg[] = "cpus=0 ";
		arg[5] = (char)('0' + ncpus);
		cmd_append(cl, &n, arg);
	}
	if ((rd_size || res_size) && !has_arg(pi.cmdline, "rd="))
		cmd_append(cl, &n, "rd=md0 ");
	if (entry_el_seen == 2 && touch_init())
		cmd_append(cl, &n, "virtio_input=1 "), touch_flag = EL2_TOUCH;
#ifdef WIFI_PCIE
	/* the Wi-Fi chip's link (wifi.c): the kernel's driver may touch PCIe space only after this */
	if (entry_el_seen == 2 && wifi_pcie_up()) {
		cmd_append(cl, &n, "bcmlink=1 ");
		uint64_t dma = wifi_dma_block();
		if (dma) {
			char arg[] = "bcmdma=0x00000000 ";
			for (int i = 0; i < 8; i++)
				arg[16 - i] = "0123456789abcdef"[(dma >> (4 * i)) & 0xf];
			cmd_append(cl, &n, arg);
		}
	}
#endif
	cmd_append(cl, &n, pi.cmdline);
	cl[n] = 0;

	/* --- device tree --- */
	stage = "device tree";
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = rsr("cntfrq_el0");
	uint64_t ranges[3] = { 0, IO_BASE, IO_SIZE };
	uint8_t seed[64];
	for (unsigned i = 0; i < sizeof(seed); i++)
		seed[i] = (uint8_t)(rsr("cntvct_el0") >> (i & 7)) ^ (uint8_t)(i * 37);

	dt_begin(&w, "device-tree");
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", "PXL7PCHEETAH", 32);
	dt_prop_padded(&w, "mlb-serial-number", "PXL7PCHEETAHMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", m.psci_hvc ? "hvc" : "smc");
	static const uint8_t wifi_address[6] = { 0x02, 0x50, 0x58, 0x4c, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x50, 0x58, 0x4c, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x70, 0x78, 0x6c };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x0000505846000001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	/* "RAMDisk" is /dev/md0, the root; "RAMDisk1" /dev/md1 (as xnuloader/loader.c) */
	dt_begin(&w, "memory-map");
	if (res_size) {
		uint64_t r0[2] = { res_pa, align_up(res_size, PAGE_SIZE_MAX) };
		dt_prop(&w, "RAMDisk", r0, sizeof(r0));
		if (rd_size) {
			uint64_t r1[2] = { rd_pa, rd_size };
			dt_prop(&w, "RAMDisk1", r1, sizeof(r1));
		}
	} else if (rd_size) {
		uint64_t r[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk", r, sizeof(r));
	}
	dt_end(&w);
	dt_end(&w);	/* chosen */

	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	dt_end(&w);

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts */
	dt_begin(&w, "options");
	dt_end(&w);

	/* the panic log, behind the window: kept by a warm reset for the next look */
	uint64_t pram_reg[2] = { pram, PANIC_LOG_SIZE };
	memset((void *)pram, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	dt_begin(&w, "cpus");
	for (int i = 0; i < ncpus; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)m.mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		/* a little core: the scheduler uses it once the big ones are busy */
		if (m.max_capacity && m.capacity[i] && m.capacity[i] <= m.max_capacity / 2)
			dt_prop_u32(&w, "small-core", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", "cheetah");
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,gic-v3");
	uint64_t gic_reg[4] = { m.gicd - IO_BASE, m.gicd_size, m.gicr - IO_BASE, m.gicr_size };
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_prop_u32(&w, "virtual-timer-intid", m.vtimer_intid);
	dt_end(&w);
	if (entry_el_seen == 2) {
		uint64_t uart_reg[2] = { FAKE_UART - IO_BASE, 0x1000 };
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);	/* arm-io */

	dt_end(&w);	/* device-tree */

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	if (dt_len > DT_MAX)
		die("device tree overflow");
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	range_log("xnuboot: device tree", dt_pa, dt_pa + dt_len);
	kv("xnuboot: boot_args", ba_pa);
	kv("xnuboot: top", top);
	puts_("xnuboot: boot-args: ");
	puts_(cl);
	put('\n');

	/* --- hand off --- */
#ifndef NO_CLOCKS
	stage = "clocks";
	clocks_up();
#endif
	stage = "screen";
	screen_on();
	wdt_pet();
	dcache_clean_inval(kern_lo, top);
	dcache_clean_inval(pram, pram + PANIC_LOG_SIZE);
	if (res_size)
		dcache_inval(res_pa, res_pa + res_size);	/* (never cleaned: see usb_dest) */
	if (entry_el_seen == 2) {
		kv("xnuboot: el2 monitor", hvc_call(EL2_FN_MONITOR, FAKE_UART, EL2_TRAP_WFI | touch_flag | IDLE_FLAG | GOVERN_FLAG, SESSION_SECS));
		puts_("xnuboot: the kernel's console follows; the run ends with the watchdog after the session\n");
	}
	kv("xnuboot: jumping to XNU at", entry_va + slide);
	puts_("====================================== XNU ======================================\n");
	log_sync();
	stage = "xnu";
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)(entry_va + slide))(ba_pa);
	die("XNU returned");
}

void at_entry(u64 el, u64 dtb)
{
	(void)dtb;
	entry_el_seen = el;
	log_open();
#ifdef WIFI_PCIE
	wifi_isolation(el);
#endif
}

void at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	wdt_pet();
	puts_("\nxnuboot: HackiOS XNU on the bare Pixel 7 Pro\n");
	kv("xnuboot: entry_el", entry_el);
	kv("xnuboot: now_el", el);
	kv("xnuboot: image", (u64)(uintptr_t)_start);
	kv("xnuboot: payload", (u64)(uintptr_t)_end);
	kv("xnuboot: fdt", dtb);
	xnuboot(dtb);
}
