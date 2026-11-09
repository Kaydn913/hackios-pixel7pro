/*
 * The touchscreen's chip: a Synaptics S3908 ("S3908GA1B0-15.0") speaking TouchComm v1, on SPI, its
 * pins worked by hand (touchspi, 2026-10-23: powered when our code runs; it answers a reset in
 * ~40 ms). Pins (live device tree, touchscreen@0): gpp20-0 clock, -1 MOSI, -2 MISO (pull-down),
 * -3 chip select; gpp23-2 reset, gpa7-0 interrupt, both active low. SPI mode 0, about 500 kHz.
 *
 * A message: 0xa5, code, length (LE16), payload, 0x5a. It is read in two goes, as TouchComm v1 has
 * it: the 4-byte header, then the rest, which comes back as a "continued read": 0xa5 0x03, then the
 * payload and the end marker. A command: code, length (LE16), payload, in one go; the reply is a
 * message like any other.
 */
#include "board.h"
#include "touch.h"

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
spi_xfer(const u8 *out, u8 *in, u32 n)
{
	pin_set(GPP20, CS, 0);
	delay_us(2);
	for (u32 i = 0; i < n; i++) {
		u8 b = spi_byte(out ? out[i] : 0xff);
		if (in)
			in[i] = b;
	}
	delay_us(2);
	pin_set(GPP20, CS, 1);
	delay_us(10);
}

void
touch_pins(void)
{
	pin_set(GPP20, CLK, 0);
	pin_set(GPP20, CS, 1);
	pin_mode(GPP20, CLK, 1);
	pin_mode(GPP20, MOSI, 1);
	pin_mode(GPP20, MISO, 0);
	*reg(GPP20 + PUD) = (*reg(GPP20 + PUD) & ~(0xfu << (4 * MISO))) | (1u << (4 * MISO));
	pin_mode(GPP20, CS, 1);
	pin_mode(GPA7, 0, 0);
	pin_set(GPP23, 2, 1);
	pin_mode(GPP23, 2, 1);
}

int
touch_irq(void)
{
	return !pin_get(GPA7, 0);
}

int
touch_reset(void)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	pin_set(GPP23, 2, 0);
	delay_us(20000);
	pin_set(GPP23, 2, 1);
	for (u64 t = ticks(); ticks() - t < f / 2;)
		if (touch_irq())
			return 1;
	return 0;
}

int
tcm_read(u8 *code, u8 *buf, u32 max)
{
	u8 head[4], rest[200];
	spi_xfer(0, head, 4);
	if (head[0] != 0xa5)
		return -1;
	*code = head[1];
	u32 len = head[2] | (u32)head[3] << 8;
	if (len == 0)
		return 0;
	u32 take = len + 3;			/* 0xa5 0x03, the payload, 0x5a */
	if (take > sizeof(rest))
		take = sizeof(rest);
	spi_xfer(0, rest, take);
	if (rest[0] != 0xa5 || rest[1] != 0x03)
		return -1;
	u32 n = len < max ? len : max;
	if (n > take - 2)
		n = take - 2;
	for (u32 i = 0; i < n; i++)
		buf[i] = rest[2 + i];
	return (int)n;
}

void
tcm_command(u8 cmd, const u8 *payload, u32 n)
{
	u8 out[16];
	out[0] = cmd, out[1] = (u8)n, out[2] = (u8)(n >> 8);
	for (u32 i = 0; i < n && i < sizeof(out) - 3; i++)
		out[3 + i] = payload[i];
	spi_xfer(out, 0, 3 + n);
}
