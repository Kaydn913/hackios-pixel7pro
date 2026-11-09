#!/bin/bash
# iOS on the bare Pixel 7 Pro: build the shim with the current time and boot it with iOS's system image.
#   pixelboot/tools/ios-run.sh LOG_NAME [SESSION_SECS]     (default 1800: half an hour of iOS)
# By default iOS draws with the Mali (GPU-PLAN.md): the kernel built in WSL ~/hackios with the GPU's
# driver, the small image with Mesa in it (root-gl.apfs), and their boot-args -- two framebuffers that
# DECON0 is flipped between (decon=3), every frame scaled into one by the GPU (eagl.c's glscale), a
# surface held while the display has it (fb_inuse).
#   KERNEL=path   another kernel (hackios/prebuilt/mach.development.bcm2837: HackiOS's own)
#   RAMDISK=path  another small image (downloads/ios/root.apfs: no Mesa)
#   XARGS="..."   these boot-args instead of the GPU's (XARGS= : none)
#   CPU=1         as before the GPU: the prebuilt kernel, root.apfs, no boot-args -- the processor draws
set -eu
cd "$(dirname "$0")/../.."
GPU_ARGS="mali=3 gl=1 glwait=1 glquiet=1 decon=3 fb_threads=6 fb_beat=0 fb_inuse=1"
if [ -n "${CPU:-}" ]; then
	KERNEL=${KERNEL:-hackios/prebuilt/mach.development.bcm2837} RAMDISK=${RAMDISK:-downloads/ios/root.apfs} XARGS=${XARGS-}
else
	KERNEL=${KERNEL:-downloads/kernel/mach.development.bcm2837.mali} RAMDISK=${RAMDISK:-downloads/ios/root-gl.apfs}
	XARGS=${XARGS-$GPU_ARGS}
fi
python -I hackios/xnuloader/mkpayload.py -o pixelboot/out/payload/ios.bin "kernel=$KERNEL" "ramdisk=$RAMDISK" \
	"--cmdline=-v epoch=$(date +%s) sb_delay=8 $XARGS"
SERIAL=$(bash pixelboot/tools/serial.sh) || exit 1; export SERIAL
DEFINES="SESSION_SECS=${2:-1800} PHONE_SERIAL=$SERIAL ${XDEFINES:-}" COMPRESS=none PAYLOAD=pixelboot/out/payload/ios.bin bash pixelboot/build.sh xnuboot
cp pixelboot/out/xnuboot-boot.img pixelboot/out/xnuboot-ios-boot.img
bash pixelboot/tools/run.sh pixelboot/out/xnuboot-ios-boot.img "$1" downloads/ios/system.apfs
