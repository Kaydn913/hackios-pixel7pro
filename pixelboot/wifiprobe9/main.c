/*
 * wifiprobe9: the BCM4389's first scan (WIFI-PLAN.md, W2).
 *
 * wifiprobe8 got the firmware answering ioctls (cur_etheraddr, the CLM, country US, WLC_UP), then:
 *  - the dongle trapped after an AXI decode error from a 16-bit write of ours to 0x607fc0, past the
 *    end of its RAM: a ring-index pointer from ring_info. With host-memory (DMA) ring indices, as the
 *    shared flags say, brcmfmac never touches the TCM ones; neither does this probe now, and no 16-bit
 *    TCM access outside RAM is made at all.
 *  - WLC_SCAN said BCME_BUFTOOSHORT: this firmware scans with the "escan" iovar (params version 2)
 *    and reports what it finds as ESCAN_RESULT events, as Linux's brcmfmac does it (cfg80211.c
 *    brcmf_run_escan / brcmf_escan_prep, fwil_types.h).
 * So: mpc 0, the ESCAN_RESULT event on, an active escan of every channel, then every BSS printed
 * once (BSSID, channel, RSSI, SSID) until the scan's final event. The firmware's own console is
 * printed after each step.
 *
 * All BAR2 (TCM) accesses are naturally aligned; no memcpy, memset or struct stores on BAR memory.
 */
#include "board.h"

typedef int s32;
typedef short s16;
typedef char s8;
#ifndef NULL
#define NULL ((void *)0)
#endif

__asm__(
	".section .rodata\n"
	".balign 4\n"
	".global fw_bin_start, fw_bin_end\n"
	"fw_bin_start:\n"
	".incbin \"downloads/wifi/fw/fw_bcmdhd.bin\"\n"
	"fw_bin_end:\n"
	".balign 4\n"
	".global nvram_cal_start, nvram_cal_end\n"
	"nvram_cal_start:\n"
	".incbin \"downloads/wifi/fw/bcmdhd.cal\"\n"
	"nvram_cal_end:\n"
	".balign 4\n"
	".global clm_blob_start, clm_blob_end\n"
	"clm_blob_start:\n"
	".incbin \"downloads/wifi/fw/bcmdhd_clm.blob\"\n"
	"clm_blob_end:\n"
	".text\n"
);

extern const u8 fw_bin_start[], fw_bin_end[];
extern const u8 nvram_cal_start[], nvram_cal_end[];
extern const u8 clm_blob_start[], clm_blob_end[];

#define PMU_ISOL        (0x18060000ul + 0x3ec4)
#define CMU_TOP         0x1e080000ul
#define CMU_HSI2        0x14400000ul
#define HSI2_GPIO       0x14440000ul
#define PCIE1_ELBI      0x14520000ul
#define PCIE1_PCS       0x14540000ul
#define PCIE1_PHY       0x14550000ul
#define PCIE1_DBI       0x14800000ul
#define PCIE1_ATU       (PCIE1_DBI + 0x300000ul)
#define PCIE1_MEM       0x60000000ul
#define PCIE1_BAR0_ADDR 0x60000000ul
#define PCIE1_BAR2_ADDR 0x60400000ul
#define PCIE1_CFG       0x60ffe000ul

#define GPIO_CON        0x0
#define GPIO_DAT        0x4
#define GPIO_PUD        0x8
#define PIN_PERST       0
#define PIN_CLKREQ      1
#define PIN_WL_REG_ON   4

/* ELBI */
#define ELBI_APP_LTSSM_ENABLE   0x054
#define ELBI_DEVICE_TYPE        0x080
#define ELBI_RDLH_LINKUP        0x2c8
#define ELBI_LINKDOWN_RST_CTRL  0x3a0
#define ELBI_SOFT_RESET         0x3a4
#define  SOFT_PWR_RESET         (1u << 1)
#define  SOFT_NON_STICKY_RESET  (1u << 3)
#define ELBI_QCH_SEL            0x3a8
#define ELBI_APP_REQ_EXIT_L1    0x3bc
#define ELBI_MSTR_PEND_SEL_NAK  0x474
#define ELBI_DBI_L1_EXIT_DIS    0x1078
#define ELBI_PHY_PORT_RST       0x1400
#define ELBI_PHY_CMN_RST        0x1404
#define ELBI_PHY_INIT_RST       0x1408

#define S_L0                    0x11

/* BCM4389 core & memory addresses */
#define CHIPC_BASE              0x18000000u
#define PCIE2_BASE              0x18001000u
#define ARMCA7_BASE             0x18020000u
#define ARMCA7_WRAP             0x18120000u
#define SYSMEM_BASE             0x18026000u
#define SYSMEM_WRAP             0x18126000u

#define SICF_CPUHALT            0x0020u

#define DONGLE_RAM_BASE         0x00200000u
#define DONGLE_RAM_SIZE         0x002a0000u             /* 17 banks = 2,752,512 bytes */
#define DONGLE_RAM_TOP          (DONGLE_RAM_BASE + DONGLE_RAM_SIZE)
#define SHARED_ADDR_SLOT        (DONGLE_RAM_TOP - 4u)   /* 0x0049fffc */

#define DNGL_RTLV_RNG_SIG       0xfeedc0deu
#define RNG_HOST_NBYTES         128u

/* MsgBuf & PCIe shared definitions */
#define PCIE_SHARED_VERSION_7           7u
#define HOST_API_VERSION_7              7u
#define HOSTCAP_H2D_VALID_PHASE         (1u << 8)
#define HOSTCAP_H2D_ENABLE_HOSTRDY      (1u << 10)
#define HOSTCAP_DS_NO_OOB_DW            (1u << 12)
#define HOSTCAP_EXTENDED_TRAP_DATA      (1u << 13)
#define HOSTCAP_UR_FW_NO_TRAP           (1u << 19)

#define H2D_HOSTREADY_MAGIC             0x12345678u
#define DHD_WRPTR_UPDATE_MAGIC          0xdeadbeefu

#define MSG_TYPE_IOCTLPTR_REQ           0x09u
#define MSG_TYPE_IOCTLPTR_REQ_ACK       0x0au
#define MSG_TYPE_IOCTLRESP_BUF_POST     0x0bu
#define MSG_TYPE_IOCTL_CMPLT            0x0cu
#define MSG_TYPE_EVENT_BUF_POST         0x0du
#define MSG_TYPE_WL_EVENT               0x0eu

#define WLC_UP                          2u
#define WLC_DOWN                        3u
#define WLC_SCAN                        50u
#define WLC_SCAN_RESULTS                51u
#define WLC_GET_VAR                     262u
#define WLC_SET_VAR                     263u

#define DLOAD_HANDLER_VER               1u
#define DLOAD_FLAG_VER_SHIFT            12u
#define DL_BEGIN                        0x0002u
#define DL_END                          0x0004u
#define DL_TYPE_CLM                     2u

#define DHD_IOCTL_REQ_PKTID             0xFFFEu

#define H2D_EPOCH_MODULO                253u
#define H2D_EPOCH_INIT_VAL              (H2D_EPOCH_MODULO + 1u)
#define BCMPCIE_CMNHDR_PHASE_BIT_INIT   0x80u

#define GATE_ON(v)      (((v) | (1u << 20) | (1u << 21)) & ~(1u << 28))

/* Host DMA buffers (aligned to 64 bytes) */
static u8 dma_scratch_buf[64] __attribute__((aligned(64)));
static u8 dma_trap_buf[1024] __attribute__((aligned(64)));

static u16 dma_h2d_wr[512] __attribute__((aligned(64)));
static u16 dma_h2d_rd[512] __attribute__((aligned(64)));
static u16 dma_d2h_wr[64]  __attribute__((aligned(64)));
static u16 dma_d2h_rd[64]  __attribute__((aligned(64)));

/* 5 Common MsgBuf Rings */
static u8 ring0_ctrl_sub[256 * 40] __attribute__((aligned(64)));
static u8 ring1_rxp_sub[64 * 32]   __attribute__((aligned(64)));
static u8 ring2_ctrl_cpl[256 * 24] __attribute__((aligned(64)));
static u8 ring3_tx_cpl[64 * 24]    __attribute__((aligned(64)));
static u8 ring4_rx_cpl[64 * 40]    __attribute__((aligned(64)));

/* IOCTL Request & Return buffers */
static u8 ioctl_req_payload[4096] __attribute__((aligned(64)));
static u8 ioctl_resp_buf[8][8192] __attribute__((aligned(64)));
static u8 event_resp_buf[8][2048] __attribute__((aligned(64)));

static void
udelay(u64 us)
{
	u64 freq, start, now;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(start));
	do {
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(now));
	} while (now - start < us * freq / 1000000);
}

static void
mdelay(u64 ms)
{
	while (ms--) {
		udelay(1000);
		if ((ms & 31) == 0)
			wdt_pet();
	}
}

static u64
now_us(void)
{
	u64 freq, t;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t / (freq / 1000000);
}

static int
poll(const char *what, u64 addr, u32 mask, u32 want, u64 max_us)
{
	u32 v = 0;
	u64 i;

	for (i = 0; i < max_us; i++) {
		v = *reg(addr);
		if ((v & mask) == want)
			break;
		udelay(1);
		if ((i & 0x3fff) == 0)
			wdt_pet();
	}
	puts_("wifiprobe9: ");
	puts_(what);
	puts_(i < max_us ? ": ok after " : ": TIMEOUT after ");
	hex(i, 6);
	puts_(" us, value 0x");
	hex(v, 8);
	put('\n');
	return i < max_us;
}

