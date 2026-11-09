/*
 * redwood_bcm4389: the Pixel 7 Pro's Wi-Fi, a Broadcom BCM4389 on PCIe1 of gs201, as a station
 * for HackiOS (pixel7-ios WIFI-PLAN.md, W3). The steps were found bare, one probe at a time, in
 * pixelboot/wifiprobe..wifiprobe13 (the last joined a WPA2 network and got an address by DHCP);
 * this is them inside the kernel, behind the station boundary (sys/redwood_sta.h) and the network
 * link (sys/redwood_net.h) that redwood's own Wi-Fi uses.
 *
 * xnuboot powers the chip and trains the PCIe link before XNU starts (pixelboot/xnuboot/wifi.c,
 * built with WIFI_PCIE) and says so with the boot-arg bcmlink=1; nothing here touches PCIe space
 * without it, since an access with the link down faults. What the windows are:
 *   0x60ffe000  the chip's configuration space (the iATU's CFG0 window for bus 1)
 *   0x60000000  BAR0, 32 KB: a 4 KB window onto the chip's backplane (its base in config 0x80),
 *               and the PCIe core's own registers
 *   0x60400000  BAR2, 4 MB: the chip's RAM (TCM), 4 MB of its address space (base in config 0x84)
 * xnuboot also gives 1 MB of RAM below 4 GB for the chip's DMA (boot-arg bcmdma=), outside XNU's
 * memory: mapped uncached here, as all memory was in the probes, so nothing needs cache maintenance.
 *
 * Boot-arg bcmwifi=N does steps 1 to N:
 *   1  map the windows; read the PCI ids (14e4:4441) and, over the backplane, the chip id
 *   2  the firmware up: fw/fw_bcmdhd.bin, bcmdhd.cal (NVRAM) and bcmdhd_clm.blob of the small image
 *      (the phone's /vendor/firmware), the MsgBuf rings, the mailbox (deep sleep, as Android's bcmdhd
 *      answers it: DS_ACK, then device wake asserted for good), the MAC, the CLM, country US, WLC_UP;
 *      then a thread that keeps reading what the chip sends (it traps ~18 s after a deep-sleep
 *      request nobody answers)
 *   3  the station: scan (escan), join (as Android's bcmdhd joins on this phone: wpa_auth 0xc0,
 *      join_pref tuples, no wpaie; the four-way handshake by lx_wpa.c, HackiOS's own; the keys
 *      through "wsec_key"), leave; and en0's link: frames out on a TX flow ring, frames in from the
 *      RX ring. redwood_sta_attach() and redwood_net_attach(): wifid and Settings take it from there.
 *
 * Who touches the chip: the service thread alone reads what it sends (drain(), every 2 ms); the
 * station's calls (redwood_sta.c's thread, one at a time) and en0's transmit post to the rings under
 * bcm_lock and wait for what the service thread sees. Before the thread runs (step 2), whoever waits
 * drains itself.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/redwood_input.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/systm.h>

/* HackiOS's WPA2 arithmetic (wifi/lx/src), under names of its own here: the lx station may be built in too */
#define _KERNEL 1
#define lx_wpa_pmk bcm4389_wpa_pmk
#define lx_wpa_message bcm4389_wpa_message
#include "../../../../wifi/lx/src/lx_wpa.c"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
#ifndef VM_WIMG_IO
#define VM_WIMG_IO              0x7             /* guarded, coherent, not cacheable: what ml_io_map maps with */
#endif
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern void IOSleep(unsigned int milliseconds);

#define PCIE1_CFG_PHYS          0x60ffe000ULL
#define PCIE1_BAR0_PHYS         0x60000000ULL
#define PCIE1_BAR0_SIZE         0x8000
#define PCIE1_BAR2_PHYS         0x60400000ULL
#define PCIE1_BAR2_SIZE         0x400000
#define DMA_SIZE                0x100000

#define CFG_BAR0_WINDOW         0x80            /* the backplane address BAR0's first 4 KB show */
#define CFG_BAR2_WINDOW         0x84            /* the 4 MB of the chip's address space BAR2 shows */
#define CHIPC_BASE              0x18000000u     /* ChipCommon: its first word is the chip id */
#define PCIE2_BASE              0x18001000u
#define ARMCA7_WRAP             0x18120000u
#define SYSMEM_WRAP             0x18126000u
#define SICF_CPUHALT            0x0020u
#define RAM_BASE                0x00200000u
#define RAM_SIZE                0x002a0000u
#define SHARED_SLOT             (RAM_BASE + RAM_SIZE - 4)
#define RNG_SIG                 0xfeedc0deu
#define RNG_BYTES               128u

/*
 * BAR2 is mapped in slices, each when first needed: the chip's RAM (0x200000-0x4a0000) needs only
 * some of them. Mappings are ml_io_map_unmappable's, from kernel_map: ml_io_map's come from the
 * small io_submap, which ran out ("kmem_alloc(..., 4194304, ...): failed with 3" for all of BAR2 in
 * w3a, then for a 512 KB slice after the GPU's mappings and our DMA block in w3b).
 */
#define BAR2_SLICE              0x80000

static volatile uint32_t *cfg, *bar0;
static volatile uint32_t *bar2_slice[PCIE1_BAR2_SIZE / BAR2_SLICE];
static uint32_t bar0_window = 0xffffffffu, bar2_window = 0xffffffffu;

static LCK_GRP_DECLARE(bcm_lock_group, "bcm4389");
static LCK_MTX_DECLARE(bcm_lock, &bcm_lock_group);      /* the rings and their indices */
static LCK_MTX_DECLARE(bcm_ioctl_lock, &bcm_lock_group); /* one ioctl at a time */
static LCK_MTX_DECLARE(bcm_tcm_lock, &bcm_lock_group);  /* BAR2's window and what is read through it */

static uint32_t
cfg_rd(uint32_t off)
{
	return cfg[off / 4];
}

static void
cfg_wr(uint32_t off, uint32_t v)
{
	cfg[off / 4] = v;
}

/*
 * Move a BAR's window. The accesses through the BAR before it have to be out first, and the
 * configuration write has to be in before the next access: the read-back alone did not keep the
 * next BAR write behind it (they are different peripherals to the CPU). In w3c3 the first write
 * after each move still went through the old window: the NVRAM's first word to 0x9a298 (the "AXI
 * slave error" of every boot so far) and a ring descriptor's to 0x7b0144, past the RAM, which
 * trapped the firmware.
 */
