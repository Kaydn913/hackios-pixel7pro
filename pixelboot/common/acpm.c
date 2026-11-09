/*
 * ACPM IPC, as Linux does it (mainline drivers/firmware/samsung/exynos-acpm.c; Google's
 * drivers/soc/google/acpm/acpm_ipc.c), from the live device tree of cheetah:
 *   acpm_ipc: reg 0x18210000 (the AP-to-APM mailbox), 0x18500000 (APM SRAM), initdata-base 0xa000.
 * At SRAM + initdata: struct acpm_framework, whose words 2 and 6 are the offset of the channel
 * table and the number of AP channels. A channel (72 bytes): id, ..., at 16 the APM's RX queue
 * (rear, front, base: ours to send on), at 40 its TX queue (rear, front, base: its replies),
 * at 52 the queue length, at 56 the message length in bytes, at 68 "the AP polls".
 * Sending: the message at base + mlen * front, front advanced, then the doorbell: bit <id> of
 * the mailbox's INTGR1. Word 0 of a message carries a sequence number (1-63) in bits 16-21,
 * which the reply carries back.
 */
#include "board.h"
#include "acpm.h"

#define ACPM_MBOX	0x18210000ul
#define ACPM_SRAM	0x18500000ul
#define ACPM_INITDATA	0xa000ul
#define MBOX_INTGR1	0x40
#define SEQ_SHIFT	16

static u32 seq;

static u64
now(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static u64
ms(u64 n)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f / 1000 * n;
}

static void
chan(u64 i, struct acpm_ch *c)
{
	u64 table = ACPM_SRAM + *reg(ACPM_SRAM + ACPM_INITDATA + 8), ch = table + 72 * i;
	c->id = *reg(ch);
	c->tx_rear = ACPM_SRAM + *reg(ch + 16);
	c->tx_front = ACPM_SRAM + *reg(ch + 20);
	c->tx_base = ACPM_SRAM + *reg(ch + 24);
	c->rx_rear = ACPM_SRAM + *reg(ch + 40);
	c->rx_front = ACPM_SRAM + *reg(ch + 44);
	c->rx_base = ACPM_SRAM + *reg(ch + 48);
	c->qlen = *reg(ch + 52);
	c->mlen = *reg(ch + 56);
	c->poll = *reg(ch + 68);
}

static u32
channels(void)
{
	return *reg(ACPM_SRAM + ACPM_INITDATA + 24);
}

void
acpm_dump(void)
{
	struct acpm_ch c;
	u32 n = channels();
	kv("acpm: AP channels", n);
	for (u32 i = 0; i < n && i < 32; i++) {
		chan(i, &c);
		puts_("acpm: channel id ");
		hex(c.id, 2);
		puts_(" qlen ");
		hex(c.qlen, 2);
		puts_(" mlen ");
		hex(c.mlen, 2);
		puts_(" poll ");
		hex(c.poll, 1);
		puts_(" tx ");
		hex(c.tx_base - ACPM_SRAM, 5);
		puts_(" rx ");
		hex(c.rx_base - ACPM_SRAM, 5);
		put('\n');
	}
}

int
acpm_open(u32 id, struct acpm_ch *c)
{
	u32 n = channels();
	for (u32 i = 0; i < n && i < 32; i++) {
		chan(i, c);
		if (c->id == id)
			return c->qlen > 2 && c->qlen < 256 && c->mlen >= 16 && c->mlen <= 64 ? 0 : -1;
	}
	return -1;
}

int
acpm_xfer(struct acpm_ch *c, u32 cmd[4])
{
	u32 front = *reg(c->tx_front) % c->qlen, next = (front + 1) % c->qlen;
	u64 t = now();
	/* room for it: the APM keeps up with what was sent before (as Google's driver waits) */
	while ((front + 2) % c->qlen == *reg(c->rx_front) || next == *reg(c->tx_rear))
		if (now() - t > ms(100))
			return -2;
	seq = seq % 63 + 1;
	cmd[0] = (cmd[0] & ~(0x3fu << SEQ_SHIFT)) | seq << SEQ_SHIFT;
	u64 slot = c->tx_base + (u64)c->mlen * front;
	for (u32 i = 0; i < c->mlen / 4; i++)
		*reg(slot + 4 * i) = i < 4 ? cmd[i] : 0;
	__asm__ volatile("dsb sy" ::: "memory");
	*reg(c->tx_front) = next;
	__asm__ volatile("dsb sy" ::: "memory");
	*reg(ACPM_MBOX + MBOX_INTGR1) = 1u << c->id;

	/* the replies: every one is taken off the queue; ours is the one with our sequence number */
	int got = -1;
	for (t = now(); got && now() - t < ms(100);) {
		u32 rear = *reg(c->rx_rear) % c->qlen, rfront = *reg(c->rx_front) % c->qlen;
		if (rear == rfront)
			continue;
		for (; rear != rfront; rear = (rear + 1) % c->qlen) {
			u64 m = c->rx_base + (u64)c->mlen * rear;
			if (((*reg(m) >> SEQ_SHIFT) & 0x3f) == seq) {
				for (u32 i = 0; i < 4; i++)
					cmd[i] = *reg(m + 4 * i);
				got = 0;
			}
		}
		*reg(c->rx_rear) = rfront;
	}
	return got;
}

u32
acpm_dvfs_get(struct acpm_ch *c, u32 domain)
{
	u32 cmd[4] = { domain & 0xfff, 0, 1, 0 };	/* FREQ_GET */
	return acpm_xfer(c, cmd) ? 0 : cmd[1];
}

int
acpm_dvfs_set(struct acpm_ch *c, u32 domain, u32 khz)
{
	u32 cmd[4] = { domain & 0xfff, khz, 0, 0 };	/* FREQ_REQ */
	return acpm_xfer(c, cmd);
}