static void
pin_field(u64 off, unsigned pin, u32 val)
{
	u32 v = *reg(HSI2_GPIO + off);

	v &= ~(0xfu << (4 * pin));
	v |= (val & 0xf) << (4 * pin);
	*reg(HSI2_GPIO + off) = v;
}

static void
pin_set(unsigned pin, int high)
{
	u32 v = *reg(HSI2_GPIO + GPIO_DAT);

	if (high)
		v |= 1u << pin;
	else
		v &= ~(1u << pin);
	*reg(HSI2_GPIO + GPIO_DAT) = v;
}

struct regval {
	u32 off, val;
};

static const struct regval phy_common[] = {
	{ 0x018, 0x50 }, { 0x048, 0x33 }, { 0x068, 0x01 }, { 0x070, 0x12 }, { 0x08c, 0x00 },
	{ 0x090, 0x21 }, { 0x0b0, 0x14 }, { 0x0b8, 0x50 }, { 0x0e0, 0x51 }, { 0x100, 0x00 },
	{ 0x104, 0x80 }, { 0x140, 0x38 }, { 0x180, 0xa4 }, { 0x188, 0x03 }, { 0x2a8, 0x38 },
	{ 0x2e4, 0x12 },
	{ 0x400, 0x80 }, { 0x408, 0x20 }, { 0x550, 0x00 }, { 0x5a8, 0x00 }, { 0x5ec, 0xff },
	{ 0x458, 0x02 }, { 0x5b0, 0x34 }, { 0x450, 0x20 },
	{ 0x0ac, 0x34 }, { 0x0b4, 0x34 }, { 0x0e0, 0x50 }, { 0x0f4, 0x00 }, { 0x0f8, 0x08 },
	{ 0x104, 0xa0 }, { 0x11c, 0x19 }, { 0x124, 0x17 }, { 0x220, 0x41 },
};

static const struct regval phy_lane[] = {
	{ 0x82c, 0x08 }, { 0x830, 0x24 }, { 0x878, 0x80 }, { 0x894, 0x40 }, { 0x8c0, 0x00 },
	{ 0x8f4, 0x30 }, { 0x908, 0x05 }, { 0x90c, 0xe0 }, { 0x914, 0xd4 }, { 0x91c, 0xd3 },
	{ 0x920, 0xce }, { 0x924, 0x01 }, { 0x928, 0x35 }, { 0x92c, 0xba }, { 0x930, 0x41 },
	{ 0x934, 0x15 }, { 0x938, 0x13 }, { 0x93c, 0x4e }, { 0x948, 0x43 }, { 0x94c, 0xfc },
	{ 0x954, 0x10 }, { 0x958, 0x69 }, { 0x964, 0x40 }, { 0x9b4, 0xf6 }, { 0x9c0, 0x2d },
	{ 0x9c4, 0xb7 }, { 0x9cc, 0x3c }, { 0x9dc, 0x7e }, { 0xa40, 0x02 }, { 0xa70, 0x26 },
	{ 0xa74, 0x00 }, { 0xb40, 0x06 }, { 0xb44, 0x06 }, { 0xb48, 0x04 }, { 0xb4c, 0x03 },
	{ 0xb50, 0x03 }, { 0xb54, 0x03 }, { 0xb58, 0x03 }, { 0xb5c, 0x03 }, { 0xc10, 0x1b },
	{ 0xc44, 0x10 }, { 0xc48, 0x10 }, { 0xc4c, 0x10 }, { 0xc50, 0x10 }, { 0xc54, 0x02 },
	{ 0xc58, 0x02 }, { 0xc5c, 0x02 }, { 0xc60, 0x02 }, { 0xc6c, 0x02 }, { 0xc70, 0x02 },
	{ 0xca8, 0xe7 }, { 0xcac, 0x00 }, { 0xcb0, 0x0e }, { 0xccc, 0x1c }, { 0xcd4, 0x05 },
	{ 0xcd8, 0x77 }, { 0xcdc, 0x7a }, { 0xdb4, 0x2f },
	{ 0x8fc, 0x80 }, { 0x914, 0xf4 }, { 0x91c, 0xd3 }, { 0x920, 0xca }, { 0x928, 0x3d },
	{ 0x92c, 0xb8 }, { 0x930, 0x41 }, { 0x934, 0x17 }, { 0x93c, 0x4c }, { 0x948, 0x73 },
	{ 0x94c, 0xfc }, { 0x96c, 0x55 }, { 0x988, 0x78 }, { 0x994, 0x3b }, { 0x9b4, 0xf6 },
	{ 0x9c4, 0xff }, { 0x9c8, 0x20 }, { 0xa08, 0x2f }, { 0xb9c, 0x3f },
	{ 0x9cc, 0x00 }, { 0xcd8, 0xff }, { 0xcdc, 0x6e },
	{ 0x82c, 0x0f }, { 0x830, 0x60 }, { 0x834, 0x7e },
	{ 0xc08, 0x00 }, { 0xc10, 0x09 }, { 0xc40, 0x04 }, { 0xc70, 0x00 },
	{ 0x9b4, 0x76 },
};

static const struct regval phy_clk[] = {
	{ 0xbcc, 0x41 }, { 0xbd4, 0x41 }, { 0xbdc, 0x68 }, { 0xbe0, 0x00 }, { 0xbe4, 0xd0 },
};

#define N(a)    (sizeof(a) / sizeof((a)[0]))

static void
write_table(u64 base, const struct regval *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		*reg(base + t[i].off) = t[i].val;
}

static void
phy_all_pwrdn_clear(void)
{
	static const u32 offs[] = {
		0x404, 0x408, 0x40c,
		0x800, 0x804, 0xa40, 0xa44, 0xa48, 0xa4c, 0xa50,
		0x1000, 0x1004, 0x1240, 0x1244, 0x1248, 0x124c, 0x1250,
	};

	*reg(PCIE1_PHY + 0xd8) = 0x28;
	mdelay(1);
	for (unsigned i = 0; i < N(offs); i++)
		*reg(PCIE1_PHY + offs[i]) = 0;
	*reg(PCIE1_PCS + 0x184) = 0;
}

static void
phy_config(void)
{
	u32 v;

	*reg(PCIE1_PHY + 0xd8) = 0x28;
	*reg(PCIE1_PCS + 0x008) |= 1u << 7;

	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 0;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 0;
	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 0;
	udelay(10);
	*reg(PCIE1_ELBI + ELBI_PHY_CMN_RST) = 1;
	udelay(10);

	write_table(PCIE1_PHY, phy_common, N(phy_common));
	write_table(PCIE1_PHY, phy_lane, N(phy_lane));
	write_table(PCIE1_PHY, phy_clk, N(phy_clk));
	write_table(PCIE1_PHY + 0x800, phy_clk, N(phy_clk));
	wdt_pet();

	*reg(PCIE1_PCS + 0x004) = 0;
	*reg(PCIE1_PCS + 0x154) = 0x700d5;
	*reg(PCIE1_PCS + 0x150) = 0x300ff;
	*reg(PCIE1_PCS + 0x170) = 0x40;
	v = *reg(PCIE1_PCS + 0x008);
	v |= (1u << 4) | (1u << 5);
	*reg(PCIE1_PCS + 0x008) = v;
	*reg(PCIE1_PCS + 0x190) = 0x100b0808;

	*reg(PCIE1_ELBI + ELBI_PHY_PORT_RST) = 1;
	*reg(PCIE1_ELBI + ELBI_PHY_INIT_RST) = 1;

	v = *reg(PCIE1_PHY + 0x5d0);
	v |= 1u << 4;
	v &= ~(1u << 3);
	*reg(PCIE1_PHY + 0x5d0) = v;
	*reg(PCIE1_PHY + 0xbf4) = 4;
}

static void
iatu_outbound(unsigned idx, u32 type, u64 cpu_base, u32 size, u64 pci_target)
{
	u64 r = PCIE1_ATU + (u64)idx * 0x200;

	*reg(r + 0x08) = (u32)cpu_base;
	*reg(r + 0x0c) = (u32)(cpu_base >> 32);
	*reg(r + 0x10) = (u32)(cpu_base + size - 1);
	*reg(r + 0x14) = (u32)pci_target;
	*reg(r + 0x18) = (u32)(pci_target >> 32);
	*reg(r + 0x00) = type;
	*reg(r + 0x04) = 1u << 31;
	udelay(10);
}

/* Read/write backplane via BAR0 */
static u32 cur_bar0_win = 0xffffffffu;

static u32
bp_read32(u32 addr)
{
	u32 win = addr & ~0xfffu;

	if (win != cur_bar0_win) {
		*reg(PCIE1_CFG + 0x80) = win;
		(void)*reg(PCIE1_CFG + 0x80);
		cur_bar0_win = win;
	}
	return *reg(PCIE1_BAR0_ADDR + (addr & 0xfffu));
}

static void
bp_write32(u32 addr, u32 val)
{
	u32 win = addr & ~0xfffu;

	if (win != cur_bar0_win) {
		*reg(PCIE1_CFG + 0x80) = win;
		(void)*reg(PCIE1_CFG + 0x80);
		cur_bar0_win = win;
	}
	*reg(PCIE1_BAR0_ADDR + (addr & 0xfffu)) = val;
}

/* Read/write TCM via BAR2 with strictly natural alignment */
static u32 cur_bar2_win = 0xffffffffu;

