# HackiOS on the Pixel 7 Pro

A port of [HackiOS](https://github.com/oosexgang/hakios-xnu-kernel) to the **Google Pixel 7 Pro** (cheetah,
Tensor G2 / gs201). HackiOS runs Apple's open-source XNU kernel (the kernel of iOS 16.5) on Android hardware,
with the unmodified iOS 16.5 user space on top: launchd, backboardd, SpringBoard and the apps.

On the Pixel it boots straight from the bootloader with `fastboot boot`, and **nothing is flashed**. A small
bare-metal loader of our own (`pixelboot/xnuboot`) takes the place of the UEFI that HackiOS uses on its
Snapdragon phone. After a reboot the phone is back in Android.

> **Research project. No Apple files and no vendor firmware are in this repository.** iOS is downloaded
> from Apple and extracted on your machine by HackiOS's scripts, and the GPU and Wi-Fi firmware are copied
> from your own phone. Not affiliated with or endorsed by Apple or Google.

## Status (2026-10-31)

| Works | Does not work (yet) |
|---|---|
| Boot to the iOS home screen in about a minute, nothing flashed | The lock screen (never shows on this port) |
| Touchscreen (Synaptics S3908 over SPI) | Wi-Fi **sending** stalls after about 50 frames (receiving works) |
| CPU clocks with a governor (throttles from 2.4 to 1.1 GHz at 80 °C) | Wi-Fi signal bars (0–1 bars at -36 dBm) |
| **GPU acceleration**: CoreAnimation draws on the Mali-G710 through Mesa's panfrost (zero copy) | Rejoining a network (the old send queue is never freed) |
| Tear-free display: DECON0 triggered on the panel's TE pulse, two framebuffers flipped | Audio, camera, haptics, Bluetooth, cellular |
| Home-screen swipes at 60 fps, busy screens up to about 57 fps (GPU scaling into the framebuffer) | Charging under iOS |
| **Wi-Fi** (Broadcom BCM4389 on PCIe): Settings scans, lists and joins WPA2 networks; DHCP and DNS work | |
| Dark mode, the home screen without widgets, a custom wallpaper | |

The detailed engineering logs, with every finding, dead end and boot-arg, are in
[docs/GPU-PLAN.md](docs/GPU-PLAN.md) and [docs/WIFI-PLAN.md](docs/WIFI-PLAN.md).

## What is here

| Path | What |
|---|---|
| [`pixelboot/`](pixelboot/) | Bare-metal programs booted with `fastboot boot`. `xnuboot` loads XNU and the RAM disk, and stays as a small EL2 monitor. `common/` holds the log (in Android's ramoops zone), the watchdogs, the EL2 monitor, ACPM, the touchscreen, and a USB fastboot device that stages iOS's 2.5 GB system image into RAM. The rest are the probes that worked out the hardware step by step: display, touch, clocks, GPU, and Wi-Fi probes 1–13. `tools/` packs and runs it all. |
| [`gpu/`](gpu/) | The Mali-G710 kernel driver: `redwood_mali*.c` (power, CSF firmware, MMU, Linux panthor's interface) and `MaliGPU.cpp` (the IOKit service Mesa talks to). `mesa/` holds the Mesa panfrost patch for Darwin and a libdrm stand-in. |
| [`wifi/`](wifi/) | The BCM4389 kernel driver (`redwood_bcm4389.c`: PCIe, MsgBuf rings, deep sleep, escan, the join, the WPA2 handshake, the network interface), HackiOS's Apple80211 glue with this port's changes, and the build scripts. |
| [`hackios-overlay/`](hackios-overlay/) | This port's changes to HackiOS's own files (display driver, GL presenter, SpringBoard injector, image builder) and `apply.sh`. **Some of these were rebuilt from backups and haven't been compiled since: read its README.** |
| [`docs/`](docs/) | The GPU and Wi-Fi engineering logs. |

## Building and running

You need an **unlocked** Pixel 7 Pro, rooted (Magisk) for reading the boot log, and a PC.

1. **HackiOS.** Clone it into `hackios/` here (git ignores that folder):
   `git clone https://github.com/oosexgang/hakios-xnu-kernel hackios`.
   Follow its README to extract iOS 16.5 and to set up the kernel build, which needs Linux or WSL.
2. **Your own files** go in `downloads/`, which git ignores:
   - `downloads/gpu/mali_csffw-r54p3.bin`: from the phone's `/vendor/firmware/` (`adb shell su -c cp ...`).
   - `downloads/wifi/fw/fw_bcmdhd.bin`, `bcmdhd.cal`, `bcmdhd_clm.blob`: from `/vendor/firmware/` too.
   - `downloads/wifi/join.c`: your network for unattended Wi-Fi tests only. The name goes on line 1 and
     the passphrase on line 2. **Never commit it**, and delete the boot images built with it.
3. **Kernel and image (Linux/WSL).** Run `HACKIOS=~/hackios bash hackios-overlay/apply.sh`, then
   `bash wifi/install-kernel.sh` (it builds the kernel), then `bash wifi/build-image.sh`.
4. **Boot (Windows, Git Bash, with LLVM and Android platform-tools):**
   `bash pixelboot/tools/ios-run.sh NAME 300`. This builds `xnuboot` with the kernel and RAM disk, boots
   it, stages the system image over USB, and saves the log as `downloads/probe/NAME.txt` when the
   phone is back in Android. See the script's header for its options (`KERNEL=`, `RAMDISK=`, `XARGS=`,
   `CPU=1`).

The scripts were written for one machine and have only been run there. Expect to adjust paths.

## Credits

- [HackiOS](https://github.com/oosexgang/hakios-xnu-kernel) by OOSEX GANG: the XNU port, the iOS user
  space bring-up, and everything this builds on.
- Linux's panthor, Google's bcmdhd and Linux's brcmfmac, read to write the drivers (see [NOTICE.md](NOTICE.md)).
- Mesa's panfrost.

## License

This project's own code is under [MIT](LICENSE). Files taken or adapted from HackiOS keep HackiOS's
licenses: APSL-2.0 for its XNU patch, MIT for the rest. [NOTICE.md](NOTICE.md) lists them.
