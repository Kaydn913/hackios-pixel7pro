/*
 * The log, the watchdogs and the exception handler, for every pixelboot program.
 *
 * The log goes into the ramoops console zone in the format the kernel's pstore reads back
 * (persistent_ram_buffer: "DBGC", start, size, then a ring of text). A watchdog reset is warm, so the
 * RAM survives and Android shows the log in /sys/fs/pstore/console-ramoops-0 (as root). A normal
 * reboot is cold and does not keep it. The bootloader appends its own log to the same zone every
 * boot, before us and again after the reset, so we append too.
 */
#include "board.h"

/* ramoops (live device tree / ramoops module parameters): console zone first, 2 MB, no ECC */
#define PRZ_BASE	0xfd3ff000ul
#define PRZ_DATA_MAX	(0x200000u - 12)
/*
 * ... of which our ring takes all but 128 KB. The bootloader appends its log after the reset (some
 * 30 KB) at start, moving start and size on, but not keeping size within the zone: a ring of the
 * whole zone came back as start 448955, size 2128717 of 2097140, and pstore threw it all away
 * (gl13). Short of the zone by more than that, the append still fits; the bytes between our ring's
 * end and the zone's, which pstore then reads as part of the log, are spaces.
 */
#define PRZ_RING	(PRZ_DATA_MAX - 0x20000u)
#define PRZ_SIG		0x43474244u		/* "DBGC": PERSISTENT_RAM_SIG */

static volatile u32 *const prz_hdr = (volatile u32 *)PRZ_BASE;	/* sig, start, size */
static volatile u8 *const prz_data = (volatile u8 *)(PRZ_BASE + 12);
static u32 prz_start, prz_size;

/*
 * pstore throws the whole zone away when start > size, and so does the bootloader ("No kernel
 * logs"): the header written never says so, even if a race lost an update of prz_size.
 */
void
log_sync(void)
{
	prz_hdr[1] = prz_start;
	prz_hdr[2] = prz_size < prz_start ? prz_start : prz_size;
}

void
log_open(void)
{
	u32 start = prz_hdr[1], size = prz_hdr[2];
	if (prz_hdr[0] == PRZ_SIG && start < PRZ_RING && size <= PRZ_RING) {
		prz_start = start;
		prz_size = size;
	} else {
		/* (Android's own log, of the whole zone, does not fit our ring: we begin again) */
		prz_hdr[0] = PRZ_SIG;
		prz_start = prz_size = 0;
		log_sync();
	}
	for (u32 i = PRZ_RING; i < PRZ_DATA_MAX; i++)
		prz_data[i] = ' ';
}

void
put(char c)
{
	prz_data[prz_start] = (u8)c;
	if (++prz_start >= PRZ_RING)
		prz_start = 0;
	if (prz_size < PRZ_RING)
		prz_size++;
	if (c == '\n')
		log_sync();
}

void
puts_(const char *s)
{
	while (*s)
		put(*s++);
}

void
hex(u64 v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		put("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}

void
kv(const char *k, u64 v)
{
	puts_(k);
	puts_("=0x");
	hex(v, 16);
	put('\n');
}

void
dump(const char *name, u64 base, u64 len)
{
	puts_("== ");
	puts_(name);
	puts_(" @0x");
	hex(base, 8);
	put('\n');
	for (u64 off = 0; off < len; off += 16) {
		hex(base + off, 8);
		put(':');
		for (u64 i = 0; i < 16; i += 4) {
			put(' ');
			hex(*reg(base + off + i), 8);
		}
		put('\n');
		wdt_pet();
	}
}

/* gs201's cluster watchdogs (watchdog_cl0/cl1): Samsung's s3c2410 layout. WTCLRINT (0xc) aborts on read. */
static const u64 wdt_base[] = { 0x10060000, 0x10070000 };
#define WTCON		0x00
#define WTDAT		0x04
#define WTCNT		0x08
#define WTCON_ENABLE	(1u << 5)

void
wdt_pet(void)
{
	for (unsigned i = 0; i < 2; i++)
		if (*reg(wdt_base[i] + WTCON) & WTCON_ENABLE)
			*reg(wdt_base[i] + WTCNT) = *reg(wdt_base[i] + WTDAT);
}

void
wdt_log(void)
{
	for (unsigned i = 0; i < 2; i++) {
		puts_(i ? "watchdog cl1: wtcon/wtdat/wtcnt" : "watchdog cl0: wtcon/wtdat/wtcnt");
		for (u64 off = 0; off < 0xc; off += 4) {
			put(' ');
			hex(*reg(wdt_base[i] + off), 8);
		}
		put('\n');
	}
}

void
hang(void)
{
	log_sync();
	for (;;)
		__asm__ volatile("wfe");
}

static unsigned faults;

/*
 * A read that aborts (EC 0x25, data abort at EL1, WnR clear) is logged and skipped -- its destination
 * register keeps whatever it held -- so one unreadable register does not end a program. A write is
 * not: skipping one also skips any base-register writeback, and everything after goes astray (that
 * is how xnuboot's first boot_args lost its framebuffer). Anything else is logged and stops here,
 * and the watchdog resets the phone.
 */
u64
exception(u64 kind, u64 esr, u64 elr, u64 far)
{
	puts_("\n!! exception kind=");
	hex(kind, 2);
	puts_(" esr=");
	hex(esr, 16);
	puts_(" elr=");
	hex(elr, 16);
	puts_(" far=");
	hex(far, 16);
	if (kind == 4 && ((esr >> 26) & 0x3f) == 0x25 && !(esr & (1u << 6)) && ++faults < 64) {
		puts_(" -- skipped\n");
		return elr + 4;
	}
	puts_(" -- stopping\n");
	hang();
}