static void
tcm_set_win(u32 win)
{
	if (win != cur_bar2_win) {
		*reg(PCIE1_CFG + 0x84) = win;
		(void)*reg(PCIE1_CFG + 0x84);
		cur_bar2_win = win;
	}
}

static u32
tcm_read32(u32 addr)
{
	tcm_set_win(addr & ~0x3fffffu);
	return *reg(PCIE1_BAR2_ADDR + (addr & 0x3fffffu));
}

static void
tcm_write32(u32 addr, u32 val)
{
	tcm_set_win(addr & ~0x3fffffu);
	*reg(PCIE1_BAR2_ADDR + (addr & 0x3fffffu)) = val;
}

/*
 * wifiprobe9's dongle crash: a 16-bit write of ours to 0x607fc0, past the end of RAM (0x4a0000), made an
 * AXI decode error, and the firmware trapped at its next error check. That is a ring-index pointer from
 * ring_info that the firmware never uses with host-memory (DMA) indices. Nothing outside RAM, then.
 */
static unsigned tcm_refused;

static int
tcm_in_ram(u32 addr)
{
	if (addr >= DONGLE_RAM_BASE && addr + 2u <= DONGLE_RAM_TOP)
		return 1;
	if (tcm_refused++ < 4)
		kv("wifiprobe9: refused a 16-bit TCM access outside RAM at", addr);
	return 0;
}

static u16
tcm_read16(u32 addr)
{
	if (!tcm_in_ram(addr))
		return 0;
	tcm_set_win(addr & ~0x3fffffu);
	return *(volatile u16 *)(PCIE1_BAR2_ADDR + (addr & 0x3fffffu));
}

static void
tcm_write16(u32 addr, u16 val)
{
	if (!tcm_in_ram(addr))
		return;
	tcm_set_win(addr & ~0x3fffffu);
	*(volatile u16 *)(PCIE1_BAR2_ADDR + (addr & 0x3fffffu)) = val;
}

/* with DMA indices (shared flag BRCMF_PCIE_SHARED_DMA_INDEX) the ring indices live in host memory only */
static int g_dma_idx;

static void
tcm_idx_write(u32 ptr, u16 val)
{
	if (!g_dma_idx)
		tcm_write16(ptr, val);
}

static u16
idx_read(u32 ptr, volatile u16 *dma)
{
	return g_dma_idx ? *dma : tcm_read16(ptr);
}

static u8
tcm_read8(u32 addr)
{
	u32 w = tcm_read32(addr & ~3u);
	return (u8)((w >> (8 * (addr & 3u))) & 0xffu);
}

static void
ai_reset_core(u32 wrap_base, u32 pre, u32 rst, u32 post)
{
	if ((bp_read32(wrap_base + 0x800) & 1u) == 0) {
		bp_write32(wrap_base + 0x408, pre | 3u);
		(void)bp_read32(wrap_base + 0x408);
		bp_write32(wrap_base + 0x800, 1u);
		udelay(20);
	}
	bp_write32(wrap_base + 0x408, rst | 3u);
	(void)bp_read32(wrap_base + 0x408);
	udelay(10);
	bp_write32(wrap_base + 0x800, 0u);
	for (unsigned k = 0; k < 100 && (bp_read32(wrap_base + 0x800) & 1u); k++)
		udelay(1);
	bp_write32(wrap_base + 0x408, post | 1u);
	(void)bp_read32(wrap_base + 0x408);
	udelay(10);
}

static u8 nvram_buf[32768] __attribute__((aligned(4)));

static u32
format_nvram(const u8 *src, u32 src_len)
{
	u32 n, dp = 0, column = 0;
	int find_nl = 0;

	if (src_len > 0)
		src_len--;

	for (n = 0; n < src_len && dp < sizeof(nvram_buf) - 8; n++) {
		u8 c = src[n];
		if (c == '\r')
			continue;
		if (find_nl && c != '\n')
			continue;
		find_nl = 0;
		if (c == '#') {
			find_nl = 1;
			continue;
		}
		if (c == '\n') {
			if (column == 0)
				continue;
			nvram_buf[dp++] = 0;
			column = 0;
			continue;
		}
		nvram_buf[dp++] = c;
		column++;
	}

	while (dp & 3u)
		nvram_buf[dp++] = 0;
	nvram_buf[dp++] = 0;
	while (dp & 3u)
		nvram_buf[dp++] = 0;
	return dp;
}

static u32 g_console_addr;
static u32 con_shown;

/* what the firmware wrote to its console since the last call */
static void
dump_fw_console(u32 console_addr)
{
	u32 log_buf = tcm_read32(console_addr + 0x08);
	u32 log_size = tcm_read32(console_addr + 0x0c);
	u32 log_idx = tcm_read32(console_addr + 0x10);

	if (log_buf < DONGLE_RAM_BASE || log_buf >= DONGLE_RAM_TOP ||
	    log_size == 0 || log_size > 0x40000u || log_idx >= log_size) {
		kv("wifiprobe9: console out of range, buf", log_buf);
		return;
	}
	if (log_idx == con_shown)
		return;
	puts_("wifiprobe9: --- firmware console ---\n");
	for (u32 i = con_shown; i != log_idx; i = (i + 1) % log_size) {
		u8 c = tcm_read8(log_buf + i);

		if (c == '\n' || (c >= 0x20 && c < 0x7f) || c == '\t')
			put((char)c);
		if ((i & 0x3ff) == 0)
			wdt_pet();
	}
	con_shown = log_idx;
	puts_("\nwifiprobe9: --- end of firmware console ---\n");
}

static void
show_console(void)
{
	if (g_console_addr >= DONGLE_RAM_BASE && g_console_addr < SHARED_ADDR_SLOT)
		dump_fw_console(g_console_addr);
}

void
at_entry(u64 el, u64 dtb)
{
	u64 x0;

	(void)dtb;
	log_open();
	puts_("\nwifiprobe9: BCM4389 MsgBuf ring configuration & WLC_GET_VAR ioctl test\n");
	kv("wifiprobe9: at entry, EL", el);
	if (el != 2)
		return;
	x0 = psci_call(0x82000504, PMU_ISOL, 0, 0);
	if ((x0 & ~1ul) == 0)
		psci_call(0x82000504, PMU_ISOL, 1, x0 | 1);
}

/* Post buffer message on Ring 0 (40 bytes) */
static void
ring0_post_buf(u16 slot, u8 msg_type, u8 epoch, u32 request_id, u16 buf_len, u64 host_addr)
{
	u8 *p = ring0_ctrl_sub + (u32)slot * 40u;
	for (unsigned k = 0; k < 40; k++)
		p[k] = 0;

	/* cmn_msg_hdr_t */
	p[0] = msg_type;
	p[1] = 0;                               /* if_id */
	p[2] = BCMPCIE_CMNHDR_PHASE_BIT_INIT;   /* flags = 0x80 */
	p[3] = epoch;
	*(volatile u32 *)(p + 4) = request_id;

	/* host_buf_len */
	*(volatile u16 *)(p + 8) = buf_len;

	/* host_buf_addr (offset +0x10) */
	*(volatile u32 *)(p + 0x10) = (u32)host_addr;
	*(volatile u32 *)(p + 0x14) = (u32)(host_addr >> 32);
}

/* Post IOCTL request message on Ring 0 (40 bytes) */
static void
ring0_post_ioctl_req(u16 slot, u8 epoch, u32 cmd, u16 trans_id, u16 in_len, u16 out_len, u64 in_addr)
{
	u8 *p = ring0_ctrl_sub + (u32)slot * 40u;
	for (unsigned k = 0; k < 40; k++)
		p[k] = 0;

	/* cmn_msg_hdr_t */
	p[0] = MSG_TYPE_IOCTLPTR_REQ;           /* 0x09 */
	p[1] = 0;                               /* if_id */
	p[2] = BCMPCIE_CMNHDR_PHASE_BIT_INIT;   /* flags = 0x80 */
	p[3] = epoch;
	*(volatile u32 *)(p + 4) = DHD_IOCTL_REQ_PKTID; /* 0xFFFE */

	*(volatile u32 *)(p + 8)  = cmd;
	*(volatile u16 *)(p + 12) = trans_id;
	*(volatile u16 *)(p + 14) = in_len;
	*(volatile u16 *)(p + 16) = out_len;

	/* host_input_buf_addr (offset +0x18) */
	*(volatile u32 *)(p + 0x18) = (u32)in_addr;
	*(volatile u32 *)(p + 0x1c) = (u32)(in_addr >> 32);
}

static u32 g_h2d_w_idx_ptr = 0;
static u32 g_d2h_w_idx_ptr = 0;
static u32 g_d2h_r_idx_ptr = 0;
static u16 r0_wr_idx = 0;
static u16 r2_rd_idx = 0;
static u8  r0_epoch = 1;
static u16 cur_trans_id = 0;

static void handle_event(const u8 *buf, u16 len);

static u16
ring0_next_slot(void)
{
	u16 slot = r0_wr_idx++;

	if (r0_wr_idx == 256)
		r0_wr_idx = 0;
	return slot;
}

/* ring 0's new write index to the dongle, then the doorbell */
static void
ring0_commit(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	dma_h2d_wr[0] = r0_wr_idx;
	tcm_idx_write(g_h2d_w_idx_ptr, r0_wr_idx);
	__asm__ volatile("dsb sy" ::: "memory");
	bp_write32(PCIE2_BASE + 0x140, DHD_WRPTR_UPDATE_MAGIC);
}