static void
window_move(uint32_t reg, uint32_t window)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	cfg_wr(reg, window);
	for (int k = 0; k < 100 && cfg_rd(reg) != window; k++) {
		delay(1);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* a backplane register, through BAR0's 4 KB window */
static volatile uint32_t *
bp_word(uint32_t addr)
{
	uint32_t window = addr & ~0xfffu;

	if (window != bar0_window) {
		window_move(CFG_BAR0_WINDOW, window);
		bar0_window = window;
	}
	return &bar0[(addr & 0xfffu) / 4];
}

static uint32_t
bp_rd(uint32_t addr)
{
	return *bp_word(addr);
}

static void
bp_wr(uint32_t addr, uint32_t v)
{
	*bp_word(addr) = v;
}

/* a mapping of a physical range that need not start on a (16 KB) page */
static volatile uint32_t *
map(uint64_t phys, uint64_t size)
{
	uint64_t base = phys & ~(uint64_t)PAGE_MASK;
	uint64_t length = round_page(phys + size) - base;
	vm_offset_t va = ml_io_map_unmappable((vm_offset_t)base, (vm_size_t)length, VM_WIMG_IO);

	return va ? (volatile uint32_t *)(va + (phys - base)) : NULL;
}

/* where a word of the chip's address space is through BAR2 (its window moved there, its slice mapped) */
static volatile uint32_t *
tcm_word(uint32_t addr)
{
	uint32_t window = addr & ~(uint32_t)(PCIE1_BAR2_SIZE - 1), offset = addr & (PCIE1_BAR2_SIZE - 1);
	unsigned slice = offset / BAR2_SLICE;

	if (window != bar2_window) {
		window_move(CFG_BAR2_WINDOW, window);
		bar2_window = window;
	}
	if (bar2_slice[slice] == NULL) {
		bar2_slice[slice] = map(PCIE1_BAR2_PHYS + (uint64_t)slice * BAR2_SLICE, BAR2_SLICE);
	}
	return bar2_slice[slice] + (offset % BAR2_SLICE) / 4;
}

static uint32_t
tcm_rd(uint32_t addr)
{
	uint32_t v;

	lck_mtx_lock(&bcm_tcm_lock);
	v = *tcm_word(addr);
	lck_mtx_unlock(&bcm_tcm_lock);
	return v;
}

static void
tcm_wr(uint32_t addr, uint32_t v)
{
	lck_mtx_lock(&bcm_tcm_lock);
	*tcm_word(addr) = v;
	lck_mtx_unlock(&bcm_tcm_lock);
}

static uint8_t
tcm_rd8(uint32_t addr)
{
	return (uint8_t)(tcm_rd(addr & ~3u) >> (8 * (addr & 3u)));
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

/* an AXI core's wrapper: reset with these control bits held, then out of reset with those (brcmfmac) */
static void
core_reset(uint32_t wrap, uint32_t pre, uint32_t rst, uint32_t post)
{
	if ((bp_rd(wrap + 0x800) & 1u) == 0) {
		bp_wr(wrap + 0x408, pre | 3u);
		(void)bp_rd(wrap + 0x408);
		bp_wr(wrap + 0x800, 1u);
		delay(20);
	}
	bp_wr(wrap + 0x408, rst | 3u);
	(void)bp_rd(wrap + 0x408);
	delay(10);
	bp_wr(wrap + 0x800, 0u);
	for (int k = 0; k < 100 && (bp_rd(wrap + 0x800) & 1u); k++) {
		delay(1);
	}
	bp_wr(wrap + 0x408, post | 1u);
	(void)bp_rd(wrap + 0x408);
	delay(10);
}

/* ---- the firmware's console (RTE's log ring in its RAM), new text since the last call ---- */

static uint32_t console_addr, console_shown, shared_addr, rings_addr;
static unsigned ioctl_timeouts;

static void
show_console(void)
{
	char line[160];
	unsigned n = 0;
	uint32_t buf, size, idx;

	if (console_addr < RAM_BASE || console_addr >= SHARED_SLOT) {
		return;
	}
	buf = tcm_rd(console_addr + 0x08);
	size = tcm_rd(console_addr + 0x0c);
	idx = tcm_rd(console_addr + 0x10);
	if (buf < RAM_BASE || buf >= SHARED_SLOT || size == 0 || size > 0x40000 || idx >= size) {
		return;
	}
	for (uint32_t i = console_shown; i != idx; i = (i + 1) % size) {
		char c = (char)tcm_rd8(buf + i);

		if (c == '\n' || n == sizeof(line) - 1) {
			line[n] = 0;
			if (n) {
				printf("bcm4389: fw| %s\n", line);
			}
			n = 0;
		} else if (c >= 0x20 && c < 0x7f) {
			line[n++] = c;
		}
	}
	console_shown = idx;
}

/* ---- MsgBuf: the rings, in the DMA memory xnuboot set aside ---- */

/* the 1 MB block (offsets); it is uncached, so what the chip and we write may sit side by side */
#define DMA_SCRATCH     0x00000                 /* 64 */
#define DMA_H2D_WR      0x01000                 /* the write and read indices of every ring, 4 bytes each */
#define DMA_H2D_RD      0x02000
#define DMA_D2H_WR      0x03000
#define DMA_D2H_RD      0x03800
#define DMA_TRAP        0x04000                 /* 4 KB: extended trap data (1 KB overflowed, wifiprobe11) */
#define DMA_RING0       0x08000                 /* control submit, 256 x 40 */
#define DMA_RING1       0x0c000                 /* RX post, 64 x 32 */
#define DMA_RING2       0x0d000                 /* control complete, 256 x 24 */
#define DMA_RING3       0x0f000                 /* TX complete, 64 x 24 */
#define DMA_RING4       0x10000                 /* RX complete, 64 x 40 */
#define DMA_IOCTL_REQ   0x11000                 /* 4 KB */
#define DMA_FLOWRING    0x12000                 /* 8 x 4 KB, one per flow ring id: 64 x 48 each */
#define DMA_IOCTL_RESP  0x20000                 /* 8 x 8 KB */
#define DMA_EVENT       0x40000                 /* 8 x 8 KB (2 KB overflowed, wifiprobe11) */
#define DMA_RX          0x60000                 /* 32 x 2 KB */
#define DMA_TX          0x80000                 /* 64 x 2 KB, one per flow ring slot */

#define MSG_FLOW_RING_CREATE    0x03u
#define MSG_FLOW_RING_CREATE_C  0x04u
#define MSG_FLOW_RING_DELETE    0x05u
#define MSG_FLOW_RING_DELETE_C  0x06u
#define MSG_IOCTLPTR_REQ        0x09u
#define MSG_IOCTLPTR_REQ_ACK    0x0au
#define MSG_IOCTLRESP_BUF_POST  0x0bu
#define MSG_IOCTL_CMPLT         0x0cu
#define MSG_EVENT_BUF_POST      0x0du
#define MSG_WL_EVENT            0x0eu
#define MSG_TX_POST             0x0fu
#define MSG_TX_STATUS           0x10u
#define MSG_RXBUF_POST          0x11u
#define MSG_RX_CMPLT            0x12u
#define MSG_H2D_MAILBOX_DATA    0x23u
#define MSG_D2H_MAILBOX_DATA    0x24u
#define PHASE_INIT              0x80u
#define EPOCH_MODULO            253u
#define DOORBELL_MAGIC          0xdeadbeefu
#define HOSTREADY_MAGIC         0x12345678u
/*
 * Flow ring ids, a new one for each join in turn: the chip did not finish deleting the last one
 * (no MSG_FLOW_RING_DELETE_CMPLT in 1 s) and refused it again with BCMPCIE_RING_IN_USE (w3c4).
 */
#define FLOWRING_FIRST          2u              /* the first flow ring id (bcmdhd FLOWID_RESERVED) */
#define FLOWRINGS               8u

#define D2H_DEV_DS_ENTER_REQ    0x02u
#define D2H_DEV_DS_EXIT_NOTE    0x04u
#define H2D_HOST_DS_ACK         0x02u
#define H2DMB_DS_DEVICE_WAKE    0x40u

#define WLC_UP                  2u
#define WLC_DOWN                3u
#define WLC_DISASSOC            52u
#define WLC_GET_VAR             262u
#define WLC_SET_VAR             263u

/* brcmfmac's fweh.h: the events */
#define E_SET_SSID              0u
#define E_DEAUTH                5u
#define E_DEAUTH_IND            6u
#define E_DISASSOC_IND          12u
#define E_LINK                  16u
#define E_ASSOC_REQ_IE          87u
#define E_ESCAN_RESULT          69u
#define E_STATUS_PARTIAL        8u

static volatile uint8_t *dma;           /* the block, uncached */
static uint64_t dma_pa;
static int service_running;

static uint16_t r0_wr, r2_rd, r3_rd, r4_rd, r1_wr, fr_wr;
static uint8_t r0_phase = PHASE_INIT, r0_epoch = 1, r1_phase = PHASE_INIT, r1_epoch = 1;
static uint8_t fr_phase = PHASE_INIT, fr_epoch = 1;
static int ds_exit_wait;
static unsigned ds_acks, ds_exits, events_seen;

/* the ioctl under way, filled in by drain() */
static uint16_t trans_id, io_tid;
static volatile int io_done, io_status;
static uint8_t *io_out;
static uint32_t io_out_max, io_out_len;

static volatile uint8_t *
at(uint32_t off)
{
	return dma + off;
}

static uint64_t
pa(uint32_t off)
{
	return dma_pa + off;
}

static void
put16(volatile uint8_t *p, uint16_t v)
{
	*(volatile uint16_t *)p = v;
}

static void
put32(volatile uint8_t *p, uint32_t v)
{
	*(volatile uint32_t *)p = v;
}

static uint16_t
get16(volatile const uint8_t *p)
{
	return *(volatile const uint16_t *)p;
}

static uint32_t
get32(volatile const uint8_t *p)
{
	return *(volatile const uint32_t *)p;
}

/*
 * The doorbell after a ring's write index has moved, as bcmdhd rings it
 * (DHD_WRPTR_UPDATE_H2D_DB_MAGIC): 0xff, the ring's msgbuf id, the new index. The firmware tells
 * a write-pointer update by the 0xff. A flow ring's id is its flow id + 3 (DHD_FLOWID_TO_RINGID:
 * after the five common rings, from flow id 2). With brcmfmac's plain magic the firmware took 2
 * frames of a flow ring and no more, and moved its read index no further (w3c6).
 */
static void
doorbell(uint32_t ring, uint16_t index)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	bp_wr(PCIE2_BASE + 0x140, 0xff000000u | ring << 16 | index);
}

/* ring 0: the next slot, cleared, its phase bit (flipped each time the ring wraps) and epoch set */
static volatile uint8_t *
r0_slot(void)
{
	volatile uint8_t *p = at(DMA_RING0 + (uint32_t)r0_wr * 40);

	for (int k = 0; k < 40; k++) {
		p[k] = 0;
	}
	p[2] = r0_phase;
	p[3] = r0_epoch;
	r0_epoch = (uint8_t)((r0_epoch + 1) % EPOCH_MODULO);
	if (++r0_wr == 256) {
		r0_wr = 0;
		r0_phase ^= PHASE_INIT;
	}
	return p;
}

static void
r0_commit(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	put16(at(DMA_H2D_WR + 0 * 4), r0_wr);
	doorbell(0, r0_wr);
}

static void
post_buf(uint8_t type, uint32_t id, uint16_t len, uint64_t addr)
{
	volatile uint8_t *p = r0_slot();

	p[0] = type;
	put32(p + 4, id);
	put16(p + 8, len);
	put32(p + 0x10, (uint32_t)addr);
	put32(p + 0x14, (uint32_t)(addr >> 32));
}

static void
mailbox_send(uint32_t data)
{
	volatile uint8_t *p = r0_slot();

	p[0] = MSG_H2D_MAILBOX_DATA;
	put32(p + 8, data);
	r0_commit();
}

/* ring 1: RX buffers */
static void
rx_post(uint32_t n)
{
	volatile uint8_t *p = at(DMA_RING1 + (uint32_t)r1_wr * 32);
	uint64_t addr = pa(DMA_RX + n * 2048);

	for (int k = 0; k < 32; k++) {
		p[k] = 0;
	}
	p[0] = MSG_RXBUF_POST;
	p[2] = r1_phase;
	p[3] = r1_epoch;
	r1_epoch = (uint8_t)((r1_epoch + 1) % EPOCH_MODULO);
	put32(p + 4, 1001 + n);
	put16(p + 10, 2048);
	put32(p + 24, (uint32_t)addr);
	put32(p + 28, (uint32_t)(addr >> 32));
	if (++r1_wr == 64) {
		r1_wr = 0;
		r1_phase ^= PHASE_INIT;
	}
}

static void
r1_commit(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	put16(at(DMA_H2D_WR + 1 * 4), r1_wr);
	doorbell(1, r1_wr);
}

/* ---- what the chip says: events, frames ---- */

/* the station's state, as events change it */
static volatile int link_up, link_lost, set_ssid_status = -1, scan_done, scan_status;
static uint8_t assoc_rsn[64];
static unsigned assoc_rsn_len;

/* what the last scan heard */
#define HEARD_MOST              64
static struct redwood_sta_bss heard_bss[HEARD_MOST];
static uint64_t heard_when[HEARD_MOST];
static uint16_t heard_chanspec[HEARD_MOST];
static unsigned heard_count;

/* frames for en0, copied out of the RX buffers under the lock and handed on after it */
#define STAGED_MOST             16
static uint8_t staged[STAGED_MOST][1514];
static uint16_t staged_len[STAGED_MOST];
static unsigned staged_count;

/* the last EAPOL frame (what follows its Ethernet header), for the handshake */
static uint8_t eapol_buf[512];
static volatile unsigned eapol_len;
static volatile int eapol_pending;

/* TX: the flow ring's state */
static volatile int flow_status = -1, flow_deleted;
static int flow_up;
static uint32_t flow_id = FLOWRING_FIRST;
static uint64_t slot_posted[64];        /* when each slot of the flow ring was last posted */
#define FLOW_MEM                (DMA_FLOWRING + (flow_id - FLOWRING_FIRST) * 0x1000u)
static unsigned tx_posted, tx_statuses, tx_dropped, rx_frames, rx_unstaged, rx_big_said;
static uint8_t own_mac[6], ap_mac[6];
static volatile int trapped;            /* the firmware has (PCIE_SHARED_TRAP): nothing answers any more */
static volatile uint32_t tx_done_id;
static volatile int tx_done_status = -1;
static uint32_t tx_next_id = 0x3001;

static struct lx_wpa wpa;               /* the handshake's state, kept for group rekeys */
static int joined;

static uint16_t
be16(volatile const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t
be32(volatile const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t
le16(volatile const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
le32(volatile const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* d11ac chanspec: band 15:14, bandwidth 13:11, control sideband 10:8, centre channel 7:0 */
static unsigned
chanspec_control(uint16_t cs)
{
	unsigned ch = cs & 0xff, sb = (cs >> 8) & 7, half;

	switch (cs & 0x3800) {
	case 0x1800: half = 2; break;
	case 0x2000: half = 6; break;
	case 0x2800: half = 14; break;
	default: return ch;
	}
	return ch - half + 4 * sb;
}

static uint16_t
chanspec_20(uint16_t cs)
{
	return (uint16_t)((cs & 0xc000) | 0x1000 | chanspec_control(cs));
}

static unsigned
chanspec_frequency(uint16_t cs)
{
	unsigned ch = chanspec_control(cs);

	if ((cs & 0xc000) == 0xc000) {
		return 5000 + 5 * ch;
	}
	return ch == 14 ? 2484 : 2407 + 5 * ch;
}

static uint16_t
frequency_chanspec(unsigned mhz)
{
	if (mhz >= 5000) {
		return (uint16_t)(0xc000 | 0x1000 | ((mhz - 5000) / 5));
	}
	return (uint16_t)(0x1000 | (mhz == 2484 ? 14 : (mhz - 2407) / 5));
}

/* one record of an escan result, struct brcmf_bss_info_le, into what the last scan heard */
static void
heard(volatile const uint8_t *bi, volatile const uint8_t *end)
{
	uint32_t len = le32(bi + 4), ie_off = le16(bi + 116), ie_len = le32(bi + 120);
	struct redwood_sta_bss *b = NULL;
	unsigned i;

	if (bi + 128 > end || len < 128 || bi + len > end) {
		return;
	}
	for (i = 0; i < heard_count; i++) {
		int same = 1;

		for (int k = 0; k < 6; k++) {
			same = same && heard_bss[i].bssid[k] == bi[8 + k];
		}
		if (same) {
			b = &heard_bss[i];
			break;
		}
	}
	if (b == NULL) {
		if (heard_count == HEARD_MOST) {
			return;
		}
		i = heard_count++;
		b = &heard_bss[i];
	}
	bzero(b, sizeof(*b));
	for (int k = 0; k < 6; k++) {
		b->bssid[k] = bi[8 + k];
	}
	b->ssid_length = bi[18] > 32 ? 32 : bi[18];
	for (int k = 0; k < b->ssid_length; k++) {
		b->ssid[k] = bi[19 + k];
	}
	heard_chanspec[i] = le16(bi + 72);
	b->frequency = (uint16_t)chanspec_frequency(heard_chanspec[i]);
	b->rssi = (int16_t)le16(bi + 78);
	b->beacon_interval = le16(bi + 14);
	b->capability = le16(bi + 16);
	if (ie_off < len && ie_len <= len - ie_off && ie_len <= REDWOOD_STA_ELEMENTS_MOST) {
		b->elements_length = (uint16_t)ie_len;
		for (uint32_t k = 0; k < ie_len; k++) {
			b->elements[k] = bi[ie_off + k];
		}
	}
	heard_when[i] = now_us();
}

/* an event: an Ethernet header of type 0x886c, Broadcom's 10 bytes, then brcmf_event_msg_be (big endian) */
static void
event_seen(volatile const uint8_t *buf, uint16_t len)
{
	unsigned e;

	for (e = 0; e <= 64 && e + 72u <= len; e += 2) {
		if (buf[e + 12] == 0x88 && buf[e + 13] == 0x6c) {
			break;
		}
	}
	if (e > 64 || e + 72u > len) {
		return;
	}

	volatile const uint8_t *m = buf + e + 24, *data = m + 48, *end = buf + len;
	uint32_t type = be32(m + 4), status = be32(m + 8), reason = be32(m + 12), datalen = be32(m + 20);
	uint16_t flags = be16(m + 2);

	if (data + datalen < end) {
		end = data + datalen;
	}
	events_seen++;
	if (type == E_ESCAN_RESULT) {
		if (status != E_STATUS_PARTIAL) {
			scan_status = (int)status;
			scan_done = 1;
		} else if (data + 12 <= end) {
			volatile const uint8_t *bi = data + 12;

			for (uint16_t n = le16(data + 10); n && bi + 8 <= end; n--) {
				uint32_t bl = le32(bi + 4);

				heard(bi, end);
				if (bl < 128) {
					break;
				}
				bi += bl;
			}
		}
		return;
	}
	/* (each console line can hold the window server up 10-50 ms: the first ones, and the link's always) */
	if (events_seen < 60 || type == E_LINK || type == E_DEAUTH || type == E_DEAUTH_IND || type == E_DISASSOC_IND) {
		printf("bcm4389: event %u status %u reason %u flags %#x\n", type, status, reason, flags);
	}
	switch (type) {
	case E_SET_SSID:
		set_ssid_status = (int)status;
		break;
	case E_LINK:
	case E_DEAUTH:
	case E_DEAUTH_IND:
	case E_DISASSOC_IND:
		if (type == E_LINK && (flags & 1)) {
			link_up = 1;
			break;
		}
		if (link_up) {
			link_lost = 1;
		}
		link_up = 0;
		break;
	case E_ASSOC_REQ_IE:
		for (volatile const uint8_t *ie = data; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
			if (ie[0] == 48 && 2u + ie[1] <= sizeof(assoc_rsn)) {
				for (unsigned k = 0; k < 2u + ie[1]; k++) {
					assoc_rsn[k] = ie[k];
				}
				assoc_rsn_len = 2u + ie[1];
			}
		}
		break;
	}
}

/* a received 802.3 frame: EAPOL for the handshake, anything else for en0 once joined */
static void
frame_seen(volatile const uint8_t *f, uint16_t len, uint16_t flags)
{
	if (!(flags & 1) || len < 14 || len > 1514) {
		return;
	}
	rx_frames++;
	/* (the first big ones: who they are for and what they are, no addresses; w3c6 and w3c7 took 15 MB in nobody asked for) */
	if (len > 1200 && rx_big_said < 6) {
		bool to_us = true, bcast = true;
		uint16_t type = (uint16_t)(f[12] << 8 | f[13]);

		for (int k = 0; k < 6; k++) {
			to_us = to_us && f[k] == own_mac[k];
			bcast = bcast && f[k] == 0xff;
		}
		rx_big_said++;
		printf("bcm4389: RX %u bytes, %s, type %#x, IP protocol %u, from port %u (frame %u)\n", len, to_us ? "to us" :
		    bcast ? "broadcast" : (f[0] & 1) ? "multicast" : "TO ANOTHER STATION", type,
		    type == 0x0800 && len > 34 ? f[23] : 0, type == 0x0800 && len > 36 ? (unsigned)(f[34] << 8 | f[35]) : 0,
		    rx_frames);
	}
	if (f[12] == 0x88 && f[13] == 0x8e) {
		if (len - 14u <= sizeof(eapol_buf)) {
			for (unsigned k = 0; k < len - 14u; k++) {
				eapol_buf[k] = f[14 + k];
			}
			eapol_len = len - 14u;
			eapol_pending = 1;
		}
		return;
	}
	if (joined && staged_count < STAGED_MOST) {
		for (unsigned k = 0; k < len; k++) {
			staged[staged_count][k] = f[k];
		}
		staged_len[staged_count++] = len;
	} else if (joined) {
		rx_unstaged++;
	}
}

/* everything the chip has completed, on rings 2, 3 and 4. bcm_lock held */
static void
drain(void)
{
	uint16_t wr = get16(at(DMA_D2H_WR + 0 * 4));
	int r0_posted = 0;

	while (r2_rd != wr) {
		volatile uint8_t *msg = at(DMA_RING2 + (uint32_t)r2_rd * 24);
		uint32_t id = get32(msg + 4);

		__asm__ volatile ("dmb sy" ::: "memory");
		switch (msg[0]) {
		case MSG_IOCTL_CMPLT:
			if (get16(msg + 14) == io_tid && !io_done) {
				uint32_t n = get16(msg + 12);

				io_status = (int16_t)get16(msg + 8);
				if (io_out && id >= 1 && id <= 8) {
					volatile uint8_t *src = at(DMA_IOCTL_RESP + (id - 1) * 8192);

					for (uint32_t c = 0; c < n && c < io_out_max; c++) {
						io_out[c] = src[c];
					}
				}
				io_out_len = n;
				io_done = 1;
			}
			if (id >= 1 && id <= 8) {
				post_buf(MSG_IOCTLRESP_BUF_POST, id, 8192, pa(DMA_IOCTL_RESP + (id - 1) * 8192));
				r0_posted = 1;
			}
			break;
		case MSG_WL_EVENT:
			if (id >= 101 && id <= 108) {
				event_seen(at(DMA_EVENT + (id - 101) * 8192), get16(msg + 12));
				post_buf(MSG_EVENT_BUF_POST, id, 8192, pa(DMA_EVENT + (id - 101) * 8192));
				r0_posted = 1;
			}
			break;
		case MSG_D2H_MAILBOX_DATA: {
			uint32_t data = get32(msg + 12);

			if (data & D2H_DEV_DS_ENTER_REQ) {
				mailbox_send(H2D_HOST_DS_ACK);
				mailbox_send(H2DMB_DS_DEVICE_WAKE);
				ds_acks++;
				ds_exit_wait = 1;
			}
			if (data & D2H_DEV_DS_EXIT_NOTE) {
				ds_exits++;
				ds_exit_wait = 0;
			}
			break;
		}
		case MSG_FLOW_RING_CREATE_C:
			flow_status = get16(msg + 8);
			break;
		case MSG_FLOW_RING_DELETE_C:
			flow_deleted = 1;
			break;
		case MSG_IOCTLPTR_REQ_ACK:
			break;
		default:
			printf("bcm4389: a control completion of type %#x\n", msg[0]);
		}
		if (++r2_rd == 256) {
			r2_rd = 0;
		}
		put16(at(DMA_D2H_RD + 0 * 4), r2_rd);
		wr = get16(at(DMA_D2H_WR + 0 * 4));
	}
	if (r0_posted) {
		r0_commit();
	}

	/* ring 3: TX statuses (host_txbuf_cmpl_t: our packet id, the completion's status) */
	wr = get16(at(DMA_D2H_WR + 1 * 4));
	while (r3_rd != wr) {
		volatile uint8_t *c = at(DMA_RING3 + (uint32_t)r3_rd * 24);

		tx_done_id = get32(c + 4);
		tx_done_status = get16(c + 8);
		if (tx_statuses++ < 16) {
			printf("bcm4389: TX status: frame %#x, status %u, flow ring %u (%u posted)\n", tx_done_id,
			    tx_done_status, get16(c + 10), tx_posted);
		}
		if (++r3_rd == 64) {
			r3_rd = 0;
		}
		put16(at(DMA_D2H_RD + 1 * 4), r3_rd);
		wr = get16(at(DMA_D2H_WR + 1 * 4));
	}

	/* ring 4: RX completions; every buffer is posted again */
	wr = get16(at(DMA_D2H_WR + 2 * 4));
	if (r4_rd != wr) {
		while (r4_rd != wr) {
			volatile uint8_t *c = at(DMA_RING4 + (uint32_t)r4_rd * 40);
			uint32_t id = get32(c + 4);

			if (id >= 1001 && id < 1001 + 32) {
				uint16_t len = get16(c + 14), off = get16(c + 16);

				if (get16(c + 8) == 0 && off < 2048 && len <= 2048 - off) {
					frame_seen(at(DMA_RX + (id - 1001) * 2048 + off), len, get16(c + 18));
				}
				rx_post(id - 1001);
			}
			if (++r4_rd == 64) {
				r4_rd = 0;
			}
			put16(at(DMA_D2H_RD + 2 * 4), r4_rd);
			wr = get16(at(DMA_D2H_WR + 2 * 4));
		}
		r1_commit();
	}
}

/*
 * wait, draining as we do: the service thread drains too, but it may be the one waiting (a group
 * rekey's "wsec_key"), and before step 2 ends there is no thread
 */
static void
wait_a_little(void)
{
	lck_mtx_lock(&bcm_lock);
	drain();
	lck_mtx_unlock(&bcm_lock);
	if (service_running) {
		IOSleep(1);
	} else {
		delay(100);
	}
}

/* one ioctl; its status (0 is success), -100 when the chip never answers */
static int
wl_ioctl(uint32_t cmd, const void *in, uint32_t in_len, void *out, uint32_t out_max, uint32_t *out_len)
{
	volatile uint8_t *req = at(DMA_IOCTL_REQ), *p;
	uint32_t max = out_max > 256 ? out_max : 256;
	int status = -100;

	if (trapped) {
		return -100;                    /* (no 3 s wait for each: wifid asks often) */
	}
	lck_mtx_lock(&bcm_ioctl_lock);
	/* device wake asserted: nothing until the chip says it is out of deep sleep */
	for (int i = 0; ds_exit_wait && i < 500; i++) {
		wait_a_little();
	}
	ds_exit_wait = 0;

	lck_mtx_lock(&bcm_lock);
	for (uint32_t k = 0; k < in_len && k < 4096; k++) {
		req[k] = ((const uint8_t *)in)[k];
	}
	io_tid = ++trans_id;
	io_done = 0;
	io_out = out;
	io_out_max = out_max;
	io_out_len = 0;
	p = r0_slot();
	p[0] = MSG_IOCTLPTR_REQ;
	put32(p + 4, 0xfffe);
	put32(p + 8, cmd);
	put16(p + 12, io_tid);
	put16(p + 14, (uint16_t)in_len);
	put16(p + 16, (uint16_t)(max > 8192 ? 8192 : max));
	put32(p + 0x18, (uint32_t)pa(DMA_IOCTL_REQ));
	put32(p + 0x1c, (uint32_t)(pa(DMA_IOCTL_REQ) >> 32));
	r0_commit();
	lck_mtx_unlock(&bcm_lock);

	for (uint64_t t0 = now_us(); now_us() - t0 < 3000000;) {
		if (io_done) {
			status = io_status;
			break;
		}
		wait_a_little();
	}
	lck_mtx_lock(&bcm_lock);
	io_out = NULL;
	io_done = 1;
	lck_mtx_unlock(&bcm_lock);
	if (status == -100 && ioctl_timeouts++ < 20) {
		printf("bcm4389: ioctl %u: no answer in 3 s\n", cmd);
	}
	if (out_len) {
		*out_len = io_out_len;
	}
	lck_mtx_unlock(&bcm_ioctl_lock);
	return status;
}

/* an iovar: its name, the NUL, then the data */
static int
iovar(int set, const char *name, const void *data, uint32_t len, void *out, uint32_t out_max)
{
	uint8_t buf[1100];                      /* on the stack: two threads may call (a group rekey) */
	uint32_t n = 0, got = 0;

	while (name[n]) {
		buf[n] = (uint8_t)name[n];
		n++;
	}
	buf[n++] = 0;
	for (uint32_t k = 0; k < len && n < sizeof(buf); k++) {
		buf[n++] = data ? ((const uint8_t *)data)[k] : 0;
	}
	return wl_ioctl(set ? WLC_SET_VAR : WLC_GET_VAR, buf, n, out, out_max, &got);
}

static int
iovar_int(const char *name, int32_t v)
{
	return iovar(1, name, &v, 4, NULL, 0);
}

/* the CLM (the regulatory data) in chunks of 1 KB: "clmload", dload header flags BEGIN/END, type CLM */
static int
clm_load(const uint8_t *clm, uint64_t size)
{
	static uint8_t chunk[12 + 1024];

	for (uint64_t off = 0; off < size; off += 1024) {
		uint32_t n = (uint32_t)(size - off > 1024 ? 1024 : size - off), len = (12 + n + 7) & ~7u;
		uint16_t flag = (uint16_t)((1u << 12) | (off == 0 ? 2u : 0u) | (off + n == size ? 4u : 0u));
		int err;

		bzero(chunk, sizeof(chunk));
		chunk[0] = (uint8_t)flag;
		chunk[1] = (uint8_t)(flag >> 8);
		chunk[2] = 2;                           /* DL_TYPE_CLM */
		chunk[4] = (uint8_t)n;
		chunk[5] = (uint8_t)(n >> 8);
		memcpy(chunk + 12, clm + off, n);
		err = iovar(1, "clmload", chunk, len, NULL, 0);
		if (err) {
			return err;
		}
	}
	return 0;
}

/* NVRAM text as the firmware wants it: lines as NUL-terminated strings, no comments or blank lines */
static uint32_t
nvram_format(const uint8_t *src, uint64_t len, uint8_t *out, uint32_t max)
{
	uint32_t n = 0, column = 0;
	int comment = 0;

	for (uint64_t i = 0; i + 1 < len && n < max - 8; i++) {
		uint8_t c = src[i];

		if (c == '\r') {
			continue;
		}
		if (comment && c != '\n') {
			continue;
		}
		comment = 0;
		if (c == '#') {
			comment = 1;
		} else if (c == '\n') {
			if (column) {
				out[n++] = 0;
				column = 0;
			}
		} else {
			out[n++] = c;
			column++;
		}
	}
	while (n & 3) {
		out[n++] = 0;
	}
	out[n++] = 0;
	while (n & 3) {
		out[n++] = 0;
	}
	return n;
}

/* ---- TX: one flow ring, to the access point ---- */

/* the flow ring for the AP (bcmdhd's tx_flowring_create_request_t), on the next id; bcm_lock not held */
static int
flowring_up(const uint8_t *da)
{
	static unsigned turn;
	volatile uint8_t *p;

	lck_mtx_lock(&bcm_lock);
	flow_id = FLOWRING_FIRST + turn++ % FLOWRINGS;
	for (uint32_t k = 0; k < 64 * 48; k++) {
		at(FLOW_MEM)[k] = 0;
	}
	fr_wr = 0;
	fr_phase = PHASE_INIT;
	fr_epoch = 1;
	bzero(slot_posted, sizeof(slot_posted));
	put16(at(DMA_H2D_WR + flow_id * 4), 0);
	put16(at(DMA_H2D_RD + flow_id * 4), 0);
	flow_status = -1;
	p = r0_slot();
	p[0] = MSG_FLOW_RING_CREATE;
	for (int k = 0; k < 6; k++) {
		p[8 + k] = da[k];
		p[14 + k] = own_mac[k];
	}
	put16(p + 22, (uint16_t)flow_id);
	put16(p + 28, 64);
	put16(p + 30, 48);
	put32(p + 32, (uint32_t)pa(FLOW_MEM));
	put32(p + 36, (uint32_t)(pa(FLOW_MEM) >> 32));
	r0_commit();
	lck_mtx_unlock(&bcm_lock);
	for (uint64_t t0 = now_us(); flow_status < 0 && now_us() - t0 < 2000000;) {
		wait_a_little();
	}
	flow_up = flow_status == 0;
	printf("bcm4389: flow ring %u: created, status %d\n", flow_id, flow_status);
	return flow_up;
}

static void
flowring_down(void)
{
	volatile uint8_t *p;

	if (!flow_up) {
		return;
	}
	lck_mtx_lock(&bcm_lock);
	flow_up = 0;
	flow_deleted = 0;
	p = r0_slot();
	p[0] = MSG_FLOW_RING_DELETE;
	put16(p + 8, (uint16_t)flow_id);
	r0_commit();
	lck_mtx_unlock(&bcm_lock);
	for (uint64_t t0 = now_us(); !flow_deleted && now_us() - t0 < 1000000;) {
		wait_a_little();
	}
	printf("bcm4389: flow ring %u: %s\n", flow_id, flow_deleted ? "deleted" : "not deleted after 1 s");
}

/* one 802.3 frame on the flow ring (host_txbuf_post_t: the Ethernet header in the item); bcm_lock held */
static uint32_t
tx_post(const uint8_t *frame, uint32_t len)
{
	volatile uint8_t *p = at(FLOW_MEM + (uint32_t)fr_wr * 48), *buf = at(DMA_TX + (uint32_t)fr_wr * 2048);
	uint32_t id = tx_next_id++;

	for (uint32_t k = 14; k < len; k++) {
		buf[k - 14] = frame[k];
	}
	for (int k = 0; k < 48; k++) {
		p[k] = 0;
	}
	p[0] = MSG_TX_POST;
	p[2] = fr_phase;
	p[3] = fr_epoch;
	fr_epoch = (uint8_t)((fr_epoch + 1) % EPOCH_MODULO);
	put32(p + 4, id);
	for (int k = 0; k < 14; k++) {
		p[8 + k] = frame[k];
	}
	p[22] = 0x01;                           /* BRCMF_MSGBUF_PKT_FLAGS_FRAME_802_3 */
	p[23] = 1;                              /* seg_cnt */
	put32(p + 32, (uint32_t)pa(DMA_TX + (uint32_t)fr_wr * 2048));
	put32(p + 36, (uint32_t)(pa(DMA_TX + (uint32_t)fr_wr * 2048) >> 32));
	put16(p + 42, (uint16_t)(len - 14));
	slot_posted[fr_wr] = now_us();
	if (++fr_wr == 64) {
		fr_wr = 0;
		fr_phase ^= PHASE_INIT;
	}
	tx_posted++;
	__asm__ volatile ("dsb sy" ::: "memory");
	put16(at(DMA_H2D_WR + flow_id * 4), fr_wr);
	doorbell(flow_id + 3, fr_wr);
	return id;
}

/* en0's transmit: a frame from the network stack */
static bool
link_transmit(mbuf_t packet, size_t length)
{
	static uint8_t frame[1514];

	bool sent = false;

	if (!flow_up || !joined || length < 14 || length > sizeof(frame)) {
		return false;
	}
	if (mbuf_copydata(packet, 0, length, frame) != 0) {
		return false;
	}
	/*
	 * room by the ring's read index, not by TX statuses: none came for broadcasts (wifiprobe13). A
	 * buffer is used again 64 frames later, at least 16 after the chip has taken its item.
	 */
	/*
	 * ... and by age: the firmware took frames well past its read index (w3c7: a DNS answer came
	 * back with the index still at 2), so a slot posted 100 ms ago is taken to be free too.
	 */
	lck_mtx_lock(&bcm_lock);
	if ((uint16_t)(fr_wr + 64 - get16(at(DMA_H2D_RD + flow_id * 4))) % 64 < 48 ||
	    now_us() - slot_posted[fr_wr] > 100000) {
		(void)tx_post(frame, (uint32_t)length);
		sent = true;
	} else {
		tx_dropped++;
	}
	lck_mtx_unlock(&bcm_lock);
	return sent;
}

static struct redwood_net_link bcm_link = {
	.name = "bcm4389",
	.wireless = true,
	.up = false,
	.transmit = link_transmit,
	.poll = NULL,
};

/* ---- the station ---- */

static volatile enum redwood_sta_phase phase = REDWOOD_STA_OFF;
static struct redwood_sta_bss joined_bss;
static int last_error;

static int sta_leave(void);

static int
sta_power(bool on)
{
	int err;

	if (!on && (joined || link_up)) {
		(void)sta_leave();
	}
	err = wl_ioctl(on ? WLC_UP : WLC_DOWN, NULL, 0, NULL, 0, NULL);
	printf("bcm4389: power %s: status %d\n", on ? "on" : "off", err);
	if (err == 0) {
		phase = on ? REDWOOD_STA_IDLE : REDWOOD_STA_OFF;
	}
	return err ? EIO : 0;
}

/* escan, struct brcmf_escan_params_le with brcmf_scan_params_v2_le (wifiprobe9); until its last event */
static int
sta_scan(const struct redwood_sta_scan *what)
{
	static uint8_t p[8 + 72 + 2 * 64 + 36 + 4];
	uint8_t *v2 = p + 8;
	/* always one SSID, as wifiprobe9 asked: the empty one is any network */
	unsigned nch = what->frequency_count > 64 ? 64 : what->frequency_count, nssid = 1;
	uint32_t ssid_at = (72 + 2 * nch + 3) & ~3u, size = 8 + ssid_at + 36 * nssid;
	int err;

	bzero(p, sizeof(p));
	p[0] = 2;                               /* BRCMF_ESCAN_REQ_VERSION_V2 */
	p[4] = 1;                               /* WL_ESCAN_ACTION_START */
	p[6] = 0x34;                            /* sync id */
	v2[0] = 2;                              /* BRCMF_SCAN_PARAMS_VERSION_V2 */
	v2[2] = (uint8_t)(size - 8);
	v2[3] = (uint8_t)((size - 8) >> 8);
	memset(v2 + 40, 0xff, 6);               /* any BSSID */
	v2[46] = 2;                             /* DOT11_BSSTYPE_ANY */
	v2[48] = what->passive ? 1 : 0;
	memset(v2 + 52, 0xff, 16);              /* nprobes, active, passive, home time: the firmware's own */
	if (what->dwell_ms) {
		uint32_t d = what->dwell_ms;

		memcpy(v2 + (what->passive ? 60 : 56), &d, 4);
	}
	for (unsigned i = 0; i < nch; i++) {
		uint16_t cs = frequency_chanspec(what->frequencies[i]);

		v2[72 + 2 * i] = (uint8_t)cs;
		v2[73 + 2 * i] = (uint8_t)(cs >> 8);
	}
	{
		uint32_t l = what->ssid_length > 32 ? 32 : what->ssid_length;

		memcpy(v2 + ssid_at, &l, 4);
		memcpy(v2 + ssid_at + 4, what->ssid, l);
	}
	v2[68] = (uint8_t)nch;
	v2[70] = (uint8_t)nssid;

	phase = REDWOOD_STA_SCANNING;
	lck_mtx_lock(&bcm_lock);
	heard_count = 0;
	scan_done = 0;
	lck_mtx_unlock(&bcm_lock);
	(void)iovar_int("mpc", 0);
	err = iovar(1, "escan", p, size, NULL, 0);
	for (uint64_t t0 = now_us(); err == 0 && !scan_done && now_us() - t0 < 12000000;) {
		IOSleep(10);
	}
	phase = joined ? REDWOOD_STA_JOINED : REDWOOD_STA_IDLE;
	printf("bcm4389: scan: escan %d, status %d, %u heard\n", err, scan_done ? scan_status : -1, heard_count);
	return err ? EIO : !scan_done ? ETIMEDOUT : 0;
}

static void
sta_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	uint64_t now = now_us();

	for (unsigned i = 0; i < heard_count; i++) {
		heard_bss[i].age_ms = (uint32_t)((now - heard_when[i]) / 1000);
		each(&heard_bss[i], context);
	}
}

/* a key for the firmware: "wsec_key" with wl_wsec_key_t, CRYPTO_ALGO_AES_CCM (wifiprobe12) */
static int
set_key(uint32_t index, const uint8_t *key, uint32_t len, const uint8_t *ea, const uint8_t *rsc)
{
	uint8_t k[164];
	int err;

	bzero(k, sizeof(k));
	memcpy(k + 0, &index, 4);
	memcpy(k + 4, &len, 4);
	memcpy(k + 8, key, len > 32 ? 32 : len);
	k[112] = 4;
	if (rsc) {
		uint32_t hi = (uint32_t)rsc[2] | (uint32_t)rsc[3] << 8 | (uint32_t)rsc[4] << 16 | (uint32_t)rsc[5] << 24;
		uint16_t lo = (uint16_t)(rsc[0] | rsc[1] << 8);
		uint32_t one = 1;

		memcpy(k + 140, &hi, 4);
		memcpy(k + 144, &lo, 2);
		memcpy(k + 132, &one, 4);
	}
	if (ea) {
		memcpy(k + 156, ea, 6);
	}
	err = iovar(1, "wsec_key", k, sizeof(k), NULL, 0);
	bzero(k, sizeof(k));
	bzero((void *)(uintptr_t)at(DMA_IOCTL_REQ), 256);
	return err;
}

/* send an EAPOL reply and wait for its TX status; that status, or -1 */
static int
eapol_send(const uint8_t *body, unsigned len)
{
	uint8_t frame[14 + LX_WPA_REPLY_MOST];
	uint32_t id;

	memcpy(frame, ap_mac, 6);
	memcpy(frame + 6, own_mac, 6);
	frame[12] = 0x88;
	frame[13] = 0x8e;
	memcpy(frame + 14, body, len);
	lck_mtx_lock(&bcm_lock);
	tx_done_status = -1;
	id = tx_post(frame, 14 + len);
	lck_mtx_unlock(&bcm_lock);
	for (uint64_t t0 = now_us(); now_us() - t0 < 1000000;) {
		if (tx_done_status >= 0 && tx_done_id == id) {
			return tx_done_status;
		}
		wait_a_little();
	}
	return -1;
}

/* the handshake's messages as they come: M1 -> M2, M3 -> M4 and the keys, later group rekeys */
static int
eapol_handle(void)
{
	static uint8_t reply[LX_WPA_REPLY_MOST], body[512];
	unsigned rlen = 0, len;
	const char *why = "";
	int r;

	lck_mtx_lock(&bcm_lock);
	len = eapol_len;
	memcpy(body, eapol_buf, len);
	eapol_pending = 0;
	lck_mtx_unlock(&bcm_lock);
	r = bcm4389_wpa_message(&wpa, body, len, reply, &rlen, &why);
	if (r < 0) {
		printf("bcm4389: an EAPOL frame not answered: %s\n", why);
	}
	if (r <= 0 || rlen == 0) {
		return r;
	}
	if (eapol_send(reply, rlen) < 0) {
		printf("bcm4389: no TX status for the EAPOL reply\n");
	}
	if (r == LX_WPA_KEYS) {
		int e1 = set_key(0, wpa.pairwise, 16, ap_mac, NULL);
		int e2 = set_key(wpa.group_index, wpa.group, wpa.group_length, NULL, wpa.group_counter);
		int32_t wsec = 0;

		(void)iovar(0, "wsec", NULL, 4, &wsec, 4);
		(void)iovar_int("wsec", wsec | 4);
		printf("bcm4389: keys set: pairwise %d, group %u: %d\n", e1, wpa.group_index, e2);
	} else if (r == LX_WPA_GROUP) {
		printf("bcm4389: group rekey: %d\n", set_key(wpa.group_index, wpa.group, wpa.group_length, NULL,
		    wpa.group_counter));
	}
	return r;
}

static int
sta_leave(void)
{
	uint8_t scb[12];

	if (joined) {
		joined = 0;
		redwood_net_link_state(false);
	}
	bzero(scb, sizeof(scb));
	scb[0] = 3;                             /* WLAN_REASON_DEAUTH_LEAVING */
	memcpy(scb + 4, ap_mac, 6);
	(void)wl_ioctl(WLC_DISASSOC, scb, sizeof(scb), NULL, 0, NULL);
	flowring_down();
	link_up = 0;
	link_lost = 0;
	bzero(&wpa, sizeof(wpa));
	if (phase != REDWOOD_STA_OFF) {
		phase = REDWOOD_STA_IDLE;
	}
	return 0;
}

/*
 * As Android's bcmdhd joins on this phone (WIFI-PLAN.md, wifiprobe11p): wpa_auth 0xc0, auth open,
 * mfp capable, "join_pref" with the PSK/CCMP tuple, no wsec and no wpaie (the firmware builds the RSN
 * IE), then "join" (wl_extjoin_params_v1) by name, the BSSID if given, the 20 MHz channel if heard.
 */
static int
sta_join(const struct redwood_sta_join *what)
{
	static const uint8_t pref[16] = { 2, 14, 0, 1, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x0f, 0xac, 0x04, 0x00, 0x0f, 0xac, 0x04 };
	static uint8_t p[80];
	uint16_t cs = 0;
	bool any = true;
	int err;

	if (joined || link_up) {
		(void)sta_leave();
	}
	bzero(&joined_bss, sizeof(joined_bss));
	for (int k = 0; k < 6; k++) {
		any = any && what->bssid[k] == 0;
	}
	/* the access point of that name heard best; if none was heard, a scan for the name first (a join at boot) */
	for (int pass = 0; pass < 2 && cs == 0; pass++) {
		if (pass == 1) {
			static struct redwood_sta_scan named;

			bzero(&named, sizeof(named));
			memcpy(named.ssid, what->ssid, what->ssid_length);
			named.ssid_length = what->ssid_length;
			(void)sta_scan(&named);
		}
		for (unsigned i = 0; i < heard_count; i++) {
			if (heard_bss[i].ssid_length == what->ssid_length &&
			    memcmp(heard_bss[i].ssid, what->ssid, what->ssid_length) == 0 &&
			    (any || memcmp(heard_bss[i].bssid, what->bssid, 6) == 0) &&
			    (cs == 0 || heard_bss[i].rssi > joined_bss.rssi)) {
				joined_bss = heard_bss[i];
				cs = chanspec_20(heard_chanspec[i]);
			}
		}
	}
	if (cs == 0) {
		memcpy(joined_bss.ssid, what->ssid, what->ssid_length);
		joined_bss.ssid_length = what->ssid_length;
	}

	bzero(&wpa, sizeof(wpa));
	if (what->security == REDWOOD_STA_WPA2_PSK) {
		if (what->passphrase_length) {
			bcm4389_wpa_pmk(what->passphrase, what->passphrase_length, what->ssid, what->ssid_length, wpa.pmk);
		} else {
			memcpy(wpa.pmk, what->pmk, 32);
		}
		(void)iovar_int("wpa_auth", 0xc0);
		(void)iovar_int("auth", 0);
		(void)iovar_int("mfp", 1);
		(void)iovar(1, "join_pref", pref, sizeof(pref), NULL, 0);
	} else {
		(void)iovar_int("wpa_auth", 0);
		(void)iovar_int("wsec", 0);
		(void)iovar_int("auth", 0);
		(void)iovar_int("mfp", 0);
	}

	bzero(p, sizeof(p));
	{
		uint32_t l = what->ssid_length;

		memcpy(p + 4, &l, 4);
		memcpy(p + 8, what->ssid, what->ssid_length);
	}
	p[40] = 0xff;                           /* scan_type: the firmware's own */
	{
		int32_t v[4] = { 80 / 20, 80, 400, -1 };    /* nprobes, active, passive and home time */

		memcpy(p + 44, v, sizeof(v));
	}
	if (any) {
		memset(p + 64, 0xff, 6);
	} else {
		memcpy(p + 64, what->bssid, 6);
	}
	if (cs) {
		uint32_t one = 1;

		memcpy(p + 72, &one, 4);
		p[76] = (uint8_t)cs;
		p[77] = (uint8_t)(cs >> 8);
	}

	phase = REDWOOD_STA_JOINING;
	link_up = 0;
	link_lost = 0;
	set_ssid_status = -1;
	assoc_rsn_len = 0;
	eapol_pending = 0;
	err = iovar(1, "join", p, 78, NULL, 0);
	printf("bcm4389: join \"%.*s\": iovar %d, %s, channel %u\n", what->ssid_length, (const char *)what->ssid, err,
	    what->security == REDWOOD_STA_WPA2_PSK ? "WPA2" : "open", cs ? chanspec_control(cs) : 0);
	for (uint64_t t0 = now_us(); err == 0 && !link_up && now_us() - t0 < 15000000;) {
		if (set_ssid_status > 0) {
			break;
		}
		IOSleep(10);
	}
	if (err || !link_up) {
		printf("bcm4389: join: no link (SET_SSID status %d)\n", set_ssid_status);
		(void)sta_leave();
		return err ? EIO : set_ssid_status == 3 ? ENETUNREACH : ETIMEDOUT;
	}
	{
		uint8_t b[6] = { 0 };
		uint32_t got = 0;

		(void)wl_ioctl(23 /* WLC_GET_BSSID */, b, 6, b, 6, &got);
		memcpy(ap_mac, b, 6);
		memcpy(joined_bss.bssid, b, 6);
	}
	if (!flowring_up(ap_mac)) {
		printf("bcm4389: join: no flow ring (status %d)\n", flow_status);
		(void)sta_leave();
		return EIO;
	}

	if (what->security == REDWOOD_STA_WPA2_PSK) {
		int keyed = 0;

		memcpy(wpa.own, own_mac, 6);
		memcpy(wpa.peer, ap_mac, 6);
		read_random(wpa.own_nonce, sizeof(wpa.own_nonce));
		wpa.rsn = assoc_rsn;
		wpa.rsn_length = assoc_rsn_len;
		for (uint64_t t0 = now_us(); !keyed && link_up && now_us() - t0 < 8000000;) {
			if (eapol_pending && assoc_rsn_len) {
				keyed = eapol_handle() == LX_WPA_KEYS;
			} else {
				IOSleep(2);
			}
		}
		if (!keyed) {
			printf("bcm4389: join: the four-way handshake did not finish\n");
			(void)sta_leave();
			return EAUTH;
		}
	}
	joined = 1;
	phase = REDWOOD_STA_JOINED;
	last_error = 0;
	redwood_net_link_state(true);
	printf("bcm4389: JOINED %02x:%02x:%02x:%02x:%02x:%02x\n", ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4],
	    ap_mac[5]);
	return 0;
}

static void
sta_state(struct redwood_sta_state *state)
{
	state->phase = phase;
	state->bss = joined_bss;
	state->last_error = last_error;
}

/* what the radio may tune to: US channels, 2.4 GHz 1-11 and 5 GHz (52-144: radar, listen only) */
static unsigned
sta_channels(struct redwood_sta_channel *channels, unsigned most)
{
	static const uint8_t five[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
		                        136, 140, 144, 149, 153, 157, 161, 165 };
	unsigned n = 0;

	for (unsigned ch = 1; ch <= 11 && n < most; ch++, n++) {
		channels[n].frequency = (uint16_t)(2407 + 5 * ch);
		channels[n].flags = 0;
	}
	for (unsigned i = 0; i < sizeof(five) && n < most; i++, n++) {
		channels[n].frequency = (uint16_t)(5000 + 5 * five[i]);
		channels[n].flags = five[i] >= 52 && five[i] <= 144 ?
		    REDWOOD_STA_CHANNEL_PASSIVE | REDWOOD_STA_CHANNEL_RADAR : 0;
	}
	return n;
}

/* what the station is asked, for the status line: the call under way (NULL: none), since when, how many of each */
static const char *volatile op_now;
static volatile uint64_t op_since;
static volatile int station_attached;
static unsigned op_powers, op_scans, op_joins, op_leaves;

static void
op_begin(const char *name, unsigned *count)
{
	(*count)++;
	op_since = now_us();
	op_now = name;
}

static int
op_end(int error)
{
	printf("bcm4389: %s: %d after %llu ms\n", op_now, error, (now_us() - op_since) / 1000);
	op_now = NULL;
	return error;
}

static int
op_power(bool on)
{
	printf("bcm4389: asked: power %s\n", on ? "on" : "off");
	op_begin(on ? "power on" : "power off", &op_powers);
	return op_end(sta_power(on));
}

static int
op_scan(const struct redwood_sta_scan *what)
{
	printf("bcm4389: asked: scan, %u channels, a name of %u bytes, %s, %u ms each\n", what->frequency_count,
	    what->ssid_length, what->passive ? "passive" : "active", what->dwell_ms);
	op_begin("scan", &op_scans);
	return op_end(sta_scan(what));
}

static int
op_join(const struct redwood_sta_join *what)
{
	bool any = true;

	for (int k = 0; k < 6; k++) {
		any = any && what->bssid[k] == 0;
	}
	printf("bcm4389: asked: join \"%.*s\", %s, %s access point, a passphrase of %u bytes\n", what->ssid_length,
	    (const char *)what->ssid, what->security == REDWOOD_STA_WPA2_PSK ? "WPA2" : "open", any ? "any" : "one",
	    what->passphrase_length);
	op_begin("join", &op_joins);
	return op_end(sta_join(what));
}

static int
op_leave(void)
{
	printf("bcm4389: asked: leave\n");
	op_begin("leave", &op_leaves);
	return op_end(sta_leave());
}

static struct redwood_sta_ops bcm_ops = {
	.power = op_power,
	.scan = op_scan,
	.heard = sta_heard,
	.join = op_join,
	.leave = op_leave,
	.state = sta_state,
	.channels = sta_channels,
};

/* every 30 s: where the station is, and what iOS has asked of it (the log's middle can be lost) */
static void
status_line(uint64_t seconds)
{
	const char *now = op_now;

	printf("bcm4389: %llu s: phase %d, %s%s for %llu ms; asked power %u, scan %u, join %u, leave %u; heard %u; "
	    "frames in %u (%u not staged), out %u (%u TX statuses, %u dropped; flow ring %u written %u, read %u); "
	    "deep sleep %u/%u, %u events, %u ioctls unanswered%s\n", seconds, (int)phase, now ? "in " : "idle",
	    now ? now : "", now ? (now_us() - op_since) / 1000 : 0, op_powers, op_scans, op_joins, op_leaves, heard_count,
	    rx_frames, rx_unstaged, tx_posted, tx_statuses, tx_dropped, flow_id, fr_wr, get16(at(DMA_H2D_RD + flow_id * 4)),
	    ds_acks, ds_exits, events_seen, ioctl_timeouts, trapped ? "; THE FIRMWARE HAS TRAPPED" : "");
	/* the flow ring's indices as each side keeps them: our DMA arrays, and the firmware's own in its RAM */
	if (rings_addr && flow_up) {
		uint32_t h2d_w = tcm_rd(rings_addr + 4), h2d_r = tcm_rd(rings_addr + 8);

		printf("bcm4389: flow ring %u: DMA write %u read %u; in the chip's RAM write %u read %u (arrays at %#x, %#x); "
		    "DMA read of rings 0-1 %u %u\n", flow_id, get16(at(DMA_H2D_WR + flow_id * 4)),
		    get16(at(DMA_H2D_RD + flow_id * 4)), h2d_w ? tcm_rd(h2d_w + flow_id * 4) & 0xffffu : 0xffffu,
		    h2d_r ? tcm_rd(h2d_r + flow_id * 4) & 0xffffu : 0xffffu, h2d_w, h2d_r, get16(at(DMA_H2D_RD)),
		    get16(at(DMA_H2D_RD + 4)));
	}
	if (seconds % 60 == 0) {
		redwood_apple80211_counts();
	}
}

/* ---- the service thread: everything the chip sends ---- */

static void
service_thread(void *arg, wait_result_t wr)
{
	(void)arg;
	(void)wr;
	/* the frames for en0, taken out of `staged` under the lock: another thread's drain() may stage more meanwhile */
	static uint8_t handing[STAGED_MOST][1514];
	static uint16_t handing_len[STAGED_MOST];

	for (uint64_t t = 0;; t++) {
		unsigned n;

		lck_mtx_lock(&bcm_lock);
		drain();
		n = staged_count;
		for (unsigned i = 0; i < n; i++) {
			memcpy(handing[i], staged[i], staged_len[i]);
			handing_len[i] = staged_len[i];
		}
		staged_count = 0;
		lck_mtx_unlock(&bcm_lock);
		for (unsigned i = 0; i < n; i++) {
			redwood_net_input(handing[i], handing_len[i]);
		}
		if (t % 500 == 0 && !trapped && shared_addr && (tcm_rd(shared_addr) & 0x400u)) {
			trapped = 1;
			printf("bcm4389: THE FIRMWARE HAS TRAPPED (shared flags: PCIE_SHARED_TRAP): the radio is gone until a reboot\n");
			show_console();
			if (joined) {
				link_lost = 1;
			}
		}
		if (joined && link_lost) {
			printf("bcm4389: the network is lost\n");
			joined = 0;
			link_lost = 0;
			phase = REDWOOD_STA_IDLE;
			last_error = ECONNRESET;
			redwood_net_link_state(false);
			redwood_sta_lost(ECONNRESET);
		}
		if (joined && eapol_pending) {
			(void)eapol_handle();           /* a group rekey */
		}
		if (t % 500 == 0) {
			show_console();
		}
		if (t % 15000 == 0 && t && station_attached) {
			status_line(t / 500);
		}
		IOSleep(2);
	}
}

/* the events a scan and a join bring, on in the firmware's mask */
static void
events_on(void)
{
	static const uint8_t want[] = { E_SET_SSID, 3, E_DEAUTH, E_DEAUTH_IND, 7, 9, 11, E_DISASSOC_IND, E_LINK, 46,
		                        E_ESCAN_RESULT, E_ASSOC_REQ_IE, 88 };
	uint8_t mask[16];

	bzero(mask, sizeof(mask));
	(void)iovar(0, "event_msgs", NULL, 16, mask, 16);
	for (unsigned i = 0; i < sizeof(want); i++) {
		mask[want[i] / 8] |= (uint8_t)(1u << (want[i] % 8));
	}
	(void)iovar(1, "event_msgs", mask, 16, NULL, 0);
}

static int
step2(void)
{
	static uint8_t nvram[32768];
	const uint8_t *fw, *cal, *clm;
	uint64_t fw_size, cal_size, clm_size, t0;
	int disk, err;
	uint32_t dma_phys = 0, shared = 0, size, sizeword, rings, rtlv, prng, mac_buf[2] = { 0, 0 };
	uint8_t *mac = (uint8_t *)mac_buf;

	if (!PE_parse_boot_argn("bcmdma", &dma_phys, sizeof(dma_phys)) || dma_phys == 0) {
		printf("bcm4389: 2: no bcmdma=: xnuboot set no DMA memory aside\n");
		return 0;
	}
	dma_pa = dma_phys;
	dma = (volatile uint8_t *)map(dma_pa, DMA_SIZE);
	fw = redwood_image_file("fw", "fw_bcmdhd.bin", &fw_size, &disk);
	cal = redwood_image_file("fw", "bcmdhd.cal", &cal_size, &disk);
	clm = redwood_image_file("fw", "bcmdhd_clm.blob", &clm_size, &disk);
	if (dma == NULL || fw == NULL || cal == NULL || clm == NULL) {
		printf("bcm4389: 2: no DMA memory, or no fw/fw_bcmdhd.bin, bcmdhd.cal, bcmdhd_clm.blob in the small image\n");
		return 0;
	}
	printf("bcm4389: 2: firmware %llu bytes, NVRAM %llu, CLM %llu; DMA memory at %#llx\n", fw_size, cal_size,
	    clm_size, dma_pa);

	/* the chip's PCIe core and power domain as brcmfmac's probe leaves them, then the download state */
	(void)bp_rd(CHIPC_BASE);
	bar0[0x21e8 / 4] |= 3u << 8;
	bp_wr(0x180001e8u, bp_rd(0x180001e8u) | (3u << 8));
	bar0[0x2a0c / 4] |= 2u << 8;
	for (int k = 0; k < 20000 && !(bar0[0x21e8 / 4] & (2u << 16)); k++) {
		delay(1);
	}
	cfg_wr(0x88, cfg_rd(0x88) | 0x300u);
	cfg_wr(0x4c, cfg_rd(0x4c) | 0x8000u);
	bp_wr(PCIE2_BASE + 0x120, 0x4e0u);
	bp_wr(PCIE2_BASE + 0x124, bp_rd(PCIE2_BASE + 0x124));
	core_reset(ARMCA7_WRAP, SICF_CPUHALT, SICF_CPUHALT, SICF_CPUHALT);
	core_reset(SYSMEM_WRAP, 0, 0, 0);
	tcm_wr(SHARED_SLOT, 0);

	/* the firmware at the start of RAM, the NVRAM at its top, under it a random seed for the chip */
	for (uint64_t off = 0; off + 4 <= fw_size; off += 4) {
		tcm_wr(RAM_BASE + (uint32_t)off, (uint32_t)fw[off] | (uint32_t)fw[off + 1] << 8 |
		    (uint32_t)fw[off + 2] << 16 | (uint32_t)fw[off + 3] << 24);
	}
	size = nvram_format(cal, cal_size, nvram, sizeof(nvram));
	for (uint32_t off = 0; off < size; off += 4) {
		tcm_wr(SHARED_SLOT - size + off, (uint32_t)nvram[off] | (uint32_t)nvram[off + 1] << 8 |
		    (uint32_t)nvram[off + 2] << 16 | (uint32_t)nvram[off + 3] << 24);
	}
	sizeword = (~(size / 4) << 16) | ((size / 4) & 0xffff);
	tcm_wr(SHARED_SLOT, sizeword);
	rtlv = SHARED_SLOT - size - 8 - RNG_BYTES;
	read_random(&prng, sizeof(prng));
	for (uint32_t off = 0; off < RNG_BYTES; off += 4) {
		prng ^= prng << 13;
		prng ^= prng >> 17;
		prng ^= prng << 5;
		tcm_wr(rtlv + off, prng | 0x01010101u);
	}
	tcm_wr(rtlv + RNG_BYTES, RNG_BYTES);
	tcm_wr(rtlv + RNG_BYTES + 4, RNG_SIG);

	/* the ARM out of reset: the first word of the image is its reset vector */
	tcm_wr(0, (uint32_t)fw[0] | (uint32_t)fw[1] << 8 | (uint32_t)fw[2] << 16 | (uint32_t)fw[3] << 24);
	t0 = now_us();
	core_reset(ARMCA7_WRAP, 0, 0, 0);
	while (now_us() - t0 < 2000000) {
		shared = tcm_rd(SHARED_SLOT);
		if (shared != sizeword && shared != 0 && shared != 0xffffffffu) {
			break;
		}
		delay(100);
	}
	printf("bcm4389: 2: the chip's ARM up after %llu us, shared area at %#x\n", now_us() - t0, shared);
	if (shared < RAM_BASE || shared >= SHARED_SLOT) {
		return 0;
	}
	delay(50000);
	console_addr = tcm_rd(shared + 0x14);
	shared_addr = shared;
	rings = tcm_rd(shared + 0x30);
	rings_addr = rings;
	printf("bcm4389: 2: shared flags %#x, rings at %#x\n", tcm_rd(shared), rings);

	/* the shared area: our scratch, trap buffer and capabilities (as Android's bcmdhd: in-band deep sleep) */
	tcm_wr(shared + 0x34, 16);
	tcm_wr(shared + 0x38, (uint32_t)pa(DMA_SCRATCH));
	tcm_wr(shared + 0x3c, (uint32_t)(pa(DMA_SCRATCH) >> 32));
	/*
	 * ... and fast delete of a flow ring when the firmware has it (flags2 PCIE_SHARED2_FAST_DELETE_RING,
	 * as bcmdhd does): a ring deleted otherwise waits for every frame on it to be completed, which
	 * most were not, and the firmware trapped ("Flowring 3 is taking too long to delete", w3c5)
	 */
	{
		uint32_t flags2 = tcm_rd(shared + 0x50), cap = 7 | (1u << 8) | (1u << 10) | (1u << 12) | (1u << 13) | (1u << 19);

		if (flags2 & 0x20u) {
			cap |= 0x00200000u;             /* HOSTCAP_FAST_DELETE_RING */
		}
		printf("bcm4389: 2: shared flags2 %#x: host capabilities %#x\n", flags2, cap);
		tcm_wr(shared + 0x54, cap);
	}
	tcm_wr(shared + 0x58, (uint32_t)pa(DMA_TRAP));
	tcm_wr(shared + 0x5c, (uint32_t)(pa(DMA_TRAP) >> 32));
	/* ring_info: where the index arrays are in our memory (4-byte indices) */
	tcm_wr(rings + 0x14, (uint32_t)pa(DMA_H2D_WR));
	tcm_wr(rings + 0x18, (uint32_t)(pa(DMA_H2D_WR) >> 32));
	tcm_wr(rings + 0x1c, (uint32_t)pa(DMA_H2D_RD));
	tcm_wr(rings + 0x20, (uint32_t)(pa(DMA_H2D_RD) >> 32));
	tcm_wr(rings + 0x24, (uint32_t)pa(DMA_D2H_WR));
	tcm_wr(rings + 0x28, (uint32_t)(pa(DMA_D2H_WR) >> 32));
	tcm_wr(rings + 0x2c, (uint32_t)pa(DMA_D2H_RD));
	tcm_wr(rings + 0x30, (uint32_t)(pa(DMA_D2H_RD) >> 32));
	/* the five common rings: id, type, items, item size, address */
	{
		static const struct { uint16_t type, items, size; uint32_t off; } ring[5] = {
			{ 1, 256, 40, DMA_RING0 }, { 3, 64, 32, DMA_RING1 }, { 1, 256, 24, DMA_RING2 },
			{ 2, 64, 24, DMA_RING3 }, { 3, 64, 40, DMA_RING4 },
		};
		uint32_t mem = tcm_rd(rings + 0x00);

		printf("bcm4389: 2: ring descriptors at %#x; the firmware's flow rings %u, submission rings %u\n", mem,
		    tcm_rd(rings + 0x34) & 0xffffu, tcm_rd(rings + 0x34) >> 16);

		for (uint32_t r = 0; r < 5; r++) {
			tcm_wr(mem + r * 16 + 0, r | (uint32_t)ring[r].type << 16);
			tcm_wr(mem + r * 16 + 4, ring[r].items | (uint32_t)ring[r].size << 16);
			tcm_wr(mem + r * 16 + 8, (uint32_t)pa(ring[r].off));
			tcm_wr(mem + r * 16 + 12, (uint32_t)(pa(ring[r].off) >> 32));
		}
	}
	bp_wr(PCIE2_BASE + 0xc34, 0x300);
	__asm__ volatile ("dsb sy" ::: "memory");
	bp_wr(PCIE2_BASE + 0x144, HOSTREADY_MAGIC);
	delay(10000);

	/* buffers for ioctl answers, events and frames */
	lck_mtx_lock(&bcm_lock);
	for (uint32_t b = 0; b < 8; b++) {
		post_buf(MSG_IOCTLRESP_BUF_POST, 1 + b, 8192, pa(DMA_IOCTL_RESP + b * 8192));
	}
	for (uint32_t b = 0; b < 8; b++) {
		post_buf(MSG_EVENT_BUF_POST, 101 + b, 8192, pa(DMA_EVENT + b * 8192));
	}
	r0_commit();
	for (uint32_t n = 0; n < 32; n++) {
		rx_post(n);
	}
	r1_commit();
	lck_mtx_unlock(&bcm_lock);

	err = iovar(0, "cur_etheraddr", NULL, 6, mac, 6);
	printf("bcm4389: 2: cur_etheraddr %02x:%02x:%02x:%02x:%02x:%02x (status %d)\n", mac[0], mac[1], mac[2],
	    mac[3], mac[4], mac[5], err);
	if (err) {
		show_console();
		return 0;
	}
	memcpy(own_mac, mac, 6);
	err = clm_load(clm, clm_size);
	printf("bcm4389: 2: CLM loaded: status %d\n", err);
	{
		uint8_t country[12] = { 'U', 'S', 0, 0, 0xff, 0xff, 0xff, 0xff, 'U', 'S', 0, 0 };

		printf("bcm4389: 2: country US: status %d\n", iovar(1, "country", country, sizeof(country), NULL, 0));
	}
	err = wl_ioctl(WLC_UP, NULL, 0, NULL, 0, NULL);
	printf("bcm4389: 2: WLC_UP: status %d; %u deep-sleep requests answered so far\n", err, ds_acks);
	events_on();
	show_console();
	return err == 0;
}

void
redwood_bcm4389_init(void)
{
	uint32_t step = 0, link = 0, id, chip;
	thread_t thread;

	if (!PE_parse_boot_argn("bcmwifi", &step, sizeof(step)) || step == 0) {
		return;
	}
	if (!PE_parse_boot_argn("bcmlink", &link, sizeof(link)) || link != 1) {
		printf("bcm4389: no bcmlink=1: xnuboot did not bring the link up (build it with WIFI_PCIE)\n");
		return;
	}

	/* 1: the windows, the ids */
	cfg = map(PCIE1_CFG_PHYS, 0x1000);
	bar0 = map(PCIE1_BAR0_PHYS, PCIE1_BAR0_SIZE);
	if (cfg == NULL || bar0 == NULL) {
		printf("bcm4389: 1: cannot map the windows\n");
		return;
	}
	id = cfg_rd(0x00);
	printf("bcm4389: 1: PCI id %04x:%04x, command/status %#x, BAR0 %#x, BAR2 %#x\n", id & 0xffff, id >> 16,
	    cfg_rd(0x04), cfg_rd(0x10), cfg_rd(0x18));
	if (id != 0x444114e4u) {
		printf("bcm4389: 1: not the BCM4389 (14e4:4441)\n");
		return;
	}
	chip = bp_rd(CHIPC_BASE);
	printf("bcm4389: 1: chip id %#x: BCM%x rev %u\n", chip, chip & 0xffff, (chip >> 16) & 0xf);
	if (step < 2) {
		return;
	}
	if (!step2()) {
		printf("bcm4389: 2: the firmware is not up\n");
		return;
	}
	if (kernel_thread_start(service_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		service_running = 1;
	}
	if (step < 3) {
		return;
	}

	/* 3: the station and en0 */
	phase = REDWOOD_STA_IDLE;
	memcpy(bcm_link.address, own_mac, 6);
	memcpy(bcm_ops.address, own_mac, 6);
	if (!redwood_net_attach(&bcm_link)) {
		printf("bcm4389: 3: en0 is taken already\n");
		return;
	}
	redwood_sta_attach(&bcm_ops);
	station_attached = 1;
	printf("bcm4389: 3: the station is attached: wifid and Settings can scan and join\n");

	/*
	 * bcmjoin=1: the network of fw/network (name, then passphrase: userland/build.sh puts it there
	 * from fw-wifi/network, in no repository) is a standing request, as redwood's own driver makes
	 * it (redwood_wifi.c join_given): for runs nobody types a password in. The log has its lengths only.
	 */
	{
		static struct redwood_sta_join what;
		unsigned name_length = sizeof(what.ssid), passphrase_length = sizeof(what.passphrase);
		uint32_t join = 0;

		if (PE_parse_boot_argn("bcmjoin", &join, sizeof(join)) && join == 1) {
			bzero(&what, sizeof(what));
			if (redwood_wifi_network(what.ssid, &name_length, what.passphrase, &passphrase_length) && name_length > 0 &&
			    passphrase_length >= 8) {
				what.ssid_length = (uint8_t)name_length;
				what.passphrase_length = (uint8_t)passphrase_length;
				what.security = REDWOOD_STA_WPA2_PSK;
				printf("bcm4389: 3: bcmjoin=1: the network of fw/network is asked for (a name of %u bytes, a passphrase "
				    "of %u): %d\n", name_length, passphrase_length, redwood_sta_ask_join_standing(&what));
			} else {
				printf("bcm4389: 3: bcmjoin=1: fw/network has no name and passphrase\n");
			}
			bzero(&what, sizeof(what));
		}
	}
}
