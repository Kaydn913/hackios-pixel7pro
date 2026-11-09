# Wi-Fi on the Pixel 7 Pro (BCM4389 over PCIe) — plan and status

Goal: join a network from iOS's Settings and browse in Safari, as HackiOS does on the POCO X5 Pro
(WCN6750 / ath11k: "join from Settings, Safari over Wi-Fi, 55–90 Mbit/s"); then the Discord app
(sideloaded, decrypted; see the end).

## What Android shows (2026-10-25, `downloads/wifi/android-recon.txt`)
- **Chip**: Broadcom BCM4389, PCI `14e4:4441`, class 0x028000, at `0001:01:00.0`, driven by Google's
  `bcmdhd4389` (module, with cfg80211). Firmware 20.101.91.
- **PCIe controller**: the second one, `14520000.pcie` (`exynos-pcie-rc`, modules `pcie_exynos_gs`
  + `pcie_exynos_gs201_rc_cal` + `exynos_pcie_iommu`). Root port `144d:eced`. Its blocks:
  sysreg 0x14420000, ia 0x14500000, **elbi 0x14520000**, **pcs 0x14540000**, **phy 0x14550000**,
  udbg 0x14560000, **dbi 0x14800000**-0x14b00fff; memory window 0x60000000-0x60feffff.
  (The first controller, 11920000, is the modem: Samsung `144d:a5a5`, driver s51xx -- not ours.)
- **Firmware**, pulled to `downloads/wifi/fw/`: `fw_bcmdhd.bin` (1.7 MB), `bcmdhd_clm.blob`,
  `bcmdhd.cal` (the radio's NVRAM/calibration), `google_wifi_firmware_config_info.pb`.
  (On the phone: /vendor/firmware, readable only as root.) The DT: `android,bcmdhd_wlan` with
  `wlan_host_wake` / `wlan_dev_wake` GPIOs and the regulator VSYS_PWR_WLAN_BT (`downloads/dts/dtbo-*.dts`).

## What HackiOS has
- `hackios/wifi/lx/`: FreeBSD LinuxKPI headers (patched) + HackiOS's glue (`lx_base.c`, `lx_bus.c`,
  `lx_80211.c`, `lx_wpa.c`) on which Linux's ath11k builds for XNU (`tools/mk-ath11k.sh`), and the iOS
  side: wifid sees a network it can join from Settings; WPA in `lx_wpa.c`.
- **No PCIe**: WCN6750 is not a PCIe device. And ath11k is a mac80211 ("softmac") driver, BCM4389
  "fullmac" (cfg80211 only): `lx_80211.c` may need a cfg80211 path beside its mac80211 one.

## Phases (each ends with something that boots and says yes or no in the log)
- **W1 — the PCIe link** (a pixelboot probe, no iOS): power (the WLAN regulator, WL_REG_ON GPIO),
  the controller's clocks/power domain, PHY + PCS init, LTSSM to L0; then read the BCM4389's config
  space through the root port (`14e4:4441`). Reference: Google's GPL `pcie-exynos-gs.c` and
  `pcie-exynos-gs201-rc-cal.c` (Pixel kernel, google-modules/... or gs201 kernel source), and the
  Android boot log (`logbuffer_pcie1`, LTSSM states). Risk: PHY tuning sequences.