/*
 * Everything the dongle has completed on ring 2: the response and event buffers it used are posted
 * again, events go to handle_event(). Returns 1 once the ioctl with transaction id tid is complete
 * (tid 0, never used, to only take events).
 */
static int
drain_d2h(u16 tid, s16 *ret_status, void *out_buf, u16 out_max, u16 *actual_out_len)
{
	int completed = 0;
	u16 d2h_wr = idx_read(g_d2h_w_idx_ptr, &dma_d2h_wr[0]);

	while (r2_rd_idx != d2h_wr) {
		__asm__ volatile("dmb sy" ::: "memory");
		u8 *msg = ring2_ctrl_cpl + (u32)r2_rd_idx * 24u;
		u8 msg_type = msg[0];
		u32 req_id  = *(volatile u32 *)(msg + 4);
		u16 status  = *(volatile u16 *)(msg + 8);

		if (msg_type == MSG_TYPE_IOCTL_CMPLT) {
			u16 resp_len = *(volatile u16 *)(msg + 12);
			u16 trans_id = *(volatile u16 *)(msg + 14);

			if (trans_id == tid) {
				completed = 1;
				*ret_status = (s16)status;
				if (out_buf && out_max > 0 && req_id >= 1 && req_id <= 8) {
					u8 *src = ioctl_resp_buf[req_id - 1];
					u16 cpy = resp_len < out_max ? resp_len : out_max;
					for (u16 c = 0; c < cpy; c++)
						((u8 *)out_buf)[c] = src[c];
					if (actual_out_len)
						*actual_out_len = resp_len;
				}
			}
			if (req_id >= 1 && req_id <= 8) {
				ring0_post_buf(ring0_next_slot(), MSG_TYPE_IOCTLRESP_BUF_POST, r0_epoch++,
				               req_id, sizeof(ioctl_resp_buf[0]), (u64)ioctl_resp_buf[req_id - 1]);
				ring0_commit();
			}
		} else if (msg_type == MSG_TYPE_WL_EVENT) {
			if (req_id >= 101 && req_id <= 108) {
				/* msgbuf_rx_event: event_data_len at +12 */
				handle_event(event_resp_buf[req_id - 101], *(volatile u16 *)(msg + 12));
				ring0_post_buf(ring0_next_slot(), MSG_TYPE_EVENT_BUF_POST, r0_epoch++,
				               req_id, 2048, (u64)event_resp_buf[req_id - 101]);
				ring0_commit();
			} else {
				kv("wifiprobe9: WL_EVENT with an unknown req_id", req_id);
			}
		}

		r2_rd_idx++;
		if (r2_rd_idx == 256)
			r2_rd_idx = 0;
		dma_d2h_rd[0] = r2_rd_idx;
		tcm_idx_write(g_d2h_r_idx_ptr, r2_rd_idx);
		d2h_wr = idx_read(g_d2h_w_idx_ptr, &dma_d2h_wr[0]);
	}
	return completed;
}

static int
send_ioctl(u32 cmd, const void *in_data, u16 in_len, void *out_buf, u16 out_max, u16 *actual_out_len)
{
	u16 tid = ++cur_trans_id;
	s16 ret_status = -1;

	if (in_data && in_len > 0) {
		for (unsigned k = 0; k < in_len && k < sizeof(ioctl_req_payload); k++)
			ioctl_req_payload[k] = ((const u8 *)in_data)[k];
	}

	u16 out_len = out_max >= 256 ? out_max : 256;
	if (out_len > sizeof(ioctl_resp_buf[0]))
		out_len = sizeof(ioctl_resp_buf[0]);

	ring0_post_ioctl_req(ring0_next_slot(), r0_epoch++, cmd, tid, in_len, out_len, (u64)ioctl_req_payload);
	ring0_commit();

	for (u64 i = 0; i < 500000; i++) {
		if (drain_d2h(tid, &ret_status, out_buf, out_max, actual_out_len))
			return (int)ret_status;
		udelay(5);
		if ((i & 0x3ff) == 0)
			wdt_pet();
	}
	return -100;
}

static int
dhd_clmload(const u8 *clm_blob, u32 clm_len)
{
	u32 offset = 0;
	u16 chunk_idx = 0;
	kv("wifiprobe9: starting CLM blob download, total bytes", clm_len);

	while (offset < clm_len) {
		u32 chunk_len = clm_len - offset;
		if (chunk_len > 1024)
			chunk_len = 1024;

		u16 dl_flag = 0;
		if (offset == 0)
			dl_flag |= DL_BEGIN;
		if (offset + chunk_len == clm_len)
			dl_flag |= DL_END;

		u8 dload_buf[1024 + 16];
		for (unsigned k = 0; k < sizeof(dload_buf); k++)
			dload_buf[k] = 0;

		*(u16 *)(dload_buf + 0) = (DLOAD_HANDLER_VER << DLOAD_FLAG_VER_SHIFT) | dl_flag;
		*(u16 *)(dload_buf + 2) = DL_TYPE_CLM;
		*(u32 *)(dload_buf + 4) = chunk_len;
		*(u32 *)(dload_buf + 8) = 0;

		for (u32 b = 0; b < chunk_len; b++)
			dload_buf[12 + b] = clm_blob[offset + b];

		u32 dload_len = (12u + chunk_len + 7u) & ~7u;

		u8 iov_buf[1024 + 32];
		const char *iov_name = "clmload";
		for (int k = 0; k < 8; k++)
			iov_buf[k] = 0;
		for (int k = 0; iov_name[k]; k++)
			iov_buf[k] = (u8)iov_name[k];

		for (u32 b = 0; b < dload_len; b++)
			iov_buf[8 + b] = dload_buf[b];

		u16 in_len = (u16)(8u + dload_len);

		int err = send_ioctl(WLC_SET_VAR, iov_buf, in_len, NULL, 256, NULL);
		if (err != 0) {
			kv("wifiprobe9: CLM download failed at chunk", chunk_idx);
			kv("wifiprobe9: err status", (u32)err);
			return err;
		}

		offset += chunk_len;
		chunk_idx++;
		if ((chunk_idx % 4) == 0 || offset == clm_len) {
			kv("wifiprobe9: CLM downloaded bytes", offset);
			wdt_pet();
		}
	}
	puts_("wifiprobe9: CLM download complete!\n");
	return 0;
}

static int
dhd_check_clmload_status(void)
{
	u8 iov_buf[64];
	for (int k = 0; k < 64; k++) iov_buf[k] = 0;
	const char *iov_name = "clmload_status";
	for (int k = 0; iov_name[k]; k++)
		iov_buf[k] = (u8)iov_name[k];

	u8 resp_buf[64];
	for (int k = 0; k < 64; k++) resp_buf[k] = 0;
	u16 actual_len = 0;
	int err = send_ioctl(WLC_GET_VAR, iov_buf, 15, resp_buf, 64, &actual_len);
	kv("wifiprobe9: WLC_GET_VAR 'clmload_status' err", (u32)err);
	if (err == 0 && actual_len >= 4) {
		u32 status_val = *(u32 *)resp_buf;
		kv("wifiprobe9: clmload_status value", status_val);
		return (int)status_val;
	}
	return -1;
}

static int
dhd_set_country(const char *ccode)
{
	u8 iov_buf[64];
	for (int k = 0; k < 64; k++) iov_buf[k] = 0;
	const char *iov_name = "country";
	for (int k = 0; iov_name[k]; k++)
		iov_buf[k] = (u8)iov_name[k];

	iov_buf[8] = ccode[0];
	iov_buf[9] = ccode[1];
	iov_buf[10] = 0;
	iov_buf[11] = 0;
	*(s32 *)(iov_buf + 12) = -1;
	iov_buf[16] = ccode[0];
	iov_buf[17] = ccode[1];
	iov_buf[18] = 0;
	iov_buf[19] = 0;

	int err = send_ioctl(WLC_SET_VAR, iov_buf, 20, NULL, 256, NULL);
	kv("wifiprobe9: set country 'US' err", (u32)err);
	return err;
}

static void
dhd_get_country(void)
{
	u8 iov_buf[64];
	for (int k = 0; k < 64; k++) iov_buf[k] = 0;
	const char *iov_name = "country";
	for (int k = 0; iov_name[k]; k++)
		iov_buf[k] = (u8)iov_name[k];

	u8 resp_buf[64];
	for (int k = 0; k < 64; k++) resp_buf[k] = 0;
	u16 actual_len = 0;
	int err = send_ioctl(WLC_GET_VAR, iov_buf, 8, resp_buf, 64, &actual_len);
	kv("wifiprobe9: WLC_GET_VAR 'country' err", (u32)err);
	if (err == 0 && actual_len >= 2) {
		puts_("wifiprobe9: country: \"");
		for (unsigned k = 0; k < actual_len && k < 10 && resp_buf[k]; k++)
			put((char)resp_buf[k]);
		puts_("\"\n");
	}
}

static int
dhd_radio_up(void)
{
	/* (wifiprobe9 also asked for "isup": this firmware has no such iovar, -23 BCME_UNSUPPORTED) */
	int err = send_ioctl(WLC_UP, NULL, 0, NULL, 256, NULL);

	kv("wifiprobe9: WLC_UP status", (u32)err);
	return err;
}

/* an iovar: its name, the NUL, then the data */
static u16
iovar_buf(u8 *buf, const char *name, const void *data, u16 len)
{
	u16 n = 0;

	while (name[n]) {
		buf[n] = (u8)name[n];
		n++;
	}
	buf[n++] = 0;
	for (u16 k = 0; k < len; k++)
		buf[n + k] = data ? ((const u8 *)data)[k] : 0;
	return n + len;
}

