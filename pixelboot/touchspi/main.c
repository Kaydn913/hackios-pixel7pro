/*
 * touchspi: talk to the touchscreen's Synaptics chip over SPI, by hand (bit-banged GPIOs), and log
 * what it says: the message after reset (its identify report), then for 30 s every message it
 * signals with its interrupt line -- touch the screen meanwhile. The raw bytes tell the protocol
 * (TouchComm: 0xa5, code, length, payload, 0x5a) and the touch report's layout.
 *
 * Pins (live device tree, touchscreen@0 ts_active): gpp20-0 clock, -1 MOSI, -2 MISO (pull-down),
 * -3 chip select (a plain GPIO there too); gpp23-2 reset (active low), gpa7-0 interrupt (active low).
 * SPI mode 0: the clock idles low, data is taken on its rising edge.
 */
#include "board.h"

#define GPP20	(0x10c40000ul + 0 * 0x20)
#define GPP23	(0x10c40000ul + 3 * 0x20)
#define GPA7	(0x180e0000ul + 1 * 0x20)
#define CON	0x0
#define DAT	0x4
#define PUD	0x8
#define CLK	0
#define MOSI	1
#define MISO	2
#define CS	3

static u64 freq;

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
	for (u64 t = ticks(); ticks() - t < freq / 1000000 * us;)
		;
}

static void
pin_mode(u64 b, u32 pin, u32 mode)
{
	*reg(b + CON) = (*reg(b + CON) & ~(0xfu << (4 * pin))) | (mode << (4 * pin));
}

static void
pin_pud(u64 b, u32 pin, u32 pud)
{
	*reg(b + PUD) = (*reg(b + PUD) & ~(0xfu << (4 * pin))) | (pud << (4 * pin));
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

/* one byte each way, MSB first, mode 0 (about 500 kHz) */
static u8
spi_byte(u8 out)
{
	u8 in = 0;
	for (int i = 7; i >= 0; i--) {
		pin_set(GPP20, MOSI, (out >> i) & 1);
		delay_us(1);
		pin_set(GPP20, CLK, 1);
		in = (u8)(in << 1 | pin_get(GPP20, MISO));
		delay_us(1);
		pin_set(GPP20, CLK, 0);
	}
	return in;
}

static void
spi_read(u8 *buf, u32 n)
{
	pin_set(GPP20, CS, 0);
	delay_us(2);
	for (u32 i = 0; i < n; i++)
		buf[i] = spi_byte(0xff);
	delay_us(2);
	pin_set(GPP20, CS, 1);
	delay_us(10);
}

static void
log_bytes(const char *what, const u8 *b, u32 n)
{
	puts_(what);
	for (u32 i = 0; i < n; i++) {
		put(i % 32 ? ' ' : '\n');
		hex(b[i], 2);
	}
	put('\n');
}

static int
irq_low(void)
{
	return !pin_get(GPA7, 0);
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
	u8 buf[96];
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	wdt_pet();
	puts_("\ntouchspi: the Synaptics chip over bit-banged SPI\n");

	/* the bus: clock and MOSI out (low), MISO in (pull-down), chip select out (high) */
	pin_set(GPP20, CLK, 0);
	pin_set(GPP20, CS, 1);
	pin_mode(GPP20, CLK, 1);
	pin_mode(GPP20, MOSI, 1);
	pin_mode(GPP20, MISO, 0);
	pin_pud(GPP20, MISO, 1);
	pin_mode(GPP20, CS, 1);
	pin_mode(GPA7, 0, 0);

	/* reset (active low, 20 ms), then the chip's first message */
	pin_set(GPP23, 2, 1);
	pin_mode(GPP23, 2, 1);
	pin_set(GPP23, 2, 0);
	delay_us(20000);
	pin_set(GPP23, 2, 1);
	u64 t0 = ticks();
	while (!irq_low() && ticks() - t0 < freq / 2)
		;
	kv("touchspi: irq after reset, us", (ticks() - t0) * 1000000 / freq);
	spi_read(buf, 64);
	log_bytes("touchspi: first message (64 bytes):", buf, 64);
	kv("touchspi: irq after that read", (u64)irq_low());
	spi_read(buf, 64);
	log_bytes("touchspi: a second read:", buf, 64);
	kv("touchspi: irq now", (u64)irq_low());

	/* 30 s of whatever it signals: touch the screen */
	puts_("touchspi: 30 s of messages -- touch the screen\n");
	u32 count = 0;
	t0 = ticks();
	while (ticks() - t0 < 30 * freq) {
		wdt_pet();
		if (!irq_low())
			continue;
		spi_read(buf, 48);
		if (count < 40)
			log_bytes("touchspi: message:", buf, 48);
		count++;
		delay_us(500);
	}
	kv("touchspi: messages in 30 s", count);
	puts_("touchspi: done; letting the watchdog reset the phone\n");
	hang();
}
