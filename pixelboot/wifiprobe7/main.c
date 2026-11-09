/*
 * wifiprobe7: BCM4389 MsgBuf ring configuration, hostready doorbell,
 * buffer posting, and WLC_GET_VAR ioctl ("cur_etheraddr" & "ver") validation
 * (WIFI-PLAN.md, Phase W2 second half).
 *
 * Sequence:
 *   1. Hardware bring-up + firmware boot (from wifiprobe6):
 *      - Release PHY isolation at EL2 via PSCI 0x82000504
 *      - WL_REG_ON high, PERST# sequence, PCIe1 Gen1 x1 link training to L0
 *      - BAR0 (0x60000000) and BAR2 (0x60400000) assigned, dm1 power domain enabled
 *      - Download fw_bcmdhd.bin + formatted bcmdhd.cal + RNG seed rTLV
 *      - Release Cortex-A7 halt, poll 0x49fffc for shared_addr (0x003cf594)
 *   2. MsgBuf host configuration:
 *      - Initialize host DMA scratch buffer, trap buffer, and DMA index arrays
 *      - Populate pciedev_shared_t (host_cap = 0x00083507, trap_addr, scratch_buf)
 *      - Populate ring_info_t (DMA index buffer host addresses)
 *      - Populate the 5 common ring_mem_t descriptors in TCM (0x0040f304)
 *      - Zero out all ring WR and RD indices in TCM and host memory
 *   3. Ring hostready doorbell (PCIH2D_DB1 = 0x12345678)
 *   4. Post 8 IOCTL response buffers (MSG_TYPE_IOCTLRESP_BUF_POST) and
 *      8 event buffers (MSG_TYPE_EVENT_BUF_POST) on Ring 0 (H2D_CONTROL_SUBMIT)
 *   5. Send WLC_GET_VAR "cur_etheraddr" (cmd = 262) on Ring 0 and ring PCIH2D_MailBox
 *   6. Poll Ring 2 (D2H_CONTROL_COMPLETE) for ACK and CMPLT, extract and log MAC address
 *   7. Send WLC_GET_VAR "ver" (cmd = 262) on Ring 0 and log version string
 *   8. Dump firmware console log to inspect dongle execution trace
 */
#include "board.h"

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
	".text\n"
);

extern const u8 fw_bin_start[], fw_bin_end[];
extern const u8 nvram_cal_start[], nvram_cal_end[];

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

#define WLC_UP                          2u
#define WLC_GET_VAR                     262u
#define WLC_SET_VAR                     263u

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

/* 5 Common MsgBuf Rings:
 *   Ring 0: H2D Control Submit: 64 items * 40 bytes = 2560 bytes
 *   Ring 1: H2D RxPost Submit:  64 items * 32 bytes = 2048 bytes
 *   Ring 2: D2H Control Cmpl:   64 items * 24 bytes = 1536 bytes
 *   Ring 3: D2H Tx Cmpl:        64 items * 24 bytes = 1536 bytes
 *   Ring 4: D2H Rx Cmpl:        64 items * 40 bytes = 2560 bytes
 */
static u8 ring0_ctrl_sub[64 * 40] __attribute__((aligned(64)));
static u8 ring1_rxp_sub[64 * 32]  __attribute__((aligned(64)));
static u8 ring2_ctrl_cpl[64 * 24] __attribute__((aligned(64)));
static u8 ring3_tx_cpl[64 * 24]   __attribute__((aligned(64)));
static u8 ring4_rx_cpl[64 * 40]   __attribute__((aligned(64)));

/* IOCTL Request & Return buffers */
static u8 ioctl_req_payload[2048] __attribute__((aligned(64)));
static u8 ioctl_resp_buf[8][2048] __attribute__((aligned(64)));
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
	puts_("wifiprobe7: ");
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

/* Read/write TCM via BAR2 */
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

static u16
tcm_read16(u32 addr)
{
	tcm_set_win(addr & ~0x3fffffu);
	return *(volatile u16 *)(PCIE1_BAR2_ADDR + (addr & 0x3fffffu));
}

