# pixelboot: HackiOS's XNU on the bare Pixel 7 Pro (cheetah, Tensor G2)

Bare-metal programs booted with `fastboot boot` (nothing is flashed) on an unlocked Pixel 7 Pro.
As of 2026-10-23, `xnuboot` boots HackiOS's prebuilt XNU (iOS 16.5's kernel) natively: VM, IOKit,
the framebuffer and the RAM-disk root come up and `hello` runs as process 1.

| Program | What it does |
|---|---|
| `fbtest` | colour bands on the panel (EL, EL2->EL1 drop, watchdogs), 60 s, frame counter in the log |
| `probe` | dumps the display pipeline's registers and the bootloader's log into the log |
| `xnuboot` | loads XNU + RAM disk (an XNUP payload behind it) and jumps in, with the EL2 monitor |

`common/` -- `start.S` (Linux-Image header, EL2->EL1 drop, EL1 and EL2 vectors), `board.c` (the log,
the watchdogs, the EL1 exception handler), `el2.c` (the EL2 monitor: a fake PL011 caught by stage 2 =
the kernel's console in the log; PSCI traps). `tools/` -- packing (`mkpixelboot.py`: a boot image laid
out like the stock one, AVB footer, 64 MB), `append_payload.py`, `unpack_dt.py`, `lz4legacy.py`.

## Build and run (Git Bash, LLVM for Windows)

```sh
python -I hackios/xnuloader/mkpayload.py -o pixelboot/out/payload/hello-v.bin \
    kernel=hackios/prebuilt/mach.development.bcm2837 ramdisk=downloads/userland/hello --cmdline=-v
COMPRESS=none PAYLOAD=pixelboot/out/payload/hello-v.bin bash pixelboot/build.sh xnuboot
adb reboot bootloader && fastboot boot pixelboot/out/xnuboot-boot.img
# about a minute after the kernel falls silent the watchdog resets the phone (warm); then, rooted:
adb exec-out "su -c 'cat /sys/fs/pstore/console-ramoops-0'" > xnu.log
```

Images over a few MB must be `COMPRESS=none`: the bootloader refuses our literal-only lz4 then.

## The phone, as found

- Bootloader loads the Image at 0x80000000 and the DTB at 0xb4000000, enters at EL2. RAM: 2 GB at
  0x80000000 (full of firmware carve-outs), 10 GB at 0x880000000 (free: XNU's 4 GB window, panic log
  at 0x980000000).
- Cluster watchdogs at 0x10060000 / 0x10070000 (s3c2410 layout) run out in about a minute; a watchdog
  reset is warm and keeps the ramoops zone (0xfd3ff000, 2 MB) for Android's pstore; a normal reboot does not.
- Panel: S6E3HC4, command mode. DPP channel 0 scans 1440x3120 from 0xfac00000. The bootloader masks
  DECON0's trigger (0x1c240030 = 0x3070); unmasking it (HW_TRIG_EN on, HW_TRIG_MASK_DECON off, after a
  shadow update request at +0x50) refreshes the panel from memory at 60 fps.
- GICv3 at 0x10400000 / 0x10440000; cores 0-3 A55, 4-5 A78, 6-7 X1 (MPIDR Aff1); PSCI by SMC.

## Cores

All 8 are listed (boot core, then X1, X1, A78, A78, then the A55s as small cores) with `cpus=8`. The
firmware starts a core at EL2, so the monitor swaps CPU_ON's entry for `el2_secondary`, which gives the
core the boot core's EL2 and then enters the kernel at EL1. XNU brings up the big cores at boot ("5 of
8 processors are running") and keeps the little ones for when the big ones are busy. The monitor is
one core at a time (a bakery lock: exclusives are not reliable on EL2's MMU-off Device memory).

## Next

iOS's real user space
(root.apfs/system.apfs from hackios-xnu-userland; too big for a boot image -- needs storage or USB),
touch (Synaptics TCM over SPI behind Google's touch bus negotiator), keeping XNU alive past the
watchdog without console output (EL2_TRAP_WFI).
