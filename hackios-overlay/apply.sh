#!/bin/bash
# Put the Pixel 7 Pro's changes into a HackiOS checkout (Linux or WSL), as the port's own build did:
#   HACKIOS=~/hackios bash hackios-overlay/apply.sh
#
# Kernel (both the overlay xnu/files and the applied tree xnu-src/xnu-8796.121.2):
#   - the display driver and IOSurface changes (xnu/files/iokit/Kernel/MiniIO*; RECOVERED, see README.md)
#   - the Mali-G710 driver (gpu/redwood_mali*.c -> bsd/dev, gpu/MaliGPU.cpp -> iokit/Kernel), listed in
#     bsd/conf/files and iokit/conf/files, its init called from redwood_input.c after the Adreno's
# Userland (userland/): eagl.c, inject.c, inject_sb.c, build.sh (RECOVERED), Mesa's panthor stand-in for
#   libdrm (gpu/mesa -> tools/mesa), and the mkjobs.py fix (fix-mkjobs.py).
# Wi-Fi is separate: wifi/install-kernel.sh.
#
# Every edit is made once; running it again changes nothing. It was written for HackiOS as of
# 2026-10-23 (xnu-8796.121.2): a newer HackiOS may have moved things, and then a step says so and stops.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
O=$REPO/hackios-overlay
H=${HACKIOS:-$HOME/hackios}
A=$H/xnu-src/xnu-8796.121.2
[ -d "$H/xnu/files" ] || { echo "apply: no HackiOS checkout at $H (set HACKIOS=)"; exit 1; }
[ -d "$A" ] || { echo "apply: no applied tree $A (run HackiOS's xnu/apply.sh first)"; exit 1; }

need() { grep -q "$2" "$1" || { echo "apply: $1 has no line matching '$2' to anchor to"; exit 1; }; }

for T in "$H/xnu/files" "$A"; do
	install -m 0644 "$O"/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp "$O"/xnu/files/iokit/Kernel/MiniIOSurface.cpp \
		"$O"/xnu/files/iokit/Kernel/MiniIOSurface.h "$T/iokit/Kernel/"
	install -m 0644 "$REPO"/gpu/redwood_mali.c "$REPO"/gpu/redwood_mali_core.c "$REPO"/gpu/redwood_mali_uapi.c "$T/bsd/dev/"
	install -m 0644 "$REPO"/gpu/MaliGPU.cpp "$T/iokit/Kernel/"
	h=$T/bsd/sys/redwood_input.h c=$T/bsd/dev/redwood_input.c
	if ! grep -q redwood_mali_init "$h"; then
		need "$h" '^void redwood_adreno_init(void);'
		sed -i 's|^void redwood_adreno_init(void);.*$|&\nvoid redwood_mali_init(void);           /* boot-arg mali=N: the Mali-G710 (redwood_mali.c) */|' "$h"
	fi
	if ! grep -q 'redwood_mali_init();' "$c"; then
		need "$c" '^	redwood_adreno_init();$'
		sed -i 's|^\tredwood_adreno_init();$|&\n\tredwood_mali_init();|' "$c"
	fi
done
if ! grep -q redwood_mali.c "$A/bsd/conf/files"; then
	need "$A/bsd/conf/files" '^bsd/dev/redwood_adreno.c'
	sed -i 's|^bsd/dev/redwood_adreno.c\tstandard$|&\nbsd/dev/redwood_mali.c\tstandard|' "$A/bsd/conf/files"
fi
if ! grep -q MaliGPU.cpp "$A/iokit/conf/files"; then
	need "$A/iokit/conf/files" '^iokit/Kernel/RedwoodGPU.cpp'
	sed -i 's|^iokit/Kernel/RedwoodGPU.cpp\t\t\t\toptional iokitcpp$|&\niokit/Kernel/MaliGPU.cpp\t\t\t\toptional iokitcpp|' "$A/iokit/conf/files"
fi

U=$H/userland
if [ -d "$U" ]; then
	install -m 0644 "$O"/userland/src/eagl.c "$O"/userland/src/inject.c "$O"/userland/src/inject_sb.c "$U/src/"
	install -m 0755 "$O"/userland/build.sh "$U/build.sh"
	mkdir -p "$H/tools/mesa/redwood-drm"
	install -m 0644 "$REPO"/gpu/mesa/redwood_drm.c "$REPO"/gpu/mesa/xf86drm.h "$H/tools/mesa/redwood-drm/"
	install -m 0644 "$REPO"/gpu/mesa/panfrost-darwin.patch "$H/tools/mesa/"
	grep -q 'for variable in' "$U/mkjobs.py" || python3 -I "$O/userland/fix-mkjobs.py" "$U/mkjobs.py"
else
	echo "apply: no $U: the userland part is skipped"
fi

echo "apply: done. Kernel: cd $H && ./xnu-build.sh -j\$(nproc) -k   Image: cd $U && REST=1 GL=1 ./build.sh"