static int
iovar_set(const char *name, const void *data, u16 len)
{
	static u8 buf[256];
	u16 n = iovar_buf(buf, name, data, len);

	return send_ioctl(WLC_SET_VAR, buf, n, NULL, 256, NULL);
}

static int
iovar_get(const char *name, void *out, u16 len)
{
	static u8 buf[256];
	u16 got = 0;
	u16 n = iovar_buf(buf, name, NULL, len);

	return send_ioctl(WLC_GET_VAR, buf, n, out, len, &got);
}

static u16
le16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static u32
le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32
be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void
putdec(s32 v)
{
	char d[12];
	int n = 0;
	u32 u = v < 0 ? (u32)-v : (u32)v;

	if (v < 0)
		put('-');
	do {
		d[n++] = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	while (n)
		put(d[--n]);
}

/* brcmfmac's fweh.h and fwil_types.h */
#define BRCMF_E_ESCAN_RESULT    69u
#define BRCMF_E_STATUS_PARTIAL  8u

static s32 escan_done = -1;     /* -1 while the scan runs, then its last event's status */
static unsigned escan_bss;
static u8 seen_bssid[64][6];
static unsigned seen_n;

/* one record of an escan result: struct brcmf_bss_info_le (natural alignment), each BSS once */
static void
show_bss(const u8 *bi)
{
	const u8 *bssid = bi + 8;
	u8 ssid_len = bi[18];

	for (unsigned s = 0; s < seen_n; s++) {
		unsigned m = 0;

		while (m < 6 && seen_bssid[s][m] == bssid[m])
			m++;
		if (m == 6)
			return;
	}
	if (seen_n < 64) {
		for (unsigned m = 0; m < 6; m++)
			seen_bssid[seen_n][m] = bssid[m];
		seen_n++;
	}
	escan_bss++;
	puts_("wifiprobe9: BSS ");
	for (unsigned m = 0; m < 6; m++) {
		hex(bssid[m], 2);
		if (m < 5)
			put(':');
	}
	puts_(" ch ");
	putdec(le16(bi + 72) & 0xff);           /* chanspec */
	puts_(" rssi ");
	putdec((s16)le16(bi + 78));
	puts_(" \"");
	for (unsigned k = 0; k < ssid_len && k < 32; k++) {
		u8 c = bi[19 + k];

		put(c >= 0x20 && c < 0x7f ? (char)c : '?');
	}
	puts_("\"\n");
}

/*
 * An event buffer: (rx_dataoffset bytes, then) an Ethernet header of type 0x886c, Broadcom's 10-byte
 * header, struct brcmf_event_msg_be (48 bytes, big endian), then the event's data.
 */
static void
handle_event(const u8 *buf, u16 len)
{
	unsigned e;

	for (e = 0; e <= 64 && e + 72u <= len; e += 2)
		if (buf[e + 12] == 0x88 && buf[e + 13] == 0x6c)
			break;
	if (e > 64 || e + 72u > len) {
		kv("wifiprobe9: an event without Broadcom's header, length", len);
		return;
	}

	const u8 *m = buf + e + 24;
	u32 type = be32(m + 4), status = be32(m + 8), datalen = be32(m + 20);
	const u8 *data = m + 48;
	const u8 *end = buf + len;

	if (data + datalen < end)
		end = data + datalen;
	if (type != BRCMF_E_ESCAN_RESULT) {
		puts_("wifiprobe9: event ");
		putdec((s32)type);
		puts_(" status ");
		putdec((s32)status);
		put('\n');
		return;
	}
	if (status != BRCMF_E_STATUS_PARTIAL) {
		escan_done = (s32)status;
		return;
	}

	/* struct brcmf_escan_result_le: buflen, version, sync_id, bss_count, then the records */
	if (data + 12 > end)
		return;
	u16 count = le16(data + 10);
	const u8 *bi = data + 12;

	for (u16 b = 0; b < count && bi + 80 <= end; b++) {
		u32 bl = le32(bi + 4);

		show_bss(bi);
		if (bl < 80)
			break;
		bi += bl;
	}
}

/* the ESCAN_RESULT event on, in the firmware's 16-byte event mask */
static int
dhd_escan_events(void)
{
	u8 mask[16];
	int err = iovar_get("event_msgs", mask, sizeof(mask));

	kv("wifiprobe9: get event_msgs err", (u32)err);
	if (err) {
		for (unsigned k = 0; k < sizeof(mask); k++)
			mask[k] = 0;
	}
	puts_("wifiprobe9: event_msgs as found ");
	for (unsigned k = 0; k < sizeof(mask); k++)
		hex(mask[k], 2);
	put('\n');
	mask[BRCMF_E_ESCAN_RESULT / 8] |= (u8)(1u << (BRCMF_E_ESCAN_RESULT % 8));
	err = iovar_set("event_msgs", mask, sizeof(mask));
	kv("wifiprobe9: set event_msgs err", (u32)err);
	return err;
}

/*
 * struct brcmf_escan_params_le, version 2 with struct brcmf_scan_params_v2_le: the firmware has
 * "scan_ver" (wifiprobe9), which is how brcmfmac turns on BRCMF_FEAT_SCAN_V2.
 */
static int
dhd_escan(void)
{
	static u8 p[8 + 72 + 36] __attribute__((aligned(8)));
	u8 *v2 = p + 8;

	for (unsigned k = 0; k < sizeof(p); k++)
		p[k] = 0;
	*(u32 *)(p + 0) = 2;                    /* BRCMF_ESCAN_REQ_VERSION_V2 */
	*(u16 *)(p + 4) = 1;                    /* WL_ESCAN_ACTION_START */
	*(u16 *)(p + 6) = 0x1234;               /* sync_id */
	*(u16 *)(v2 + 0) = 2;                   /* BRCMF_SCAN_PARAMS_VERSION_V2 */
	*(u16 *)(v2 + 2) = 72 + 36;             /* the fixed part and one SSID */
	for (unsigned k = 0; k < 6; k++)
		v2[40 + k] = 0xff;              /* any BSSID */
	v2[46] = 2;                             /* DOT11_BSSTYPE_ANY */
	*(u32 *)(v2 + 48) = 0;                  /* BRCMF_SCANTYPE_ACTIVE */
	*(s32 *)(v2 + 52) = -1;                 /* nprobes */
	*(s32 *)(v2 + 56) = -1;                 /* active_time */
	*(s32 *)(v2 + 60) = -1;                 /* passive_time */
	*(s32 *)(v2 + 64) = -1;                 /* home_time */
	*(u32 *)(v2 + 68) = 1u << 16;           /* one SSID (the empty one at +72: any), every channel */
	return iovar_set("escan", p, sizeof(p));
}

static void
dhd_wait_escan(u32 ms)
{
	s16 st;

	for (u32 t = 0; t < ms && escan_done < 0; t++) {
		drain_d2h(0, &st, NULL, 0, NULL);
		mdelay(1);
		if ((t & 0x7f) == 0)
			wdt_pet();
	}
	kv("wifiprobe9: escan's last status (0 done, 0xffffffff still running)", (u32)escan_done);
	kv("wifiprobe9: networks seen", escan_bss);
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	const u64 t_start = now_us();
	u32 v, s;
	u64 i, t0;
	int linked = 0;

	(void)el;
	(void)entry_el;
	(void)dtb;
	wdt_pet();

	kv("wifiprobe9: pmu 0x3ec4", *reg(PMU_ISOL));
	*reg(CMU_TOP + 0x20c8) = GATE_ON(*reg(CMU_TOP + 0x20c8));
	*reg(CMU_HSI2 + 0x620) |= 1u << 4;
	for (unsigned k = 0; k < 1000 && (*reg(CMU_HSI2 + 0x620) & (1u << 16)); k++)
		udelay(1);
	for (u32 off = 0x2008; off <= 0x20e4; off += 4)
		if ((off >= 0x2008 && off <= 0x200c) || (off >= 0x2018 && off <= 0x2024) ||
		    (off >= 0x2050 && off <= 0x2074) || (off >= 0x207c && off <= 0x2080) ||
		    (off >= 0x2094 && off <= 0x20a0) || (off >= 0x20c8 && off <= 0x20e4))
			*reg(CMU_HSI2 + off) = GATE_ON(*reg(CMU_HSI2 + off));
	udelay(100);

	pin_field(GPIO_PUD, PIN_PERST, 0);
	pin_set(PIN_PERST, 0);
	pin_field(GPIO_CON, PIN_PERST, 1);
	pin_set(PIN_PERST, 0);
	pin_field(GPIO_PUD, PIN_CLKREQ, 3);
	pin_field(GPIO_CON, PIN_CLKREQ, 2);
	pin_field(GPIO_PUD, PIN_WL_REG_ON, 0);
	pin_set(PIN_WL_REG_ON, 1);
	pin_field(GPIO_CON, PIN_WL_REG_ON, 1);
	pin_set(PIN_WL_REG_ON, 1);
	mdelay(200);
	wdt_pet();

	phy_all_pwrdn_clear();
	phy_config();
	wdt_pet();

	poll("PLL lock", PCIE1_PHY + 0x3f0, 1u << 3, 1u << 3, 10000);
	poll("CDR lock", PCIE1_PHY + 0xe0c, 1u << 2, 1u << 2, 10000);
	poll("OC done", PCIE1_PHY + 0xde8, 0xf, 0xf, 10000);
	wdt_pet();

	v = *reg(PCIE1_ELBI + ELBI_SOFT_RESET);
	v &= ~SOFT_PWR_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	mdelay(1);
	v |= SOFT_PWR_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	*reg(PCIE1_ELBI + ELBI_DEVICE_TYPE) = 4;
	v = *reg(PCIE1_ELBI + ELBI_SOFT_RESET);
	v |= SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	udelay(10);
	v &= ~SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	mdelay(1);
	v |= SOFT_NON_STICKY_RESET;
	*reg(PCIE1_ELBI + ELBI_SOFT_RESET) = v;
	*reg(PCIE1_DBI + 0x890) = 0x12000;

	pin_set(PIN_PERST, 1);
	mdelay(20);

	v = *reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1);
	v |= 1u | (1u << 4);
	*reg(PCIE1_ELBI + ELBI_APP_REQ_EXIT_L1) = v;
	*reg(PCIE1_ELBI + ELBI_LINKDOWN_RST_CTRL) = 1u << 1;
	v = *reg(PCIE1_ELBI + ELBI_QCH_SEL);
	v &= ~((0xfu << 8) | (0xfu << 4) | 0xfu);
	*reg(PCIE1_ELBI + ELBI_QCH_SEL) = v;
	*reg(PCIE1_ELBI + ELBI_MSTR_PEND_SEL_NAK) = 1;
	*reg(PCIE1_ELBI + ELBI_DBI_L1_EXIT_DIS) = 1;

	*reg(PCIE1_DBI + 0x8bc) = 1;
	*reg(PCIE1_DBI + 0x00) = 0xeced144du;
	v = *reg(PCIE1_DBI + 0x710);
	v = (v & ~(0x3fu << 16)) | (0x1u << 16);
	*reg(PCIE1_DBI + 0x710) = v;
	v = *reg(PCIE1_DBI + 0x80c);
	v = (v & ~(0x1fu << 8)) | (0x1u << 8);
	*reg(PCIE1_DBI + 0x80c) = v;
	v = *reg(PCIE1_DBI + 0x70 + 0x0c);
	v &= ~((0x7u << 15) | 0xfu);
	v |= (0x7u << 15) | 0x3u;
	*reg(PCIE1_DBI + 0x70 + 0x0c) = v;
	*reg(PCIE1_DBI + 0x40 + 0x04) = 0;
	v = *reg(PCIE1_DBI + 0x70 + 0x30);
	v = (v & ~0xfu) | 0x2u;
	*reg(PCIE1_DBI + 0x70 + 0x30) = v;
	*reg(PCIE1_DBI + 0x18) = 0x00ff0100;
	*reg(PCIE1_DBI + 0x20) = 0x60706000;
	*reg(PCIE1_DBI + 0x04) |= 0x6;

	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 1;
	t0 = now_us();
	for (i = 0; ; i++) {
		v = *reg(PCIE1_ELBI + ELBI_RDLH_LINKUP);
		s = v & 0x3f;
		if (s == S_L0) {
			linked = 1;
			break;
		}
		if (now_us() - t0 > 500000)
			break;
		udelay(2);
		if ((i & 0xfff) == 0)
			wdt_pet();
	}
	kv(linked ? "wifiprobe9: LINK UP (L0)" : "wifiprobe9: NO L0", now_us() - t0);
	wdt_pet();

	if (linked) {
		mdelay(3);
		iatu_outbound(0, 0x4, PCIE1_CFG, 0x1000, 1u << 24);
		iatu_outbound(1, 0x0, PCIE1_MEM, 0x800000, PCIE1_MEM);

		*reg(PCIE1_CFG + 0x10) = (u32)PCIE1_BAR0_ADDR;
		*reg(PCIE1_CFG + 0x14) = 0;
		*reg(PCIE1_CFG + 0x18) = (u32)PCIE1_BAR2_ADDR;
		*reg(PCIE1_CFG + 0x1c) = 0;
		*reg(PCIE1_CFG + 0x04) = (*reg(PCIE1_CFG + 0x04) & ~0x7u) | 0x6u;
		udelay(10);

		bp_read32(CHIPC_BASE);
		*reg(PCIE1_BAR0_ADDR + 0x21e8) |= (3u << 8);
		bp_write32(0x180001e8u, bp_read32(0x180001e8u) | (3u << 8));
		*reg(PCIE1_BAR0_ADDR + 0x2a0c) |= (2u << 8);
		poll("pcie2 powerctl DMN1", PCIE1_BAR0_ADDR + 0x21e8, 2u << 16, 2u << 16, 20000);
		*reg(PCIE1_CFG + 0x88) |= 0x300u;
		*reg(PCIE1_CFG + 0x4c) |= 0x8000u;

		bp_write32(PCIE2_BASE + 0x120, 0x4e0u);
		u32 bar2cfg = bp_read32(PCIE2_BASE + 0x124);
		bp_write32(PCIE2_BASE + 0x124, bar2cfg);

		/* Download state */
		tcm_set_win(0x00000000u);
		ai_reset_core(ARMCA7_WRAP, SICF_CPUHALT, SICF_CPUHALT, SICF_CPUHALT);
		ai_reset_core(SYSMEM_WRAP, 0, 0, 0);
		tcm_write32(SHARED_ADDR_SLOT, 0);
		wdt_pet();

		/* Download firmware */
		u32 fw_len = (u32)(fw_bin_end - fw_bin_start);
		const u32 *fw_words = (const u32 *)fw_bin_start;
		u32 resetinstr = fw_words[0];
		tcm_set_win(0x00000000u);
		volatile u32 *dst = (volatile u32 *)(PCIE1_BAR2_ADDR + DONGLE_RAM_BASE);
		u32 nwords = fw_len / 4;
		for (u32 w = 0; w < nwords; w++) {
			dst[w] = fw_words[w];
			if ((w & 0xffffu) == 0)
				wdt_pet();
		}

		/* Format & download NVRAM */
		u32 raw_nv_len = (u32)(nvram_cal_end - nvram_cal_start);
		u32 varsize = format_nvram(nvram_cal_start, raw_nv_len);
		u32 varaddr = SHARED_ADDR_SLOT - varsize;
		u32 varsizew = ((~(varsize / 4u) << 16) & 0xffff0000u) | ((varsize / 4u) & 0xffffu);
		const u32 *nv_words = (const u32 *)nvram_buf;
		for (u32 off = 0; off < varsize; off += 4)
			tcm_write32(varaddr + off, nv_words[off / 4]);
		tcm_write32(SHARED_ADDR_SLOT, varsizew);

		/* RNG seed rTLV */
		u32 rtlv_addr = varaddr - 8u - RNG_HOST_NBYTES;
		u32 prng = (u32)now_us() ^ 0xa5a55a5au;
		for (u32 off = 0; off < RNG_HOST_NBYTES; off += 4) {
			prng ^= prng << 13;
			prng ^= prng >> 17;
			prng ^= prng << 5;
			if ((prng & 0xffu) == 0) prng |= 0x01u;
			if ((prng & 0xff00u) == 0) prng |= 0x0100u;
			if ((prng & 0xff0000u) == 0) prng |= 0x010000u;
			if ((prng & 0xff000000u) == 0) prng |= 0x01000000u;
			tcm_write32(rtlv_addr + off, prng);
		}
		tcm_write32(rtlv_addr + RNG_HOST_NBYTES, RNG_HOST_NBYTES);
		tcm_write32(rtlv_addr + RNG_HOST_NBYTES + 4u, DNGL_RTLV_RNG_SIG);
		wdt_pet();

		/* Boot Cortex-A7 */
		tcm_write32(0x00000000u, resetinstr);
		t0 = now_us();
		ai_reset_core(ARMCA7_WRAP, 0, 0, 0);

		u32 shared_addr = varsizew;
		for (i = 0; i < 2000000; i++) {
			shared_addr = tcm_read32(SHARED_ADDR_SLOT);
			if (shared_addr != varsizew && shared_addr != 0 && shared_addr != 0xffffffffu)
				break;
			udelay(1);
			if ((i & 0x3fff) == 0)
				wdt_pet();
		}
		kv("wifiprobe9: CA7 boot elapsed (us)", now_us() - t0);
		kv("wifiprobe9: shared_addr", shared_addr);
		wdt_pet();

		if (shared_addr >= DONGLE_RAM_BASE && shared_addr < SHARED_ADDR_SLOT) {
			mdelay(50);
			wdt_pet();

			u32 rings_info_ptr = tcm_read32(shared_addr + 0x30);
			u32 console_addr   = tcm_read32(shared_addr + 0x14);
			g_console_addr = console_addr;
			kv("wifiprobe9: rings_info_ptr", rings_info_ptr);
			wdt_pet();

			/* Step 1: Zero all host memory structures */
			puts_("wifiprobe9: Step 1: zeroing host memory structures...\n");
			for (unsigned k = 0; k < sizeof(dma_scratch_buf); k++) dma_scratch_buf[k] = 0;
			for (unsigned k = 0; k < sizeof(dma_trap_buf); k++) dma_trap_buf[k] = 0;
			for (unsigned k = 0; k < sizeof(dma_h2d_wr); k++) ((u8 *)dma_h2d_wr)[k] = 0;
			for (unsigned k = 0; k < sizeof(dma_h2d_rd); k++) ((u8 *)dma_h2d_rd)[k] = 0;
			for (unsigned k = 0; k < sizeof(dma_d2h_wr); k++) ((u8 *)dma_d2h_wr)[k] = 0;
			for (unsigned k = 0; k < sizeof(dma_d2h_rd); k++) ((u8 *)dma_d2h_rd)[k] = 0;
			for (unsigned k = 0; k < sizeof(ring0_ctrl_sub); k++) ring0_ctrl_sub[k] = 0;
			for (unsigned k = 0; k < sizeof(ring1_rxp_sub); k++) ring1_rxp_sub[k] = 0;
			for (unsigned k = 0; k < sizeof(ring2_ctrl_cpl); k++) ring2_ctrl_cpl[k] = 0;
			for (unsigned k = 0; k < sizeof(ring3_tx_cpl); k++) ring3_tx_cpl[k] = 0;
			for (unsigned k = 0; k < sizeof(ring4_rx_cpl); k++) ring4_rx_cpl[k] = 0;
			for (unsigned k = 0; k < sizeof(ioctl_req_payload); k++) ioctl_req_payload[k] = 0;
			puts_("wifiprobe9: Step 1 done: host memory structures zeroed\n");
			wdt_pet();

			/* Step 2: Configure pciedev_shared_t scratch, trap, and host_cap */
			puts_("wifiprobe9: Step 2: configuring pciedev_shared_t scratch, trap, host_cap...\n");
			tcm_write32(shared_addr + 0x34, 16u);
			tcm_write32(shared_addr + 0x38, (u32)(u64)dma_scratch_buf);
			tcm_write32(shared_addr + 0x3c, (u32)((u64)dma_scratch_buf >> 32));

			u32 host_cap = HOST_API_VERSION_7 | HOSTCAP_H2D_VALID_PHASE |
			               HOSTCAP_H2D_ENABLE_HOSTRDY | HOSTCAP_DS_NO_OOB_DW |
			               HOSTCAP_EXTENDED_TRAP_DATA | HOSTCAP_UR_FW_NO_TRAP;
			tcm_write32(shared_addr + 0x54, host_cap);
			tcm_write32(shared_addr + 0x58, (u32)(u64)dma_trap_buf);
			tcm_write32(shared_addr + 0x5c, (u32)((u64)dma_trap_buf >> 32));

			kv("wifiprobe9: host_cap written", host_cap);
			kv("wifiprobe9: scratch_buf PA", (u32)(u64)dma_scratch_buf);
			kv("wifiprobe9: trap_buf PA", (u32)(u64)dma_trap_buf);
			puts_("wifiprobe9: Step 2 done: pciedev_shared_t configured\n");
			wdt_pet();

			/* Step 3: Configure ring_info_t host DMA index array pointers */
			puts_("wifiprobe9: Step 3: populating ring_info_t host index pointers...\n");
			tcm_write32(rings_info_ptr + 0x14, (u32)(u64)dma_h2d_wr);
			tcm_write32(rings_info_ptr + 0x18, (u32)((u64)dma_h2d_wr >> 32));
			tcm_write32(rings_info_ptr + 0x1c, (u32)(u64)dma_h2d_rd);
			tcm_write32(rings_info_ptr + 0x20, (u32)((u64)dma_h2d_rd >> 32));
			tcm_write32(rings_info_ptr + 0x24, (u32)(u64)dma_d2h_wr);
			tcm_write32(rings_info_ptr + 0x28, (u32)((u64)dma_d2h_wr >> 32));
			tcm_write32(rings_info_ptr + 0x2c, (u32)(u64)dma_d2h_rd);
			tcm_write32(rings_info_ptr + 0x30, (u32)((u64)dma_d2h_rd >> 32));

			u32 ringmem_ptr   = tcm_read32(rings_info_ptr + 0x00);
			u32 h2d_w_idx_ptr = tcm_read32(rings_info_ptr + 0x04);
			u32 h2d_r_idx_ptr = tcm_read32(rings_info_ptr + 0x08);
			u32 d2h_w_idx_ptr = tcm_read32(rings_info_ptr + 0x0c);
			u32 d2h_r_idx_ptr = tcm_read32(rings_info_ptr + 0x10);

			kv("wifiprobe9: ringmem_ptr", ringmem_ptr);
			kv("wifiprobe9: h2d_w_idx_ptr", h2d_w_idx_ptr);
			kv("wifiprobe9: d2h_w_idx_ptr", d2h_w_idx_ptr);
			kv("wifiprobe9: h2d_r_idx_ptr", h2d_r_idx_ptr);
			kv("wifiprobe9: d2h_r_idx_ptr", d2h_r_idx_ptr);
			u32 shared_flags = tcm_read32(shared_addr);
			kv("wifiprobe9: shared flags", shared_flags);
			kv("wifiprobe9: rx_dataoffset", tcm_read32(shared_addr + 36));
			g_dma_idx = (shared_flags & 0x10000u) != 0;             /* BRCMF_PCIE_SHARED_DMA_INDEX */
			kv("wifiprobe9: index size in host memory (0: TCM indices)",
			   g_dma_idx ? ((shared_flags & 0x100000u) ? 2 : 4) : 0); /* BRCMF_PCIE_SHARED_DMA_2B_IDX */
			puts_("wifiprobe9: Step 3 done: host index pointers written\n");
			wdt_pet();

			/* Step 4: Clear TCM ring write/read indices */
			puts_("wifiprobe9: Step 4: clearing TCM ring write/read indices...\n");
			/* (TCM indices only: 4 bytes apart as in brcmfmac; 2 H2D and 3 D2H common rings) */
			for (unsigned r = 0; r < 3; r++) {
				if (r < 2) {
					tcm_idx_write(h2d_w_idx_ptr + r * 4u, 0);
					tcm_idx_write(h2d_r_idx_ptr + r * 4u, 0);
				}
				tcm_idx_write(d2h_w_idx_ptr + r * 4u, 0);
				tcm_idx_write(d2h_r_idx_ptr + r * 4u, 0);
			}
			puts_("wifiprobe9: Step 4 done: TCM ring indices cleared\n");
			wdt_pet();

			/* Step 5: Configure 5 common rings in TCM */
			puts_("wifiprobe9: Step 5: writing 5 common ring descriptors to TCM...\n");
			struct {
				u16 idx;
				u8  type;
				u16 max_item;
				u16 len_items;
				u64 pa;
			} cmn_rings[5] = {
				{ 0, 1, 256, 40, (u64)ring0_ctrl_sub },
				{ 1, 3, 64,  32, (u64)ring1_rxp_sub },
				{ 2, 1, 256, 24, (u64)ring2_ctrl_cpl },
				{ 3, 2, 64,  24, (u64)ring3_tx_cpl },
				{ 4, 3, 64,  40, (u64)ring4_rx_cpl },
			};

			for (unsigned r = 0; r < 5; r++) {
				u32 rm = ringmem_ptr + r * 16u;
				u32 w0 = (u32)cmn_rings[r].idx | ((u32)cmn_rings[r].type << 16);
				u32 w1 = (u32)cmn_rings[r].max_item | ((u32)cmn_rings[r].len_items << 16);
				tcm_write32(rm + 0x00, w0);
				tcm_write32(rm + 0x04, w1);
				tcm_write32(rm + 0x08, (u32)cmn_rings[r].pa);
				tcm_write32(rm + 0x0c, (u32)(cmn_rings[r].pa >> 32));
			}
			puts_("wifiprobe9: Step 5 done: 5 common rings initialized in TCM\n");
			wdt_pet();

			/* Step 6: Enable PCIE2 interrupt mask */
			puts_("wifiprobe9: Step 6: unmasking PCIE2 interrupt (0xC34)...\n");
			bp_write32(PCIE2_BASE + 0xC34, 0x300u);
			kv("wifiprobe9: PCIE2 interrupt mask (0xC34)", bp_read32(PCIE2_BASE + 0xC34));
			puts_("wifiprobe9: Step 6 done: PCIE2 interrupt unmasked\n");
			wdt_pet();

			/* Memory barrier before doorbells */
			__asm__ volatile("dsb sy" ::: "memory");

			/* Step 7: Signal hostready */
			puts_("wifiprobe9: Step 7: signaling hostready (PCIH2D_DB1 = 0x12345678)...\n");
			bp_write32(PCIE2_BASE + 0x144, H2D_HOSTREADY_MAGIC);
			mdelay(10);
			wdt_pet();
			puts_("wifiprobe9: Step 7 done: hostready signaled\n");

			/* Step 8: Post 8 response and 8 event buffers on Ring 0 */
			puts_("wifiprobe9: Step 8: posting 8 response and 8 event buffers...\n");
			u8 epoch = 1;
			u16 r0_wr = 0;
			for (u16 b = 0; b < 8; b++) {
				ring0_post_buf(r0_wr++, MSG_TYPE_IOCTLRESP_BUF_POST, epoch++,
				               1u + b, sizeof(ioctl_resp_buf[0]), (u64)ioctl_resp_buf[b]);
			}
			for (u16 b = 0; b < 8; b++) {
				ring0_post_buf(r0_wr++, MSG_TYPE_EVENT_BUF_POST, epoch++,
				               101u + b, 2048, (u64)event_resp_buf[b]);
			}
			puts_("wifiprobe9: Step 8 done: buffers posted to Ring 0\n");
			wdt_pet();

			/* Step 9: Post WLC_GET_VAR cur_etheraddr */
			puts_("wifiprobe9: Step 9: preparing WLC_GET_VAR 'cur_etheraddr' IOCTL...\n");
			const char *iov_name = "cur_etheraddr";
			u16 iov_len = 0;
			while (iov_name[iov_len]) {
				ioctl_req_payload[iov_len] = (u8)iov_name[iov_len];
				iov_len++;
			}
			ioctl_req_payload[iov_len++] = 0; /* 14 bytes */

			u16 ioctl_slot = r0_wr++;
			ring0_post_ioctl_req(ioctl_slot, epoch++, WLC_GET_VAR, 1u,
			                     iov_len, 256, (u64)ioctl_req_payload);

			__asm__ volatile("dsb sy" ::: "memory");
			puts_("wifiprobe9: Step 9 done: IOCTL posted to Ring 0 memory\n");
			wdt_pet();

			/* Step 10: Update Ring 0 write index */
			puts_("wifiprobe9: Step 10: updating Ring 0 write index...\n");
			dma_h2d_wr[0] = r0_wr;
			tcm_idx_write(h2d_w_idx_ptr, r0_wr);
			kv("wifiprobe9: Ring 0 write index updated to", r0_wr);
			puts_("wifiprobe9: Step 10 done: Ring 0 write index updated\n");
			wdt_pet();

			/* Step 11: Ring H2D doorbell */
			puts_("wifiprobe9: Step 11: ringing H2D doorbell (PCIH2D_MailBox = 0xdeadbeef)...\n");
			bp_write32(PCIE2_BASE + 0x140, DHD_WRPTR_UPDATE_MAGIC);
			puts_("wifiprobe9: Step 11 done: H2D doorbell rang\n");
			wdt_pet();

			/* Step 12: Poll for dongle response on Ring 2 */
			puts_("wifiprobe9: Step 12: polling for dongle response on Ring 2...\n");
			t0 = now_us();
			u16 last_h2d_rd = 0xffff, last_d2h_wr = 0xffff;
			int got_cpl = 0;
			for (i = 0; i < 200000; i++) {
				u16 cur_h2d_rd = idx_read(h2d_r_idx_ptr, &dma_h2d_rd[0]);
				u16 cur_d2h_wr = idx_read(d2h_w_idx_ptr, &dma_d2h_wr[0]);

				if (cur_h2d_rd != last_h2d_rd) {
					last_h2d_rd = cur_h2d_rd;
					kv("wifiprobe9: dongle advanced H2D RD index to", cur_h2d_rd);
				}
				if (cur_d2h_wr != last_d2h_wr) {
					last_d2h_wr = cur_d2h_wr;
					kv("wifiprobe9: dongle advanced D2H WR index to", cur_d2h_wr);
				}

				/* Check host DMA memory for Ring 2 item 0 & item 1 */
				if (ring2_ctrl_cpl[0] != 0 || dma_d2h_wr[0] != 0) {
					got_cpl = 1;
					break;
				}

				udelay(5);
				if ((i & 0x3ff) == 0)
					wdt_pet();
			}

			kv("wifiprobe9: completion wait elapsed (us)", now_us() - t0);
			kv("wifiprobe9: final H2D RD index", idx_read(h2d_r_idx_ptr, &dma_h2d_rd[0]));
			kv("wifiprobe9: final D2H WR index", idx_read(d2h_w_idx_ptr, &dma_d2h_wr[0]));
			kv("wifiprobe9: final Host DMA D2H WR index", dma_d2h_wr[0]);
			kv("wifiprobe9: PCIE2 interrupt status (0xC30)", bp_read32(PCIE2_BASE + 0xC30));

			if (!got_cpl) {
				puts_("wifiprobe9: Step 12 TIMEOUT: no completion on Ring 2!\n");
			} else {
				puts_("wifiprobe9: Step 12 done: completion received on Ring 2!\n");
			}
			wdt_pet();

			/* Step 13: Dump Ring 2 completion slots */
			puts_("wifiprobe9: Step 13: === Ring 2 (D2H_CONTROL_COMPLETE) Items ===\n");
			for (unsigned item = 0; item < 4; item++) {
				u8 *msg = ring2_ctrl_cpl + item * 24u;
				puts_("  item[");
				hex(item, 1);
				puts_("]: type=0x");
				hex(msg[0], 2);
				puts_(" if_id=");
				hex(msg[1], 2);
				puts_(" flags=0x");
				hex(msg[2], 2);
				puts_(" epoch=");
				hex(msg[3], 2);
				puts_(" req_id=0x");
				hex(*(volatile u32 *)(msg + 4), 8);
				puts_(" status=");
				hex(*(volatile u16 *)(msg + 8), 4);
				puts_(" flowid=");
				hex(*(volatile u16 *)(msg + 10), 4);
				put('\n');

				/* If this is MSG_TYPE_IOCTL_CMPLT (0x0c), inspect payload */
				if (msg[0] == MSG_TYPE_IOCTL_CMPLT) {
					u16 resp_len = *(volatile u16 *)(msg + 12);
					u16 trans_id = *(volatile u16 *)(msg + 14);
					u32 req_id   = *(volatile u32 *)(msg + 4);
					kv("wifiprobe9: IOCTL_CMPLT resp_len", resp_len);
					kv("wifiprobe9: IOCTL_CMPLT trans_id", trans_id);
					kv("wifiprobe9: IOCTL_CMPLT matching buffer req_id", req_id);

					/* If req_id is 1..8, inspect that ioctl_resp_buf */
					if (req_id >= 1 && req_id <= 8) {
						u8 *resp = ioctl_resp_buf[req_id - 1];
						puts_("wifiprobe9: Factory MAC Address: ");
						for (int m = 0; m < 6; m++) {
							hex(resp[m], 2);
							if (m < 5) put(':');
						}
						put('\n');
					}
				}
			}
			puts_("wifiprobe9: Step 13 done: completions parsed\n");
			wdt_pet();

			/* Repost response buffer 8 (consumed by cur_etheraddr) */
			ring0_post_buf(r0_wr++, MSG_TYPE_IOCTLRESP_BUF_POST, epoch++,
			               8u, sizeof(ioctl_resp_buf[0]), (u64)ioctl_resp_buf[7]);
			__asm__ volatile("dsb sy" ::: "memory");
			dma_h2d_wr[0] = r0_wr;
			tcm_idx_write(h2d_w_idx_ptr, r0_wr);
			bp_write32(PCIE2_BASE + 0x140, DHD_WRPTR_UPDATE_MAGIC);

			/* Update host read index for completions consumed in Step 13 */
			r2_rd_idx = 3;
			dma_d2h_rd[0] = 3;
			tcm_idx_write(d2h_r_idx_ptr, 3);

			/* Initialize globals for send_ioctl */
			r0_wr_idx = r0_wr;
			r0_epoch = epoch;
			cur_trans_id = 1;
			g_h2d_w_idx_ptr = h2d_w_idx_ptr;
			g_d2h_w_idx_ptr = d2h_w_idx_ptr;
			g_d2h_r_idx_ptr = d2h_r_idx_ptr;
			show_console();
			wdt_pet();

			/* Step 14: Download CLM regulatory blob */
			puts_("wifiprobe9: Step 14: downloading CLM blob (regulatory data)...\n");
			u32 clm_len = (u32)(clm_blob_end - clm_blob_start);
			int clm_err = dhd_clmload(clm_blob_start, clm_len);
			kv("wifiprobe9: Step 14 clmload result", (u32)clm_err);
			dhd_check_clmload_status();
			puts_("wifiprobe9: Step 14 done\n");
			show_console();
			wdt_pet();

			/* Step 15: Set country code */
			puts_("wifiprobe9: Step 15: setting country to US...\n");
			dhd_set_country("US");
			dhd_get_country();
			puts_("wifiprobe9: Step 15 done\n");
			show_console();
			wdt_pet();

			/* Step 16: Bring radio UP (WLC_UP = 2) */
			puts_("wifiprobe9: Step 16: bringing radio UP (WLC_UP)...\n");
			dhd_radio_up();
			puts_("wifiprobe9: Step 16 done\n");
			show_console();
			wdt_pet();

			/* Step 17: what an escan needs: mpc off (brcmfmac's brcmf_scan_config_mpc), its event on */
			puts_("wifiprobe9: Step 17: mpc 0, ESCAN_RESULT event on...\n");
			s32 zero = 0;
			kv("wifiprobe9: set mpc 0 err", (u32)iovar_set("mpc", &zero, 4));
			dhd_escan_events();
			puts_("wifiprobe9: Step 17 done\n");
			show_console();
			wdt_pet();

			/* Step 18: an escan as brcmfmac's brcmf_run_escan starts one: active, any SSID, every channel */
			puts_("wifiprobe9: Step 18: starting escan...\n");
			kv("wifiprobe9: escan err", (u32)dhd_escan());
			show_console();
			wdt_pet();

			/* Step 19: the results come as ESCAN_RESULT events, the last one with status SUCCESS */
			puts_("wifiprobe9: Step 19: waiting for scan results (up to 15 s)...\n");
			dhd_wait_escan(15000);
			puts_("wifiprobe9: Step 19 done\n");
			wdt_pet();

			/* Dump scratch buffer readback */
			kv("wifiprobe9: scratch_buf[0..3]", *(u32 *)dma_scratch_buf);
			kv("wifiprobe9: scratch_buf[4..7]", *(u32 *)(dma_scratch_buf + 4));

			/* Dump firmware console log */
			if (console_addr >= DONGLE_RAM_BASE && console_addr < SHARED_ADDR_SLOT)
				dump_fw_console(console_addr);
		}
	}

	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 0;
	pin_set(PIN_PERST, 0);
	pin_set(PIN_WL_REG_ON, 0);
	kv("wifiprobe9: total us", now_us() - t_start);
	puts_("wifiprobe9: done; letting the watchdog reset the phone\n");
	hang();
}
