#!/bin/bash
# Put redwood_bcm4389.c into the WSL kernel tree and build the kernel (WIFI-PLAN.md W3):
#   MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -u <you> -- bash /mnt/c/<path to this repository>/wifi/install-kernel.sh
# The driver goes into the overlay (xnu/files) and the applied tree (xnu-src/xnu-8796.121.2): listed
# in bsd/conf/files, declared in sys/redwood_input.h, called from redwood_input.c after the Mali's
# init. Each edit is made once. The kernel lands in downloads/kernel/mach.development.bcm2837.wifi
# (the GPU kernel, .mali, stays as it is). NOBUILD=1: only the install.
set -e
WIN=${WIN:-$(cd "$(dirname "$0")/.." && pwd)}	# this repository (its downloads/: your own files)
cd ~/hackios
A=xnu-src/xnu-8796.121.2

for root in xnu/files "$A"; do
	install -m 0755 "$WIN/wifi/redwood_bcm4389.c" "$root/bsd/dev/redwood_bcm4389.c"
	# (our changes to HackiOS's own Wi-Fi glue, W3c: redwood_apple80211_counts() for the driver's status line)
	for f in "$WIN"/wifi/ios-side/*.c; do install -m 0644 "$f" "$root/bsd/dev/"; done
	for f in "$WIN"/wifi/ios-side/*.h; do install -m 0644 "$f" "$root/bsd/sys/"; done
	h="$root/bsd/sys/redwood_input.h"
	grep -q redwood_bcm4389_init "$h" || sed -i '/^void redwood_mali_init(void);/a void redwood_bcm4389_init(void);        /* boot-arg bcmwifi=N: the Pixel 7 Pro'"'"'s BCM4389 Wi-Fi (redwood_bcm4389.c) */' "$h"
	c="$root/bsd/dev/redwood_input.c"
	grep -q 'redwood_bcm4389_init()' "$c" || sed -i 's/^\tredwood_mali_init();$/\tredwood_mali_init();\n\tredwood_bcm4389_init();/' "$c"
done
grep -q redwood_bcm4389 "$A/bsd/conf/files" || sed -i '/^bsd\/dev\/redwood_mali\.c/a bsd/dev/redwood_bcm4389.c\tstandard' "$A/bsd/conf/files"

echo "install-kernel: $(grep -c redwood_bcm4389 "$A/bsd/conf/files") conf/files line, $(grep -c 'redwood_bcm4389_init()' "$A/bsd/dev/redwood_input.c") call, $(grep -c redwood_bcm4389_init "$A/bsd/sys/redwood_input.h") declaration"
[ -n "${NOBUILD:-}" ] && exit 0

./xnu-build.sh -j12 -k > /tmp/xnu-build-wifi.log 2>&1 || true
grep -n 'error:\|warning:' /tmp/xnu-build-wifi.log | grep -i 'bcm4389\|redwood_input\|apple80211\|lx_wpa' | head -20
grep -n 'built kernel' /tmp/xnu-build-wifi.log | tail -2
k="$A/BUILD/obj/mach.development.bcm2837"
if [ -f "$k" ] && [ "$k" -nt "$WIN/wifi/redwood_bcm4389.c" ]; then
	cp "$k" "$WIN/downloads/kernel/mach.development.bcm2837.wifi"
	echo "install-kernel: kernel -> downloads/kernel/mach.development.bcm2837.wifi ($(stat -L -c %s "$k") bytes)"
else
	echo "install-kernel: no new kernel; see /tmp/xnu-build-wifi.log"
	tail -30 /tmp/xnu-build-wifi.log
fi
