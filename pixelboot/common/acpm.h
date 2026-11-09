/*
 * ACPM: the firmware of the power-management core (APM), which owns the clocks and voltages of
 * the CPU clusters. Requests go through queues in its SRAM and a mailbox doorbell (acpm.c).
 */
struct acpm_ch {
	u32 id, mlen, qlen, poll;
	u64 tx_base, tx_front, tx_rear;		/* ours to the APM (its RX queue) */
	u64 rx_base, rx_front, rx_rear;		/* the APM's to us (its TX queue) */
};

void acpm_dump(void);				/* log the channel table */
int acpm_open(u32 id, struct acpm_ch *c);	/* the AP channel with this ID; 0 = found */
int acpm_xfer(struct acpm_ch *c, u32 cmd[4]);	/* send, wait for the reply (into cmd); 0 = replied */

/* DVFS (Linux: drivers/firmware/samsung/exynos-acpm-dvfs.c): rates in kHz */
#define ACPM_DVFS_CPUCL0	2
#define ACPM_DVFS_CPUCL1	3
#define ACPM_DVFS_CPUCL2	4
u32 acpm_dvfs_get(struct acpm_ch *c, u32 domain);	/* kHz, 0 = no answer */
int acpm_dvfs_set(struct acpm_ch *c, u32 domain, u32 khz);