- **W2 — the driver**: Linux `brcmfmac` (PCIe, msgbuf protocol; BCM4389 support from Asahi Linux's work)
  on the LinuxKPI layer, with the Pixel's own firmware + NVRAM (`bcmdhd.cal`) + CLM. Goal: firmware up,
  a scan listing networks in the log. Fallback: Google's `bcmdhd` (GPL, much bigger).
- **W3 — iOS**: a cfg80211 path in `lx_80211.c` for a fullmac driver; join from Settings; DHCP; Safari.
- **Then Discord**: no App Store here (no Apple ID sign-in, FairPlay daemon exits -- README), so a
  decrypted copy of the app as a system app (the user's own, decrypted on a jailbroken iPhone), a version
  that supports iOS 16.5; text chat is plausible, voice needs audio.

## What the Pixel's device tree says (downloads/dts: soc-0.dts pcie@14520000 + dtbo-0.dts fragment@36/@74)
- On this phone: **1 lane, Gen2** (max-link-speed 2), ep-device-type 1 (BCM Wi-Fi), use-sysmmu/ia/l1ss/sicd
  true, `pmu-offset 0x3ec4` (PHY isolation, PMU base 0x18060000), supplies vreg1/vreg2 (PMIC phandles
  0x33/0x34 -- are they on after the bootloader?). pinctrl-0 = 0x16b/0x16c (CLKREQ, PERST).
- GPIOs, all in HSI2's pin controller 0x14440000 (gs201-pinctrl), bank **gph2** (offset 0): **PERST# pin 0**,
  **WL_REG_ON pin 4**, WL_DEV_WAKE pin 5; WL_HOST_WAKE is gpa6 pin 1.

## Google's sequence (downloads/ref/pcie-exynos-rc.c, android-gs-pantah-5.10-android15-qpr1)
- `exynos_pcie_rc_poweron` (l. 3333): PHY isolation bypass (PMU 0x3ec4) -> clocks (none listed) -> SysMMU
  -> pinctrl active -> phy_all_pwrdn_clear (rc-cal) -> establish_link.
- `exynos_pcie_rc_establish_link` (l. 3072): assert PHY reset + `exynos_pcie_rc_pcie_phy_config`
  (pcie-exynos-gs201-rc-cal.c, l. 133-430) -> PLL lock (phy 0x3f0 bit 3), CDR lock (0xe0c bit 2), OC done
  (0xde8 = 0xf) -> soft power reset (ELBI PCIE_SOFT_RESET) -> device type RC (ELBI 0x80 = 4) -> non-sticky
  reset -> EQ off (own conf 0x890 = 0x12000) -> **PERST# high, 20 ms for BCM Wi-Fi** -> L1 exit / Q-channel /
  NAK settings -> dw_pcie_setup_rc -> **LTSSM enable (ELBI 0x54 = 1)**, poll RDLH_LINKUP & LTSSM mask for S_L0.
- bcmdhd raises WL_REG_ON before asking for poweron.

## W1 results (2026-10-25)
- **wifiprobe1** (reads only): PMU 0x3ec4 = 0 (PHY isolated); gph2 CON = 0, DAT 0 -> PERST# and WL_REG_ON are
  inputs, low: **the chip is off**, ours to power. HSI2 sysreg answers at 0x0-0x10 (0x4 = 0xffffffff, never
  written by Google's driver on gs201), external aborts above. **ELBI's first read never returned.**
- **wifiprobe2**: the clocks were on already -- every HSI2 PCIe gate (CMU_HSI2 0x14400000 + 0x2008..0x20e4)
  0x00200000 (CG_VAL), user mux 0x620 = 0x10 (from the top), CMU_TOP gate 0x20c8 on. Forcing them on changed
  nothing: PHY/PCS external abort, ELBI hang. (Gate bits: CG_VAL 21, MANUAL 20, auto gating 28.)
- **wifiprobe3**: PMU 0x3ec4 ignores plain writes and the +0xc000 set alias. **An SMC from EL1 never returns**
  -- not even PSCI_VERSION (0x84000000). **From EL2 (probe's at_entry) they work**: PSCI 1.1; SMC_CMD_PRIV_REG
  0x82000504 (Google's include/linux/soc/samsung/exynos-smc.h; x1 address, x2 option 0 READ / 1 WRITE /
  2 RMW, x3 value) READ 0x18063ec4 = 0, WRITE 1, READ = 1. **With the isolation released everything
  answers**: PHY reset values exact (0x10=0x55 0x14=0x51 0x40=0x50 0xd8=0x28 0x1054=0x77), PCS, ELBI (0x80 = 4
  RC, 0x2c8 RDLH_LINKUP/LTSSM = 0x2200: Detect), DBI (root port 144d:a5a5 before setup, header type 1).
  So in the kernel (XNU at EL1), this SMC must go through xnuboot's EL2 monitor, as PSCI does.
- **wifiprobe4** (2026-10-25 15:08, `pixelboot/wifiprobe4/main.c`, log `downloads/probe/wifiprobe4.txt`, first try):
  **LINK UP AND THE CHIP ANSWERS: bus 1 dev 0 = `444114e4` (14e4:4441), class 0x028000 rev 03, subsystem
  14e4:4389.** Sequence (all of it ~250 ms): isolation SMC at EL2 (0 -> 1) -> PERST# gph2[0] output low +
  CLKREQ# gph2[1] fn 2 pull-up (pinctrl 0x16b/0x16c, from soc-0.dts: pcie1_clkreq / pcie1_perst) -> WL_REG_ON
  gph2[4] output high, 200 ms (**CLKREQ# went low by then: the chip powered up and asks for its clock**) ->
  phy_all_pwrdn_clear -> phy_config (reset values all as expected; num_lanes 1) -> PLL lock at once, CDR lock
  after ~3.5 ms, OC done at once; phy 0xe18 = 0x82 after the soft power reset (bit 7 ok) -> device type 4,
  non-sticky reset, EQ off (0x890 = 0x12000 reads back) -> PERST# high, 20 ms -> L1 exit / linkdown-rst manual
  / Q-channel 0 (ip-ver 0x984500) / NAK / DBI L1 exit -> a minimal dw_pcie_setup_rc + exynos_pcie_setup_rc
  (DBI_RO_WR_EN, PORT_LINK_CONTROL x1, GEN2_CTRL lanes 1, LNKCAP, LNKCTL2 target Gen2, buses 0/1/ff,
  MEM+MASTER) -> LTSSM on: Poll.Active at +43 us, **L0 at +127 us** (0x2c8 = 0x03000111). Link: **Gen1 x1**
  (DBI 0x80 = 0x30110000). iATU outbound 0 (DBI+0x300000) CFG0 bus 1 at 0x60ffe000-0x60ffefff, read the ID.
  Config space: status 0x0010 (cap list), BAR0 = 0x4 and BAR2 = 0x4 (64-bit memory, unassigned), cap
  pointer 0x48, INTA. No completion timeout (ELBI IRQ2 0x3800a008, bit 24 clear). Left the chip off after
  (LTSSM off, PERST# low, WL_REG_ON low). Left out on purpose: the IA sequencer (an L1.2-exit CDR workaround
  armed by the link-up IRQ), udbg dump setup, SysMMU, IRQs, L1SS.
  Also learned: the PHY's supplies (S2MPG13 LDO3S "L3S_PCIE1", LDO18S "L18S_PCIE1") are `regulator-always-on`
  (dtbo-0.dts); VSYS_PWR_WLAN_BT is only a power-meter rail. Nothing to switch on there.
- **wifiprobe5** (2026-10-25 15:51, `pixelboot/wifiprobe5/main.c`, log `downloads/probe/wifiprobe5.txt`):
  **BAR0 + BAR2 sized, assigned, all 18 AI EROM cores enumerated, and TCM/SYSMEM read/written across both BAR1 windows!**
  - `BAR0` size = `0x10000` (64 KB), `BAR2` size = `0x400000` (4 MB).
  - Root port memory window `DBI+0x20 = 0x60706000` (`0x60000000..0x607fffff`), iATU outbound region 1
    (`DBI+0x300200`) mapped `0x60000000..0x607fffff -> 0x60000000`, `BAR0 = 0x60000000`, `BAR2 = 0x60400000`,
    bus 1 command `0x04 = 0x6` (MEM + MASTER).
  - With `BAR0_WINDOW` (`PCI config 0x80`) set to `0x18000000`, reading `BAR0 + 0x00` returned
    **`0x10034389`**: chip ID **`0x4389`**, rev **3**, package **0**, chip type **1 (AXI)**; capabilities
    (`+0x04`) = `0x18400009`; EROM pointer (`+0xfc`) = **`0x18105000`**.
  - **Multi-backplane power domain (`dm1`)**: Pulled Google's `bcmdhd4389` source into `downloads/ref/bcmdhd4389/`.
    BCM4389 has a split backplane (`MULTIBP_ENAB`): before accessing `0x18020000` / `0x18120000` (WLAN/ARM domain `dm1`),
    we request `SRPWR_DMN1_ARMBPSD_MASK` (`1 << 1`) in `powerctl` at `BAR0 + 0x21e8` (PCIe2 `0x180021e8`) and
    `0x180001e8` (ChipCommon) (`|= 3 << 8`, polling bit `1 << 17` -> `0x00030300`), plus `DAR_PCIE_PWR_CTRL`
    (`BAR0 + 0x2a0c |= 2 << 8`), and enable coherent BAR access in `PCIE_CFG_SUBSYSTEM_CONTROL` (`PCI config 0x88 |= 0x300`).
  - **Complete AI EROM core table (`0x18105000`)**:
    - `core[00]`: `0x800` rev `0x46` (ChipCommon) `base=0x18000000 wrap=0x18100000 ioctrl=0x1 resetctrl=0x0`
    - `core[01]`: `0x83c` rev `0x48` (PCIe Gen2) `base=0x18001000 wrap=0x18101000 ioctrl=0x1 resetctrl=0x0`
    - `core[02]`: `0x847` rev `0x09` (**ARM Cortex-A7**) `base=0x18020000 wrap=0x18120000 ioctrl=0x0 resetctrl=0x1`
    - `core[03..05]`: `0x812` rev `0x56` (3x D11 802.11 main/aux/scan) `base=0x18021000..0x18023000 wrap=0x18121000..0x18123000`
    - `core[06]`: `0x851` rev `0x04` (HWA) `base=0x18024000 wrap=0x18124000`
    - `core[07]`: `0x849` rev `0x09` (**SYSMEM**) `base=0x18026000 wrap=0x18126000 ioctrl=0x0 resetctrl=0x1`
    - `core[09]`: `0x840` rev `0x14` (GCI) `base=0x18010000`
    - `core[0a]`: `0x827` rev `0x28` (PMU) `base=0x18012000`
  - **SYSMEM (`0x849`) & BAR2 (`tcm`)**:
    - Applied PCIe2 `configaddr=0x4e0` / `configdata` R/W WAR (`0x17`).
    - Reset `SYSMEM` (`wrap=0x18126000`); `coreinfo = 0x00000115` (`SRCI_SRNB_MASK | SRCI_SRNB_MASK_EXT = 0x1f0` ->
      `0x11` = 17 banks = `2,752,512` bytes = `0x2a0000` at `dongle_ram_base = 0x200000`, `ramend = 0x49ffff`).
      (Note: mainline `brcmfmac`'s `brcmf_chip_sysmem_ramsize` masks only `0xf0` and misses `SRCI_SRNB_MASK_EXT = 0x100`!)
    - With `PCI_BAR1_WIN (0x84) = 0x0`: `BAR2 + 0x000000` read `0xba3ef000`; `BAR2 + 0x200000` (`dongle_ram_base`)
      wrote `0x4861636b` and read back **`0x4861636b`**.
    - With `PCI_BAR1_WIN (0x84) = 0x400000`: `BAR2 + 0x09fffc` (backplane `0x49fffc`, top of RAM where `nvram_csm`
      and `pciedev_shared_t` address live) wrote `0x694f5331` and read back **`0x694f5331`**. Zero CTOs.

- **wifiprobe6** (2026-10-25 16:08, `pixelboot/wifiprobe6/main.c`, log `downloads/probe/wifiprobe6.txt`, first try):
  **FIRMWARE + NVRAM DOWNLOADED AND CORTEX-A7 BOOTED TO THREADX + MSGBUF READY IN 644 ms!**
  - Entered download state: halted `ARMCA7` (`0x18120000`, `SICF_CPUHALT = 0x20`), reset `SYSMEM` (`0x18126000`),
    cleared `0x49fffc`.
  - Downloaded `fw_bcmdhd.bin` (`0x1aeab2` = `1,764,018` bytes, `resetinstr = 0xbbbef214`) to `0x200000` via `BAR2`
    in `815.9 ms` (`first_word = 0xbbbef214`, `last_word = 0x6c697562`).
  - Formatted `bcmdhd.cal` (`27,237` bytes) via `process_nvram_vars` into `varsize = 0x5d2c` (`23,852` bytes), wrote to
    `varaddr = 0x49a2d0`, and wrote `varsizew = 0xe8b4174b` to `0x49fffc`.
  - Wrote 128-byte heap ASLR random seed rTLV (`sig = 0xfeedc0de`, `len = 0x80`) to `0x49a248`.
  - Wrote `resetinstr` (`0xbbbef214`) to TCM `0x0` and released `ARMCA7` halt (`ai_reset_core(0x18120000, 0, 0, 0)`).
  - **Dongle booted in `644.2 ms` (`0x9d46d` us)** and published **`shared_addr = 0x003cf594`** at `0x49fffc`!
  - `pciedev_shared_t` at `0x003cf594`:
    - `flags = 0x70050107` (version `7`, `2BYTE_INDICES`, `DMA_INDEX`, `HOSTRDY_SUPPORT`, `USE_MAILBOX`, etc.)
    - `trap_addr = 0x0`, `assert_line = 0x0` (zero traps or asserts)
    - `console_addr = 0x003cf614` (`log.buf = 0x0040a4f8`, `log.buf_size = 0x2000`, `log.idx = 0x577`)
    - `fwid = 0x68777be5`, `max_host_rxbufs = 0x05ff` (1535), `total_lfrag = 0x049f` (1183), `dma_rxoffset = 0x0`
    - `h2d_mb_data_ptr = 0x0040f5f4`, `d2h_mb_data_ptr = 0x0040f5f8`, `rings_info_ptr = 0x0040f2b8`, `flags2 = 0x40943041`
  - `ring_info_t` at `0x0040f2b8`:
    - `ringmem_ptr = 0x0040f304` (5x 16-byte `ring_mem_t` structs in TCM, populated by host in `dhd_prot_ring_init`)
    - `h2d_w_idx_ptr = 0x004145f8`, `h2d_r_idx_ptr = 0x004148ac` (`0x2b4` bytes = 346 x 2B H2D queues)
    - `d2h_w_idx_ptr = 0x00414b60`, `d2h_r_idx_ptr = 0x00414b70` (`0x10` bytes = 8 x 2B D2H rings)
    - (Note: `sh_addr_t` in `ring_info_t` is two `uint32`s with 4-byte alignment, so `max_tx_flowrings` is at `+0x34`, not `+0x38`.)
  - Firmware `CONSOLE:` log (`1,399` bytes):
    - `RTE (PCIE-MSGBUF) 20.101.91 (wlan=r1098710 c1 release2) on BCM4389 r3 @ 29.985/160.501/1002.823MHz`
    - `pciemsgbuf0: Broadcom PCIE MSGBUF driver`
    - Attached all 3 slices: `wl0` (5G), `wl1` (2G), `wl2` (2G+5G scan core)
    - `SPLITRX_MODE: 4 tcmsegsize 0 d11rxoffset 0`
    - `Firmware boot took 617099 us` / `ThreadX v5.6 initialized`

- **wifiprobe7b** (2026-10-25 16:58, `pixelboot/wifiprobe7b/main.c`, log `downloads/probe/wifiprobe7b.txt`, first clean run):
  **MSGBUF RINGS INITIALIZED, BIDIRECTIONAL PCIE BUS MASTER DMA WORKING, AND FIRST IOCTL RETURNED FACTORY MAC ADDRESS!**
  - Booted firmware in `645.3 ms` (`0x9d6a5` us), `shared_addr = 0x003b0ec8`, `rings_info_ptr = 0x004456e0`.
  - Configured `host_cap = 0x00083507` (API ver 7, valid phase, hostready, no OOB DW, ext trap, no trap on UR).
  - Populated host DMA scratch buffer (`0x801e1600`), trap buffer (`0x801e1640`), and DMA index buffers in `ring_info_t`.
  - Initialized all 5 common MsgBuf ring descriptors in TCM (`ringmem_ptr = 0x0040f304`). All TCM accesses strictly naturally aligned (16-bit halfwords via `tcm_write16`/`tcm_read16` for index arrays, 32-bit words for 4-aligned words; zero unaligned aborts).
  - Unmasked PCIE2 mailbox interrupt (`0xC34 = 0x300`), signaled `hostready` (`PCIH2D_DB1 = 0x12345678`).
  - Posted 8 `MSG_TYPE_IOCTLRESP_BUF_POST` buffers (req_id 1..8) and 8 `MSG_TYPE_EVENT_BUF_POST` buffers (req_id 101..108) on Ring 0.
  - Posted IOCTL 1: `WLC_GET_VAR` `"cur_etheraddr"` (cmd = 262, trans_id = 1, input_len = 14, output_len = 256) at Ring 0 slot 16 (`wr = 17`).
  - Rang H2D doorbell (`PCIH2D_MailBox = 0xdeadbeef`).
  - **The BCM4389 woke up and read all 17 items from host RAM via PCIe DMA**: `final TCM H2D RD index = 0x11` (17 items consumed).
  - **The BCM4389 DMA'd 3 completion items back to host RAM on Ring 2 (`final TCM D2H WR = 3`, `final Host DMA D2H WR = 3`) in 435 us**:
    - `item[0]`: `type = 0x24` (`MSG_TYPE_D2H_MAILBOX_DATA`), status 0.
    - `item[1]`: `type = 0x0a` (`MSG_TYPE_IOCTLPTR_REQ_ACK`), req_id `0xFFFE`, status 0.
    - `item[2]`: `type = 0x0c` (`MSG_TYPE_IOCTL_CMPLT`), req_id `0x08`, trans_id 1, status 0, `resp_len = 256`.
  - **Extracted factory Wi-Fi MAC address from IOCTL response buffer 8 in host RAM**: **`xx:xx:xx:xx:xx:xx`**!

- **wifiprobe8** (2026-10-25 17:36, `pixelboot/wifiprobe8/main.c`, log `downloads/probe/wifiprobe8-mac.txt`;
  `wifiprobe8.txt` now holds the 18:09 run below). (Claude's note: the MAC differs every run -- 26:73:... in 7b,
  de:91:... here -- and is locally administered: a random one the firmware makes, not the phone's. The host
  will have to set the real one, as Android's driver does.)
  **STEP-BY-STEP VERIFICATION COMPLETED: EVERY STEP AFTER `rings_info_ptr` LOGGED BEFORE AND AFTER, ZERO EXCEPTIONS OR HANGS, AND CONFIRMED FACTORY MAC ADDRESS!**
  - Booted firmware in `645.2 ms` (`0x9d840` us), `shared_addr = 0x003ced88`, `rings_info_ptr = 0x003cf034`.
  - Step 1: Zeroed host memory structures (`dma_scratch_buf`, `dma_trap_buf`, rings 0–4) - OK.
  - Step 2: Configured `pciedev_shared_t` (`host_cap = 0x00083507`, scratch `0x801e2600`, trap `0x801e2640`) - OK.
  - Step 3: Populated `ring_info_t` host DMA index pointers (`dma_h2d_wr`, `dma_d2h_rd`, etc.) - OK.
  - Step 4: Cleared TCM ring write/read indices with naturally aligned 16-bit stores - OK.
  - Step 5: Configured 5 common MsgBuf ring descriptors in TCM (`ringmem_ptr = 0x003cf080`) - OK.
  - Step 6: Unmasked PCIE2 interrupt (`0xC34 = 0x300`) - OK.
  - Step 7: Signaled `hostready` (`PCIH2D_DB1 = 0x12345678`) - OK.
  - Step 8: Posted 8 response buffers (`req_id` 1..8) and 8 event buffers (`req_id` 101..108) on Ring 0 - OK.
  - Step 9: Prepared and posted `WLC_GET_VAR "cur_etheraddr"` IOCTL at Ring 0 slot 16 - OK.
  - Step 10: Updated Ring 0 write index to 17 (`0x11`) in host RAM and TCM - OK.
  - Step 11: Rang H2D mailbox doorbell (`PCIH2D_MailBox = 0xdeadbeef`) - OK.
  - Step 12: Polled Ring 2 completions with timeout and watchdog pet. Dongle consumed all 17 items (`h2d_rd = 0x11`) and posted 3 completion items back to host RAM on Ring 2 (`d2h_wr = 3`) within 430 us - OK.
  - Step 13: Parsed completion items from Ring 2:
    - `item[0]`: `type = 0x24` (`MSG_TYPE_D2H_MAILBOX_DATA`), status 0.
    - `item[1]`: `type = 0x0a` (`MSG_TYPE_IOCTLPTR_REQ_ACK`), req_id `0xFFFE`, status 0.
    - `item[2]`: `type = 0x0c` (`MSG_TYPE_IOCTL_CMPLT`), req_id `0x08`, trans_id 1, status 0, `resp_len = 256`.
    - **Confirmed Factory MAC Address: `xx:xx:xx:xx:xx:xx`**!
  - Probe completed cleanly, letting the watchdog reset the phone back to Android (`boot reason watchdog,apc,early`).
- **wifiprobe8, second version** (2026-10-25 18:09, same source extended with steps 14-18, log `downloads/probe/wifiprobe8.txt`):
  CLM download OK (`clmload_status` 0), country `US` set and read back, `WLC_UP` 0. Then: `isup` -23 (BCME_UNSUPPORTED:
  no such iovar here, not a radio failure); `WLC_SCAN` -14 (BCME_BUFTOOSHORT), the other layout -23; every
  `WLC_SCAN_RESULTS` timed out (-100). The firmware console (dumped at the end) says why the dongle went quiet:
  at 1.002 s "err check: core 0x18104000, error 7 ... addr 0x00607fc0, AXI decode error, Multiple AXI Errors",
  AER with TLP header `40000001 00000003 60607fc0` (a 2-byte memory write **from the host** to BAR offset
  0x607fc0), then `TRAP 4` (NULL pointer, dfar 0) and "RO overwritten". RAM is 0x200000-0x4a0000: 0x607fc0 is
  past its end -- one of ring_info's TCM index pointers (h2d_r or d2h_r, never logged), which the probe cleared in
  step 4 and polled in step 12. The firmware uses host-memory (DMA) indices, and brcmfmac never touches the TCM
  ones then (pcie.c `brcmf_pcie_init_ringbuffers`). The firmware checks for errors once a second, so the crash
  shows ~1 s after boot whatever the probe was doing; the first wifiprobe8 run had probably finished by then.

## W3 — Wi-Fi in HackiOS (plan, 2026-10-30)
HackiOS already has everything above the radio, from the redwood port (WCN6750/ath11k): the station
boundary `bsd/sys/redwood_sta.h` (ops: power, scan, heard, join, leave, state, channels, address;
`redwood_sta_attach()`), `bsd/dev/redwood_sta.c` (one thread for those calls, the last scan kept),
`bsd/dev/redwood_apple80211.c` (wifid's Apple80211 requests: Settings), and the Ethernet interface
`bsd/sys/redwood_net.h` (`redwood_net_attach()` with transmit/poll, `redwood_net_input()`,
`redwood_net_link_state()`; configd runs DHCP when the link is up). The standing network to join is
`fw/network` of the small image (WSL `~/hackios/fw-wifi/network`: name, then passphrase, as join.c).
So W3 is a Broadcom *station side*: a link driver behind those two interfaces, made of the probes.
- Where things go: the PCIe link in **xnuboot** (it runs at EL2 first, like the probes, for the PMU
  isolation SMC; `XDEFINES=WIFI_PCIE=1` in ios-run.sh, off by default), in `pixelboot/xnuboot/wifi.c`.
  The driver in the kernel: `wifi/redwood_bcm4389.c` here, copied into WSL `~/hackios/xnu/files/bsd/dev`
  and the applied tree `xnu-src/xnu-8796.121.2` (listed in its `bsd/conf/files`; init called from
  `redwood_input.c` next to `redwood_wifi_init()`; boot-arg `bcmwifi=N`). Firmware in WSL
  `~/hackios/fw-wifi/` (fw_bcmdhd.bin, bcmdhd.cal, bcmdhd_clm.blob; userland/build.sh to copy them into
  `fw/`), read with `redwood_image_file("fw", ...)`.
- Kernel memory for DMA: one 16 KB page per ring/buffer group (`kmem_alloc` + `kvtophys`, so each is
  physically contiguous), and the device is not coherent: clean before the dongle reads, invalidate
  before we read (`flush_dcache`), as redwood_mali.c does. No interrupts at first: a kernel thread
  polls (the probes' drain loop), or redwood_net's `poll`.
- **W3a** xnuboot brings PCIe1 to L0 and assigns the BARs; the kernel (bcmwifi=1) maps config space,
  BAR0, BAR2 and reads 14e4:4441 and the chip id over the backplane. iOS boots beside it.
- **W3b** bcmwifi=2: firmware + NVRAM download, the shared area, rings, mailbox/deep sleep, ioctls
  (MAC, CLM, country, up), a polling thread. Log: firmware up, MAC.
- **W3c** bcmwifi=3: the station ops (escan -> heard, the Android-style join, lx_wpa, keys, leave,
  state, channels) and the redwood_net link (flow ring TX, RX ring input), `redwood_sta_attach()`;
  the standing join of fw/network; configd's DHCP; ping / Safari.
- **W3d** Settings: the list, joining a network typed in on the phone (redwood_apple80211 exists).

## Status
- 2026-10-31 14:05: **W3c: Settings scans, lists and JOINS <home network> (WPA2); DHCP and DNS work; TX stalls.**
  Runs w3c..w3c8 (`downloads/probe/w3c*.txt`). wifi/redwood_bcm4389.c now has step 3 (station ops,
  en0 link, lx_wpa.c built in under bcm4389_* names, a status line every 30 s). Found and fixed on the way:
  - Settings froze / wifid sent nothing: **configd was not running** -- userland/mkjobs.py's RWA_PACKED16
    loop reused `name`, so every REST job went into one file "RWA_PACKED16" (since ~Oct 22, all images).
    Fixed (loop variable renamed); the wifi image now has 408 jobs. GPU images get it on their next build.
  - The firmware trapped at boot sometimes: the **first BAR write after a window move went through the old
    window** (NVRAM's first word to 0x9a298 = the "AXI slave error" of every boot; a ring descriptor to
    0x7b0144 -> AXI decode error -> trap). window_move(): dsb, write, read back until equal, dsb.
  - wifid disassociated ~20 s after each join: COUNTRY_CODE (51) set was refused -> now accepted, get
    answers "US" (wifi/ios-side/redwood_apple80211.c, installed by install-kernel.sh with the driver;
    also redwood_apple80211_counts() for the status line, and its per-request lines capped at 150).
  - Rejoin got BCMPCIE_RING_IN_USE (4): a new flow ring id per join (2-9, 4 KB each). Flow ring delete
    never completes ("Flowring N is taking too long to delete" -> trap, w3c5); flags2 0x40943041 has no
    FAST_DELETE_RING.
  - Doorbell now as bcmdhd rings it: 0xff000000 | ringid << 16 | wr (flow ring id + 3). Changed nothing visible.
  - **Open: TX.** The flow ring's DMA read index stays at 2 and TX statuses come only for EAPOL (and frame 9),
    yet frames do go out (DHCP 10.0.0.183, DNS answered in 3.5 s, w3c7). With room counted by that index
    TX stopped at 50 frames and every fetch timed out. w3c8 (stopped by the user before its log) adds:
    slots free after 100 ms too, and the status line prints the ring's indices from DMA and from the chip's
    RAM (ring_info +4/+8). Also unexplained: ~15 MB arrived in w3c6/w3c7 while nothing went out (w3c8
    prints the first big frames' kind). Android (android-connect.txt) uses flow ids 37 (unicast, prio 3,
    512 items) and 41 (multicast 33:33.., prio 0, 2048 items), deletes complete at once.
  - Unattended runs: `JOIN=1 wifi/build-image.sh` builds downloads/ios/root-gl-wifi-join.apfs with
    fw/network from downloads/wifi/join.c (lengths printed only) and RC_JOBS=wifi/rc.jobs.bcmwifi
    (waitaddr, resolve, fetches); boot-arg `bcmjoin=1` joins it. THE IMAGE, pixelboot/out/payload/ios.bin,
    xnuboot/Image, xnuboot.elf and the two xnuboot*-boot.img HOLD THE PASSPHRASE: delete after each run.
  - Also open: 0 / 1 bar while wifid sees -36 dBm (link quality event?); typing stutters (not the logging;
    maybe the 400 REST daemons now back).
- 2026-10-31 00:00: **W3b works: the firmware is up inside HackiOS, beside iOS** (`downloads/probe/w3b2.txt`).
  bcmwifi=2: ARM up after 0.63 s, shared flags 0x70050107, cur_etheraddr xx:xx:xx:xx:xx:xx, CLM 0,
  country US 0, WLC_UP 0; the service thread: "30 s on: 1 deep-sleep requests answered, 1 exits, 2
  events", no trap. iOS booted to the home screen with the system image staged over USB (the usbdev.c
  MS OS descriptor fix works; the user had to remove the Windows record once more, broken by the last
  run of the old xnuboot). DMA: xnuboot's `bcmdma=` block at 0xff000000 (wifi_dma_block in
  xnuboot/main.c), mapped uncached. All mappings are now `ml_io_map_unmappable` (kernel_map): plain
  ml_io_map's io_submap ran out (a 512 KB BAR2 slice panicked after the GPU's mappings and the DMA
  block, w3b). Image: `downloads/ios/root-gl-wifi.apfs` (`wifi/build-image.sh`: the GPU image + fw/
  fw_bcmdhd.bin, bcmdhd.cal, bcmdhd_clm.blob). Run: as W3a with `RAMDISK=downloads/ios/root-gl-wifi.apfs`
  and `bcmwifi=2`. To look at: the firmware console printed "AXI slave error" / "Multiple AXI Errors"
  twice early on, and "device Uncorrectible error 0x8000" with a TLP header (0x6049a298: an address in
  BAR2's range) right at boot. Settings' Wi-Fi does nothing yet: that is W3c (station ops,
  redwood_sta_attach, redwood_net link). **Next: W3c.**
- 2026-10-30 23:55: **W3b prepared, not written yet** (paused at the user's 5-hour limit). Findings:
  - Firmware into the image: WSL `~/hackios/userland/root.apfs` is byte-identical to
    `downloads/ios/root-gl.apfs` (md5 afe0678cc79e, 2026-10-25 12:35), so the GPU image is reproduced by
    `REST=1 GL=1 ./build.sh root.apfs` in `~/hackios/userland` (Mesa is not rebuilt while
    `$M/built-from-air` matches `tools/mesa/air/*`). build.sh's fw-wifi block (line ~560) only takes
    redwood's `wpss.mdt`: add one for `../fw-wifi/fw_bcmdhd.bin bcmdhd.cal bcmdhd_clm.blob` (copy those
    from `downloads/wifi/fw/` to `~/hackios/fw-wifi/`), rebuild, save as `downloads/ios/root-gl-wifi.apfs`,
    run with `RAMDISK=` that. The kernel reads them with `redwood_image_file("fw", ...)` (it found the
    Mali's on /dev/md0).
  - Kernel W3b design: DMA memory one 16 KB page per group (kmem_alloc KMA_KOBJECT|KMA_ZERO|KMA_DATA +
    kvtophys; flush_dcache each page once after allocation): page 1 scratch @0, h2d_wr @1K, h2d_rd @2K,
    d2h_wr @3K, d2h_rd @3.5K, trap @4K; page 2 ring0 (256x40); page 3 rings 1-4 (2048+6144+1536+2560,
    H2D and D2H never sharing a 64-byte line); page 4 flow ring + TX buffer + ioctl request; 4 pages each
    for ioctl responses (8x8 KB), events (8x8 KB), RX (32x2 KB). Clean (flush_dcache) every item,
    payload and index we write before the doorbell; clean+invalidate every D2H index, item and buffer
    before reading it. A kernel thread (kernel_thread_start; IOSleep(10)) drains rings 2-4 after init,
    for the mailbox (deep sleep: DS_ACK + device wake, as wifiprobe11+) and events. Then the probe's steps:
    DMN1 power, config 0x88/0x4c, PCIE2 0x120/0x124, halt the CA7, firmware to TCM 0x200000, NVRAM to
    the top of RAM, the shared slot, release, shared area, host_cap (INBAND_DW|NO_OOB_DW...), ring_info,
    rings, hostready, buffers, cur_etheraddr, CLM, country, WLC_UP. Log goal: "bcm4389: 2: firmware up,
    MAC ..., WLC_UP 0", and no trap after 30 s.
- 2026-10-30 23:40: **W3a works** (`downloads/probe/w3a2.txt`). xnuboot with `XDEFINES=WIFI_PCIE=1`
  (`pixelboot/xnuboot/wifi.c`): isolation off at EL2, PLL/CDR/OC, L0 after ~110 us, 444114e4, BARs, then
  `bcmlink=1` in the boot-args. The kernel (`wifi/redwood_bcm4389.c`, installed and built by
  `wifi/install-kernel.sh` in WSL; kernel `downloads/kernel/mach.development.bcm2837.wifi`), boot-arg
  `bcmwifi=1`: "PCI id 14e4:4441 ... chip id 0x10034389: BCM4389 rev 3". Run:
  `KERNEL=downloads/kernel/mach.development.bcm2837.wifi XDEFINES="WIFI_PCIE=1" XARGS="mali=3 gl=1
  glwait=1 glquiet=1 decon=3 fb_threads=6 fb_beat=0 fb_inuse=1 bcmwifi=1" bash pixelboot/tools/ios-run.sh NAME 150`.
  Found on the way: ml_io_map of BAR2's 4 MB at once panics (KMA_NOFAIL, KERN_NO_SPACE): BAR2 is mapped
  in 512 KB slices when first touched (tcm_word). **USB**: Windows' record of 18d1:4ee0 (bootloader
  fastboot, which xnuboot's USB device copies) lost its WinUSB binding and Android interface GUID, so
  neither fastboot nor xnuboot's staging was seen ("our USB device did not show up"; the run then booted
  without the system image: "exec of /sbin/launchd failed"). The user removed the record
  (`pnputil /remove-device "USB\VID_18D1&PID_4EE0\<serial>"`); the cause, xnuboot's device not
  answering the MS OS 1.0 descriptor requests Windows sends it (vendor code 0x47, cached in
  usbflags\18D14EE00100), is fixed in `pixelboot/common/usbdev.c` (string 0xee, compat ID WINUSB,
  DeviceInterfaceGUIDs). Not yet run with the fix. **Next: W3b** (firmware up in the kernel).
- 2026-10-30 23:00: **wifiprobe13: DHCP over the encrypted link: GOT AN ADDRESS, 10.0.0.22**
  (`downloads/probe/wifiprobe13.txt`). `pixelboot/wifiprobe13/` = wifiprobe12 + a DHCP client: DISCOVER and
  REQUEST built by hand (IPv4 0.0.0.0 -> 255.255.255.255, UDP 68 -> 67, BOOTP flags 0, options 53, 61, 50,
  54, 55), sent as broadcasts on the flow ring; replies matched by xid on the RX ring. OFFER and ACK from
  10.0.0.1: address 10.0.0.22, mask 255.255.255.0, router 10.0.0.1, DNS the ISP's server, lease 172800 s (the
  same lease Android has: same MAC). Then 3 ARP frames from the router came in, decrypted, during the 10 s
  hold; the link stayed up. **W2 is done: the chip is up, scans, joins WPA2, keys, and carries data both
  ways.** Loose ends: no TX status comes back for the broadcast DHCP frames (EAPOL's do), and the
  DISCOVER was sent twice (the first OFFER was missed by the retry check); the station nonce comes from
  the counter (fill_nonce), fine for a probe only. Build/run as wifiprobe12 with `wifiprobe13` in the
  paths; delete `pixelboot/out/wifiprobe13*` after.
  **Next: W3**, the driver inside HackiOS: this MsgBuf/PCIe code (rings, mailbox/deep sleep, events, the
  Android-style join, RX/TX, lx_wpa) moved into the kernel as a network interface, and iOS's Wi-Fi stack
  (wifid, Settings) on top (`hackios/wifi/lx/` is the starting point).
- 2026-10-30 22:55: **wifiprobe12: the 4-way handshake completes, KEYED** (`downloads/probe/wifiprobe12.txt`).
  `pixelboot/wifiprobe12/` = wifiprobe11 + a TX flow ring (id 2, MSG_TYPE_FLOW_RING_CREATE on ring 0 as
  bcmdhd's tx_flowring_create_request_t, 64 x 48-byte MSG_TYPE_TX_POST items, TX status on ring 3) +
  hackios/wifi/lx/src/lx_wpa.c compiled in as it is (`wpa.c` with memcpy/memset/memcmp/strlen,
  `include/string.h`; build.sh now adds a program's own include/). The PMK is made on the PC:
  `python -I pixelboot/tools/wpa_pmk.py downloads/wifi/join.c pixelboot/out/wifiprobe12/pmk.bin` before
  the build (on the phone 8192 HMAC rounds on Device memory would outlast the watchdog). Run: M1 -> M2
  (121 bytes, TX status 0), M3 (key info 0x13ca, MIC good) -> M4 (TX status 0), then "wsec_key" pairwise
  (index 0, the AP's address) and group (index 1, its RSC), wsec 4: keyed 72 ms after the flow ring was
  up; the link stayed up through a 10 s hold (no DEAUTH reason 15). No other frame came in those 10 s:
  a quiet network, or the firmware filters broadcast/multicast. **Next: wifiprobe13** = DHCP over the
  encrypted link (DISCOVER/OFFER/REQUEST/ACK, unicast replies) to prove both directions.
- 2026-10-30 22:43: **wifiprobe11p: ASSOCIATED, and the AP's EAPOL message 1 arrives** (`downloads/probe/wifiprobe11p.txt`).
  The fix came from Android's own connect, recorded on the phone (`/sys/wifi/wl_dbg_level` "DBG:1 SCAN:1"
  for one Wi-Fi off/on with `dmesg -w` to a file, then restored to "ERR:1 INFO:1 DBG:0 SCAN:0 ...
  P2PACTION:1"; the log, keys filtered, is `downloads/probe/android-connect-short.txt`). bcmdhd on this
  phone does **not** use the explicit security path: wpa_auth 0xc0, auth 0, no wsec (left 0), mfp 1,
  `join_pref` with WPA tuples (WL_JOIN_PREF_WPA 2, length, 0, count, then AKM / unicast / multicast suites
  as in an RSN IE: 00-0f-ac-02, 00-0f-ac-04, 00-0f-ac-04), **no wpaie** (auto_wpa: the firmware builds
  the RSN IE), then `join` v1 by SSID with broadcast BSSID and one 20 MHz chanspec (0xd09d). With that:
  request with the SSID IE and the firmware's RSN IE (caps 0x000c), AUTH 0, ASSOC_RESP 0, LINK up,
  WLC_GET_SSID "<home network>"; then EAPOL-Key message 1 three times (key info 0x008a, replay 2, 3, 4) on the
  RX ring, and DEAUTH_IND reason 15 (4-way handshake timeout) since nothing answers yet. No traps.
  The scan errors (EL 30) still show during the join scan; they don't matter.
  **Next: wifiprobe12** = a TX flow ring (MSG_TYPE_FLOW_RING_CREATE for the AP, TX posts) + lx_wpa.c:
  answer M1 with M2 (our RSN IE = the one in the request: 30 14 ... 0c 00), check M3's MIC, unwrap the
  GTK, send M4, then the keys with WLC_SET_KEY. Then DHCP.
- 2026-10-30 21:42: **wifiprobe11 (runs 11 to 11k): the dongle is stable, the AP refuses the association**
  (`pixelboot/wifiprobe11/main.c`, logs `downloads/probe/wifiprobe11*.txt`). Solved on the way:
  1. **Deep sleep**, as the Pixel's own bcmdhd does it (`downloads/ref/bcmdhd4389`, built with PCIE_INB_DW):
     host_cap HOSTCAP_DS_INBAND_DW | HOSTCAP_DS_NO_OOB_DW; mailbox data both ways as control ring messages
     (no PCIE_SHARED_USE_MAILBOX in the flags): MSG_TYPE_D2H_MAILBOX_DATA 0x24 in (data at +12),
     MSG_TYPE_H2D_MAILBOX_DATA 0x23 out (data at +8). The DS_ENTER_REQ (0x2) at boot gets DS_ACK (0x2) then
     H2DMB_DS_DEVICE_WAKE (0x40); DS_EXIT_NOTE (0x4) comes back; never deasserted. No traps in 40 s runs.
     (Ring DS_ACK with only NO_OOB_DW: trap at the next doorbell, 11b. brcmfmac's TCM + SB mailbox: ignored.)
  2. **H2D epochs start at 1** (bcmdhd: H2D_EPOCH_INIT_VAL % 253); an RX post with epoch 0 was "h2d livelock
     error". The phase bit flips on each ring wrap.
  3. **Event buffers 8 KB** (brcmfmac's MAX_CTL_PKT_SIZE): 2 KB ones overflowed into our globals (scan
     results). Extended trap buffer 4 KB. ring_info: 40 TX flowrings, 43 submission, 4 completion rings.
  4. RX: 32 buffers on ring 1 accepted (no frames yet: never associated).
  The join, as bcmdhd's wl_cfg80211_connect: wpa_auth 0x80, auth 0, wsec 4, `mfp` 0 (found at 1), `wpaie`
  our RSN IE (same as wpa_supplicant's for this AP), then the `join` iovar, wl_extjoin_params_v1 (wlc_ver
  14.4; the firmware reads v1 whatever: v0 is BCME_BADSSIDLEN). **AUTH succeeds, then the AP answers the
  association with status 1 (unspecified)** every time: to the scanned BSSID + chanspec, by SSID only, and
  with Android's own MAC for this network (`DEFINES=JOIN_MAC=0x...`, from `adb shell cmd wifi status`).
  WLC_SET_SSID instead: NO_NETWORKS. `assoc_info` / `assoc_req_ies` (11j, 11k): the request is 226 bytes
  and its IEs begin with **26 zero bytes where the SSID IE belongs**; hostapd answers status 1 to a missing
  or wrong SSID. Unknown whether the zeros are really on the air or only in the iovar's copy.
  **Android's request, for comparison (22:11)**: `adb shell cmd wifi set-verbose-logging enabled`, Wi-Fi
  off/on, then logcat: `req_ies - hexdump(len=237): 00 08 xx xx xx xx xx xx xx xx 01 08 8c 12 98 24 ...`:
  the SSID IE ("<home network>"), then the same rates/power/channels IEs as ours (logcat cuts it at 32 bytes).
  bcmdhd copies `assoc_req_ies` as it is (wl_get_assoc_ies), so **our firmware really builds the request
  without the SSID**: our state differs from Android's. Verbose logging was off before and is off again.
  11l: without WLC_SET_INFRA (bcmdhd never sends it on connect) nothing changed.
  Both symptoms fit a firmware whose SSID is empty: no SSID IE on the targeted join, NO_NETWORKS with
  WLC_SET_SSID, and the SSID-only join (11k) took 13 s and picked <home network> as if by wildcard.
  11m: an open join (wsec 0, wpa_auth 0, empty wpaie) is pruned by the firmware itself (event 23 PRUNE,
  reason 1 = encryption mismatch): it never reaches the AP. WLC_GET_SSID: length 0 after every join.
  11n: WLC_GET_SSID stays empty right after WLC_SET_SSID, 1 s later and after the join (NO_NETWORKS).
  **`EL: 30 8fd4` = EVENT_LOG_TAG_SCAN_ERROR** (include/event_log_tag.h): 3-5 per join attempt, every
  join path; our escan never logs one. So the firmware's internal join scan fails, and the targeted join
  goes on from the escan's cached BSS without the SSID. (`8fd4` can't be decoded: no logstrs.bin on the
  phone; /vendor/firmware/fw_bcmdhd.map, copied to downloads/wifi/fw, names functions but no strings.)
  11o: the join's chanspec as bcmdhd gives it, 20 MHz on the control channel (0xd09d, not the scan
  record's 80 MHz 0xe29b): no change. Android's preinit (optimised path: the firmware has
  `preinit_status`) sets nothing SSID- or scan-related that we lack: rsdb_mode only with a custom antenna.
  **Next**: Android's exact ioctl/iovar sequence on connect, from bcmdhd's own debug logging (module
  parameter `dhd_msg_level` / `wl_dbg_level` under /sys/module/bcmdhd*, reset by a reboot; ask first),
  diffed against ours. Then candidates: the scan time ioctls the legacy preinit sets
  (WLC_SET_SCAN_CHANNEL_TIME / UNASSOC_TIME / PASSIVE_TIME), roam_off 1, PM. Build: `DEFINES="JOIN_MAC=0x..." bash pixelboot/build.sh wifiprobe11`, run as usual, and
  delete `pixelboot/out/wifiprobe11*` afterwards (it holds the passphrase).
- 2026-10-30 20:30: **wifiprobe10 ran: not joined, two findings** (`downloads/probe/wifiprobe10.txt`; the
  user's file is `downloads/wifi/join.c`, built with `DEFINES=JOIN_CONF=C:/.../downloads/wifi/join.c`).
  1. **This firmware has no supplicant**: `sup_wpa` -23 (BCME_UNSUPPORTED), `WLC_SET_WSEC_PMK` -24
     (BCME_BADLEN). infra, wpa_auth 0x80, auth 0, wsec 4 all set and read back fine. Android does the 4-way
     handshake on the host, so we must too: EAPOL over the data path (RX buffers on ring 1 / completions on
     ring 4, a TX flow ring), and `wpaie` with our RSN IE before the join, as brcmfmac does with wpa_supplicant.
     The crypto exists: `hackios/wifi/lx/src/lx_wpa.c` (SHA-1, PBKDF2, PRF, AES unwrap, the 4 messages).
  2. **The dongle trapped at 18 s: `pciedev_dsack_taking_too_long: DS ACK not received`**. With
     HOSTCAP_DS_NO_OOB_DW the firmware asks for deep sleep through the D2H mailbox (shared+44: 0x1
     DS_ENTER_REQ) and needs H2D_HOST_DS_ACK (0x2) at shared+40 plus a mailbox doorbell (brcmfmac
     `brcmf_pcie_handle_mb_data` / `brcmf_pcie_send_mb_data`). Every ioctl after it timed out (-100).
     Probes 8-9 ended before the first request. Any longer probe needs this.
  During the 13 s before the trap: no events at all after WLC_SET_SSID (err 0); the console shows bursts of
  `EL: 30 8fd4` every 5 s (join retries, probably: no RSN IE and no supplicant). <home network> was seen (ch 155, -34).
  Next: **wifiprobe11** = DS ACK + RX path + `wpaie` + join: SET_SSID 0, LINK up, and the AP's EAPOL M1
  (ethertype 0x888e) in the RX ring. Then **wifiprobe12** = a TX flow ring + lx_wpa.c: M2..M4, keys via
  WLC_SET_KEY, a keyed link.
- 2026-10-30: **wifiprobe10 written (the join)**: it needs `downloads/wifi/join.conf`, written by the
  user (line 1 the SSID, line 2 the WPA2 passphrase; CRLF and a BOM are fine). `pixelboot/wifiprobe10/main.c` =
  wifiprobe9 (the scan marks join.conf's SSID with `<- join.conf`) plus brcmfmac's FWSUP_PSK connect: infra 1,
  `wpa_auth` 0x80, `auth` 0, `wsec` 4, `sup_wpa` 1 (each read back), WLC_SET_WSEC_PMK with the passphrase as it
  is (the firmware derives the PMK), WLC_SET_SSID with the SSID only; up to 20 s for LINK up + PSK_SUP status 6,
  then BSSID / chanspec / RSSI, 10 s held, WLC_DISASSOC. Every join event printed with status, reason, flags.
  The passphrase is never printed, but it is in the boot image: delete join.conf and the image when done.
  Build and run: `bash pixelboot/build.sh wifiprobe10 && bash pixelboot/tools/run.sh pixelboot/out/wifiprobe10-boot.img wifiprobe10`.
  What to look for: `JOINED`, `keyed=...1`, `links lost=0`. If PSK_SUP events come but never status 6: the
  passphrase (or a WPA3-only network). If SET_SSID fails: the SSID, or the router wants PMF (`mfp`).
  Test build with a dummy file (`DEFINES=JOIN_CONF=<path>`) compiled clean.
- 2026-10-30 20:15: **wifiprobe9 ran: the scan works** (`downloads/probe/wifiprobe9.txt`). Shared flags 0x70050107
  (DMA indices, 4-byte), every ioctl answered (MAC, CLM load + status 0, country US, WLC_UP 0, mpc 0,
  event_msgs, escan 0), then 19 BSS on 2.4 and 5 GHz (channels 1-11, 50, 106, 155; RSSI -30 to -93) and
  escan's last status 0. Firmware console clean: no AXI decode error, no trap; the dongle was still up when
  the probe ended at 6.3 s. W2's goal (firmware up, networks in the log) is met by the bare-metal probe.
  Not yet exercised: joining a network (WPA2), the data path (flow rings, RX buffers, DHCP), and all of it
  inside XNU (W3). The MAC the chip reports, xx:xx:xx:xx:xx:xx, has the locally administered bit set.
  The log holds neighbours' SSIDs: keep it out of anything published.
- 2026-10-29: **wifiprobe9 written and built** (the phone wasn't at hand). `pixelboot/wifiprobe9/main.c`
  = wifiprobe8 plus: ring indices in host memory only when the shared flags say DMA indices (flags, both
  read-index pointers and rx_dataoffset are logged), no 16-bit TCM access outside RAM at all; the firmware console
  printed after each step; no `isup`; then `mpc` 0, the ESCAN_RESULT event (69) set in `event_msgs`, an active
  `escan` (version 2 params, any SSID, every channel; layouts from brcmfmac's fwil_types.h / cfg80211.c), and up
  to 15 s of events: every BSS printed once (BSSID, channel, RSSI, SSID) until the final ESCAN_RESULT.
  Run: `bash pixelboot/tools/run.sh pixelboot/out/wifiprobe9-boot.img wifiprobe9` (log `downloads/probe/wifiprobe9.txt`).
  What to look for: no "AXI decode error" in the console; `wifiprobe9: BSS ...` lines; `escan's last status` 0.
  If the ioctls stop working, check `shared flags` (bit 16 = DMA indices) first.
- 2026-10-25 17:36: **W1 DONE, W2 MsgBuf + Bidirectional PCIe DMA + IOCTL VERIFIED (`wifiprobe8`)**.
  Discrete step-by-step verification confirmed all MsgBuf ring structures, doorbells, and IOCTL processing with zero hangs or aborts.
  Now verified and ready for:
  1. Download `bcmdhd_clm.blob` (`downloads/wifi/fw/bcmdhd_clm.blob`, 18,348 bytes) via `clmload` iovar chunks.
  2. Issue `WLC_UP` to power up the radio PHYs.
  3. Run Wi-Fi scan (`WLC_SCAN` / `escan` / `WLC_SCAN_RESULTS`) to dump visible SSID network names and RSSIs to the probe log.
  4. Port this proven PCIe + MsgBuf ring driver into HackiOS's `hackios/wifi/lx/` for iOS 16.5 (Phase W3).
- 2026-10-25: **the controller and PHY are awake and ours** (wifiprobe3d). Next: **probe 4, the link** --
  in at_entry (EL2) the isolation SMC; then WL_REG_ON (gph2[4]) high and the chip's power-on delay (bcmdhd:
  ~200 ms), PERST# (gph2[0]) an output, low; phy_all_pwrdn_clear; establish_link as above (PHY config from
  pcie-exynos-gs201-rc-cal.c l. 154-392, num_lanes 1; checks; soft resets; PERST# high + 20 ms; LTSSM on;
  poll 0x2c8 & 0x3f for L0 = 0x11); then an iATU outbound region (DBI + 0x300000, unrolled) for CFG0 to
  bus 1 at the config window 0x60ffe000, and read **14e4:4441**. Still to look up: pinctrl 0x16b/0x16c (CLKREQ).
- Earlier: **Probe 1 written and built**: `pixelboot/wifiprobe/main.c`
  -- reads only: PMU isolation, gph2 (PERST/WL_REG_ON function and level), HSI2 sysreg, PCIe1 ELBI, PCS,
  PHY (+ lock/OC status words), DBI (root port's config: 144d:eced if clocked), safest first.
  Run: `bash pixelboot/build.sh wifiprobe && bash pixelboot/tools/run.sh pixelboot/out/wifiprobe-boot.img wifiprobe1`
  (log: downloads/probe/wifiprobe1.txt; ~3 min, no iOS). Then probe 2: WL_REG_ON, isolation, PHY config,
  PERST, LTSSM -> L0, read the chip's 14e4:4441.
