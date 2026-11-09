/*
 * What every pixelboot program gets on cheetah (Pixel 7 Pro, gs201): a log that survives into
 * Android, the cluster watchdogs, and an exception handler that logs. See board.c.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef long s64;

static inline volatile u32 *
reg(u64 a)
{
	return (volatile u32 *)a;
}

/* the log: the ramoops console zone, read back in Android as /sys/fs/pstore/console-ramoops-0 */
void log_open(void);
void log_sync(void);
void put(char c);
void puts_(const char *s);
void hex(u64 v, int digits);
void kv(const char *k, u64 v);
void dump(const char *name, u64 base, u64 len);

/* the cluster watchdogs the bootloader leaves running (about a minute) */
void wdt_pet(void);
void wdt_log(void);

/* PSCI SYSTEM_RESET; a watchdog reset is the warm one that keeps the log */
u64 psci_call(u64 fn, u64 a1, u64 a2, u64 a3);
void hang(void) __attribute__((noreturn));

/* the EL2 monitor (el2.c), for a program entered at EL2:
   hvc_call(EL2_FN_MONITOR, fake serial page, flags, seconds the watchdogs are fed for -- 0: always) */
u64 hvc_call(u64 fn, u64 a1, u64 a2, u64 a3);
#define EL2_FN_MONITOR	0x50580001ul	/* "PX" 1 */
#define EL2_TRAP_WFI	1ul		/* feed the watchdogs whenever the kernel idles, too */
#define EL2_TOUCH	2ul		/* the touchscreen as a virtio tablet at 0x0a000000 (boot-arg virtio_input=1) */
#define EL2_GOVERN	8ul		/* a thermal governor for the big and middle clusters (ACPM) */
#define EL2_IDLE	4ul		/* a trapped WFI waits (WFE, woken by a ~40 us event stream) instead of returning at once */
