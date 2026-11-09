# Licenses and notices

This project's own code is under the **MIT License** ([LICENSE](LICENSE)). Some files come from, or are
adapted from, other projects and keep those projects' licenses. They are listed below.

## From HackiOS (oosexgang/hakios-xnu-kernel, formerly hackios-xnu-kernel / hackios-xnu-userland)

HackiOS licenses its XNU patch, and the files derived from it, under the **Apple Public Source License 2.0**
([LICENSES/APSL-2.0.txt](LICENSES/APSL-2.0.txt)), and the rest of its code under MIT.

**APSL-2.0.** These are HackiOS's kernel-tree files with this port's changes, or files adapted from HackiOS
while its whole kernel repository was APSL-2.0 (October 2026):

| File | Origin |
|---|---|
| `hackios-overlay/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | HackiOS's display driver, changed for the Pixel's DECON (recovered, see below) |
| `hackios-overlay/xnu/files/iokit/Kernel/MiniIOSurface.cpp`, `MiniIOSurface.h` | HackiOS's IOSurface, changed (recovered) |
| `wifi/ios-side/redwood_apple80211.c`, `redwood_sta.h` | HackiOS's Wi-Fi glue (`bsd/dev`, `bsd/sys`), changed |
| `pixelboot/xnuboot/main.c` | adapted from HackiOS's `avf/shim/avfboot.c` |
| `pixelboot/xnuboot/identity.h`, `fdtx.c`, `fdtx.h` | from HackiOS's loader (`xnuloader/`) |
| `pixelboot/wifiprobe12/wpa.c`, `pixelboot/wifiprobe13/wpa.c` | HackiOS's `wifi/lx/src/lx_wpa.c` |

**MIT, copyright HackiOS's authors.** These are HackiOS userland files with this port's changes:

| File | Origin |
|---|---|
| `hackios-overlay/userland/src/eagl.c`, `inject.c`, `inject_sb.c`, `hackios-overlay/userland/build.sh` | HackiOS's userland, changed (recovered) |
| `gpu/mesa/redwood_drm.c`, `gpu/mesa/xf86drm.h` | HackiOS's `tools/mesa/redwood-drm`, changed for panthor |

## Mesa

`gpu/mesa/panfrost-darwin.patch` is a patch to Mesa 25.0.7 (MIT).

## Written with reference to

The drivers were written from these projects' sources and documentation. Register layouts, message
formats and bring-up sequences follow them, but no files were copied:

- Linux `drivers/gpu/drm/panthor` and `include/uapi/drm/panthor_drm.h` (Mali CSF GPUs)
- Google's `bcmdhd` for the BCM4389 and Linux's `brcmfmac` (Broadcom FullMAC, MsgBuf over PCIe)
- Google's gs201 kernel sources: PCIe (`pcie-exynos-*`), ACPM (`acpm_ipc.c`), and the phone's device tree
- Linux `drivers/firmware/samsung/exynos-acpm.c`

## Not in this repository

- **Nothing from Apple.** iOS 16.5 is downloaded from Apple and extracted on your own machine by HackiOS's
  scripts.
- **No vendor firmware.** The Mali GPU firmware (`mali_csffw-r54p3.bin`) and the Broadcom Wi-Fi firmware,
  NVRAM and CLM blob are copied from your own phone's `/vendor/firmware`.
- **No logs, kernels or images.** Boot logs contain nearby Wi-Fi networks, and images contain Apple's
  code or your Wi-Fi passphrase.

iOS, iPhone and Apple are trademarks of Apple Inc. Pixel and Android are trademarks of Google LLC. This
project is not affiliated with or endorsed by Apple or Google.