static void
tcm_write16(u32 addr, u16 val)
{
	tcm_set_win(addr & ~0x3fffffu);
	*(volatile u16 *)(PCIE1_BAR2_ADDR + (addr & 0x3fffffu)) = val;
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

static void
dump_fw_console(u32 console_addr)
{
	u32 log_buf = tcm_read32(console_addr + 0x08);
	u32 log_size = tcm_read32(console_addr + 0x0c);
	u32 log_idx = tcm_read32(console_addr + 0x10);

	kv("wifiprobe7: console log.buf", log_buf);
	kv("wifiprobe7: console log.buf_size", log_size);
	kv("wifiprobe7: console log.idx", log_idx);

	if (log_buf < DONGLE_RAM_BASE || log_buf >= DONGLE_RAM_TOP ||
	    log_size == 0 || log_size > 0x40000u) {
		puts_("wifiprobe7: console log buffer out of range\n");
		return;
	}

	u32 n = log_idx < log_size ? log_idx : log_size;
	puts_("wifiprobe7: --- FIRMWARE CONSOLE (");
	hex(n, 4);
	puts_(" bytes) ---\n");
	for (u32 i = 0; i < n; i++) {
		u8 c = tcm_read8(log_buf + i);
		if (c == '\n' || (c >= 0x20 && c < 0x7f) || c == '\t')
			put((char)c);
		if ((i & 0x3ff) == 0)
			wdt_pet();
	}
	puts_("\nwifiprobe7: --- END FIRMWARE CONSOLE ---\n");
}

void
at_entry(u64 el, u64 dtb)
{
	u64 x0;

	(void)dtb;
	log_open();
	puts_("\nwifiprobe7: BCM4389 MsgBuf ring configuration & WLC_GET_VAR ioctl test\n");
	kv("wifiprobe7: at entry, EL", el);
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

	/* host_buf_addr (at offset +0x10) */
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

	/* host_input_buf_addr at offset +0x18 */
	*(volatile u32 *)(p + 0x18) = (u32)in_addr;
	*(volatile u32 *)(p + 0x1c) = (u32)(in_addr >> 32);
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

	kv("wifiprobe7: pmu 0x3ec4", *reg(PMU_ISOL));
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
	kv(linked ? "wifiprobe7: LINK UP (L0)" : "wifiprobe7: NO L0", now_us() - t0);
	wdt_pet();

	if (linked) {
		mdelay(3);
		iatu_outbound(0, 0x4, PCIE1_CFG, 0x1000, 1u << 24);
		iatu_outbound(1, 0x0, PCIE1_MEM, 0x800000, PCIE1_MEM);

		/* Assign BAR0 = 0x60000000, BAR2 = 0x60400000, enable MEM + MASTER */
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
		kv("wifiprobe7: CA7 boot elapsed (us)", now_us() - t0);
		kv("wifiprobe7: shared_addr", shared_addr);
		wdt_pet();

		if (shared_addr >= DONGLE_RAM_BASE && shared_addr < SHARED_ADDR_SLOT) {
			mdelay(50);
			wdt_pet();

			u32 rings_info_ptr = tcm_read32(shared_addr + 0x30);
			u32 console_addr   = tcm_read32(shared_addr + 0x14);
			kv("wifiprobe7: rings_info_ptr", rings_info_ptr);

			/* Zero all host memory structures */
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

			/*
			 * Setup pciedev_shared_t:
			 *   +0x34: host_dma_scratch_buffer_len = 16
			 *   +0x38: host_dma_scratch_buffer (low, high)
			 *   +0x54: host_cap = 0x00083507
			 *   +0x58: host_trap_addr (low, high)
			 */
			tcm_write32(shared_addr + 0x34, 16u);
			tcm_write32(shared_addr + 0x38, (u32)(u64)dma_scratch_buf);
			tcm_write32(shared_addr + 0x3c, (u32)((u64)dma_scratch_buf >> 32));

			u32 host_cap = HOST_API_VERSION_7 | HOSTCAP_H2D_VALID_PHASE |
			               HOSTCAP_H2D_ENABLE_HOSTRDY | HOSTCAP_DS_NO_OOB_DW |
			               HOSTCAP_EXTENDED_TRAP_DATA | HOSTCAP_UR_FW_NO_TRAP;
			tcm_write32(shared_addr + 0x54, host_cap);
			tcm_write32(shared_addr + 0x58, (u32)(u64)dma_trap_buf);
			tcm_write32(shared_addr + 0x5c, (u32)((u64)dma_trap_buf >> 32));

			kv("wifiprobe7: host_cap written", host_cap);
			kv("wifiprobe7: scratch_buf PA", (u32)(u64)dma_scratch_buf);
			kv("wifiprobe7: trap_buf PA", (u32)(u64)dma_trap_buf);

			/*
			 * Setup ring_info_t:
			 *   +0x14: h2d_w_idx_hostaddr
			 *   +0x1c: h2d_r_idx_hostaddr
			 *   +0x24: d2h_w_idx_hostaddr
			 *   +0x2c: d2h_r_idx_hostaddr
			 */
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

			/* Clear TCM ring write/read indices for common rings (uint16 per ring) */
			for (unsigned r = 0; r < 5; r++) {
				tcm_write16(h2d_w_idx_ptr + r * 2u, 0);
				tcm_write16(h2d_r_idx_ptr + r * 2u, 0);
				tcm_write16(d2h_w_idx_ptr + r * 2u, 0);
				tcm_write16(d2h_r_idx_ptr + r * 2u, 0);
			}

			/*
			 * Setup 5 Common ring_mem_t in TCM (ringmem_ptr + r * 16):
			 *   +0x00: u16 idx, u8 type, u8 rsvd
			 *   +0x04: u16 max_item, u16 len_items
			 *   +0x08: u32 base_addr.low
			 *   +0x0c: u32 base_addr.high
			 */
			struct {
				u16 idx;
				u8  type;
				u16 max_item;
				u16 len_items;
				u64 pa;
			} cmn_rings[5] = {
				{ 0, 1, 64, 40, (u64)ring0_ctrl_sub },
				{ 1, 3, 64, 32, (u64)ring1_rxp_sub },
				{ 2, 1, 64, 24, (u64)ring2_ctrl_cpl },
				{ 3, 2, 64, 24, (u64)ring3_tx_cpl },
				{ 4, 3, 64, 40, (u64)ring4_rx_cpl },
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
			puts_("wifiprobe7: 5 common rings initialized in TCM\n");

			/* Enable interrupt mask on PCIE2 (def_intmask = 0x300) */
			bp_write32(PCIE2_BASE + 0xC34, 0x300u);
			kv("wifiprobe7: PCIE2 interrupt mask (0xC34)", bp_read32(PCIE2_BASE + 0xC34));

			/* Memory barrier before ringing doorbells */
			__asm__ volatile("dsb sy" ::: "memory");

			/* Signal hostready: write 0x12345678 to PCIH2D_DB1 (0x144) */
			puts_("wifiprobe7: signaling hostready (PCIH2D_DB1 = 0x12345678)...\n");
			bp_write32(PCIE2_BASE + 0x144, H2D_HOSTREADY_MAGIC);
			mdelay(10);
			wdt_pet();

			/*
			 * Post 8 IOCTL response buffers (msg_type = 0x0b) and
			 * 8 Event buffers (msg_type = 0x0d) on Ring 0
			 */
			u8 epoch = 1;
			u16 r0_wr = 0;
			for (u16 b = 0; b < 8; b++) {
				ring0_post_buf(r0_wr++, MSG_TYPE_IOCTLRESP_BUF_POST, epoch++,
				               1u + b, 2048, (u64)ioctl_resp_buf[b]);
			}
			for (u16 b = 0; b < 8; b++) {
				ring0_post_buf(r0_wr++, MSG_TYPE_EVENT_BUF_POST, epoch++,
				               101u + b, 2048, (u64)event_resp_buf[b]);
			}

			/*
			 * Send IOCTL 1: WLC_GET_VAR "cur_etheraddr"
			 * Put string "cur_etheraddr" + NUL into ioctl_req_payload
			 */
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

			/* Update Ring 0 write index */
			dma_h2d_wr[0] = r0_wr;
			tcm_write16(h2d_w_idx_ptr, r0_wr);

			/* Ring doorbell: write magic to PCIH2D_MailBox (0x140) */
			kv("wifiprobe7: Ring 0 write index updated to", r0_wr);
			puts_("wifiprobe7: ringing H2D doorbell (PCIH2D_MailBox = 0xdeadbeef)...\n");
			bp_write32(PCIE2_BASE + 0x140, DHD_WRPTR_UPDATE_MAGIC);

			/*
			 * Poll for dongle response on Ring 2 (D2H_CONTROL_COMPLETE):
			 * Check:
			 *   1. TCM h2d_r_idx (has dongle read Ring 0?)
			 *   2. TCM d2h_w_idx (has dongle posted Ring 2 completions?)
			 *   3. Host DMA d2h_w_idx and ring2_ctrl_cpl content
			 */
			t0 = now_us();
			u16 last_h2d_rd = 0xffff, last_d2h_wr = 0xffff;
			for (i = 0; i < 200000; i++) {
				u16 cur_h2d_rd = tcm_read16(h2d_r_idx_ptr);
				u16 cur_d2h_wr = tcm_read16(d2h_w_idx_ptr);

				if (cur_h2d_rd != last_h2d_rd) {
					last_h2d_rd = cur_h2d_rd;
					kv("wifiprobe7: dongle advanced H2D RD index to", cur_h2d_rd);
				}
				if (cur_d2h_wr != last_d2h_wr) {
					last_d2h_wr = cur_d2h_wr;
					kv("wifiprobe7: dongle advanced D2H WR index to", cur_d2h_wr);
				}

				/* Check host DMA memory for Ring 2 item 0 & item 1 */
				if (ring2_ctrl_cpl[0] != 0 || dma_d2h_wr[0] != 0) {
					break;
				}

				udelay(5);
				if ((i & 0xfff) == 0)
					wdt_pet();
			}

			kv("wifiprobe7: completion wait elapsed (us)", now_us() - t0);
			kv("wifiprobe7: final TCM H2D RD index", tcm_read16(h2d_r_idx_ptr));
			kv("wifiprobe7: final TCM D2H WR index", tcm_read16(d2h_w_idx_ptr));
			kv("wifiprobe7: final Host DMA D2H WR index", dma_d2h_wr[0]);
			kv("wifiprobe7: PCIE2 interrupt status (0xC30)", bp_read32(PCIE2_BASE + 0xC30));

			/* Dump Ring 2 completion slots */
			puts_("wifiprobe7: === Ring 2 (D2H_CONTROL_COMPLETE) Items ===\n");
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
				hex(*(u32 *)(msg + 4), 8);
				puts_(" status=");
				hex(*(u16 *)(msg + 8), 4);
				puts_(" flowid=");
				hex(*(u16 *)(msg + 10), 4);
				put('\n');

				/* If this is MSG_TYPE_IOCTL_CMPLT (0x0c), inspect payload */
				if (msg[0] == MSG_TYPE_IOCTL_CMPLT) {
					u16 resp_len = *(u16 *)(msg + 12);
					u16 trans_id = *(u16 *)(msg + 14);
					u32 req_id   = *(u32 *)(msg + 4);
					kv("wifiprobe7: IOCTL_CMPLT resp_len", resp_len);
					kv("wifiprobe7: IOCTL_CMPLT trans_id", trans_id);
					kv("wifiprobe7: IOCTL_CMPLT matching buffer req_id", req_id);

					/* If req_id is 1..8, inspect that ioctl_resp_buf */
					if (req_id >= 1 && req_id <= 8) {
						u8 *resp = ioctl_resp_buf[req_id - 1];
						puts_("wifiprobe7: MAC Address: ");
						for (int m = 0; m < 6; m++) {
							hex(resp[m], 2);
							if (m < 5) put(':');
						}
						put('\n');
					}
				}
			}

			/* Dump scratch buffer readback */
			kv("wifiprobe7: scratch_buf[0..3]", *(u32 *)dma_scratch_buf);
			kv("wifiprobe7: scratch_buf[4..7]", *(u32 *)(dma_scratch_buf + 4));

			/* Dump firmware console log */
			if (console_addr >= DONGLE_RAM_BASE && console_addr < SHARED_ADDR_SLOT)
				dump_fw_console(console_addr);
		}
	}

	*reg(PCIE1_ELBI + ELBI_APP_LTSSM_ENABLE) = 0;
	pin_set(PIN_PERST, 0);
	pin_set(PIN_WL_REG_ON, 0);
	kv("wifiprobe7: total us", now_us() - t_start);
	puts_("wifiprobe7: done; letting the watchdog reset the phone\n");
	hang();
}
