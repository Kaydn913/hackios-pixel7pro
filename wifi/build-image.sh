#!/bin/bash
# The GPU image with the BCM4389's firmware in fw/ (WIFI-PLAN.md W3b):
#   MSYS_NO_PATHCONV=1 wsl -d Ubuntu-24.04 -u <you> -- bash /mnt/c/<path to this repository>/wifi/build-image.sh
# Copies the phone's firmware files (downloads/wifi/fw: the vendor's, in no repository) into
# ~/hackios/fw-wifi, gives userland/build.sh a block that puts them into the small image (once), and
# builds that image as the GPU's is built (REST=1 GL=1: userland/root.apfs was byte-identical to
# downloads/ios/root-gl.apfs). The result is downloads/ios/root-gl-wifi.apfs; root-gl.apfs stays.
#
# JOIN=1: for runs with nobody at the phone (W3c): the user's network goes in too, as fw/network
# (downloads/wifi/join.c, rewritten by a parser that prints its lengths only; the kernel joins it
# with bcmjoin=1), and the job list is wifi/rc.jobs.bcmwifi (wait for an address, fetch). The
# result is downloads/ios/root-gl-wifi-join.apfs, which HOLDS THE PASSPHRASE: delete it after the
# run. fw-wifi/network and userland's own root/ and root.apfs are removed here once it is built.
set -e
WIN=${WIN:-$(cd "$(dirname "$0")/.." && pwd)}	# this repository (its downloads/: your own files)
cd ~/hackios
mkdir -p fw-wifi
install -m 0644 "$WIN/downloads/wifi/fw/fw_bcmdhd.bin" "$WIN/downloads/wifi/fw/bcmdhd.cal" \
	"$WIN/downloads/wifi/fw/bcmdhd_clm.blob" fw-wifi/

cd userland
if ! grep -q fw_bcmdhd.bin build.sh; then
	block='# ... or the Pixel 7 Pro'"'"'s Broadcom chip (bsd/dev/redwood_bcm4389.c): the phone'"'"'s /vendor/firmware/fw_bcmdhd.bin,\
# bcmdhd.cal (its NVRAM) and bcmdhd_clm.blob (the regulatory data), in ../fw-wifi\
if [ -f ../fw-wifi/fw_bcmdhd.bin ]; then\
	mkdir -p root/fw\
	install -m 0644 ../fw-wifi/fw_bcmdhd.bin ../fw-wifi/bcmdhd.cal ../fw-wifi/bcmdhd_clm.blob root/fw/\
fi'
	sed -i "/^# \.\.\. and the vibration motor's waveforms/i $block" build.sh
fi
# (and, once, the network for bcmjoin=1 beside it, when there is one)
if ! grep -q 'bcmjoin=1' build.sh; then
	sed -i '/install -m 0644 ..\/fw-wifi\/fw_bcmdhd.bin/a \	# the network to join with bcmjoin=1 (wifi/build-image.sh JOIN=1 puts it there for one build)\
	if [ -z "$SHARE" ] \&\& [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi' build.sh
fi
grep -n 'fw_bcmdhd\|bcmjoin' build.sh | head -4

out=root-gl-wifi.apfs
if [ -n "${JOIN:-}" ]; then
	out=root-gl-wifi-join.apfs
	install -m 0644 "$WIN/wifi/rc.jobs.bcmwifi" rc.jobs.bcmwifi
	# join.c as the probes parse it (pixelboot/tools/wpa_pmk.py): a BOM, the name to the first line
	# break, the passphrase to the next; written as two lines. Only the lengths are printed.
	(umask 077; python3 -I - "$WIN/downloads/wifi/join.c" ../fw-wifi/network <<'EOF'
import sys
data = open(sys.argv[1], "rb").read()
pos = 3 if data.startswith(b"\xef\xbb\xbf") else 0
def line(start):
    end = start
    while end < len(data) and data[end] not in b"\r\n":
        end += 1
    return data[start:end], end
ssid, pos = line(pos)
if data[pos:pos + 1] == b"\r":
    pos += 1
if data[pos:pos + 1] == b"\n":
    pos += 1
passphrase, _ = line(pos)
open(sys.argv[2], "wb").write(ssid + b"\n" + passphrase + b"\n")
print(f"build-image: fw-wifi/network: a name of {len(ssid)} bytes, a passphrase of {len(passphrase)}")
EOF
	)
	REST=1 GL=1 RC_JOBS=rc.jobs.bcmwifi ./build.sh root.apfs > /tmp/build-image-wifi.log 2>&1 || { rm -f ../fw-wifi/network; tail -20 /tmp/build-image-wifi.log; exit 1; }
	rm -f ../fw-wifi/network
else
	REST=1 GL=1 ./build.sh root.apfs > /tmp/build-image-wifi.log 2>&1 || { tail -20 /tmp/build-image-wifi.log; exit 1; }
fi
grep -n 'fw/' /tmp/build-image-wifi.log | tail -3
cp root.apfs "$WIN/downloads/ios/$out"
echo "build-image: downloads/ios/$out ($(stat -c %s root.apfs) bytes)"
if [ -n "${JOIN:-}" ]; then
	ls root/fw/
	rm -rf root root.apfs
	echo "build-image: userland/root and root.apfs removed (they held the network)"
fi
