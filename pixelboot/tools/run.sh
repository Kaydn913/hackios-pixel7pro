#!/bin/bash
# One run on the phone, from Android to Android, with the log at the end (Git Bash on Windows).
#
#   pixelboot/tools/run.sh BOOT_IMG LOG_NAME [STAGE_FILE]
#
# Reboots the phone (in Android, adb) to fastboot, boots BOOT_IMG; with STAGE_FILE, waits for our
# USB fastboot device (getvar product = pixelboot), stages the file and says "oem boot". Then waits
# for Android -- the run ends in the watchdog's warm reset -- and saves the ramoops log as
# downloads/probe/LOG_NAME.txt. Every step has a time limit; a step that fails says so and stops.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
# adb and fastboot: ADB= / FB= if they are not on PATH (winget's Google.PlatformTools is looked for too)
PT="$(cygpath -u "${LOCALAPPDATA:-}" 2>/dev/null)/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools"
ADB=${ADB:-$(command -v adb || echo "$PT/adb.exe")} FB=${FB:-$(command -v fastboot || echo "$PT/fastboot.exe")}
export ADB FB
# the phone: SERIAL=..., or the one phone attached
S=$(bash "$HERE/pixelboot/tools/serial.sh") || exit 1
IMG=${1:?usage: run.sh BOOT_IMG LOG_NAME [STAGE_FILE]} NAME=${2:?log name} STAGE=${3:-}
[ -f "$IMG" ] || { echo "run: no $IMG"; exit 1; }
[ -z "$STAGE" ] || [ -f "$STAGE" ] || { echo "run: no $STAGE"; exit 1; }
LOG="$HERE/downloads/probe/$NAME.txt"

adb_state() { "$ADB" devices 2>/dev/null | awk -v s=$S '$1==s{print $2}'; }
in_fastboot() { "$FB" devices 2>/dev/null | grep -q $S; }
step() { echo "run: $(date +%H:%M:%S) $*"; }

for i in $(seq 1 40); do [ "$(adb_state)" = device ] && break; in_fastboot && break; sleep 3; done
if ! in_fastboot; then
	[ "$(adb_state)" = device ] || { step "the phone is in neither Android nor fastboot"; exit 1; }
	timeout 20 "$ADB" -s $S reboot bootloader
	for i in $(seq 1 90); do sleep 2; in_fastboot && break; done	# the bootloader can take minutes
	in_fastboot || { step "no fastboot after reboot bootloader"; exit 1; }
fi
step "fastboot boot $(basename "$IMG")"
timeout 90 "$FB" -s $S boot "$IMG" 2>&1 | grep -E 'Booting|FAIL'

if [ -n "$STAGE" ]; then
	sleep 6
	ok=0
	for i in $(seq 1 40); do timeout 5 "$FB" -s $S getvar product 2>&1 | grep -q pixelboot && { ok=1; break; }; sleep 1; done
	[ $ok = 1 ] || { step "our USB device did not show up"; exit 1; }
	step "stage $(basename "$STAGE") ($(stat -c %s "$STAGE") bytes)"
	timeout 1800 "$FB" -s $S stage "$STAGE" 2>&1 | tail -1
	step "oem boot"
	timeout 15 "$FB" -s $S oem boot 2>&1 | tail -1
fi

step "waiting for Android"
for i in $(seq 1 400); do sleep 3; [ "$(adb_state)" = device ] && break; done
[ "$(adb_state)" = device ] || { step "Android did not come back"; exit 1; }
sleep 12
MSYS_NO_PATHCONV=1 timeout 20 "$ADB" -s $S exec-out "su -c 'cat /sys/fs/pstore/console-ramoops-0'" > "$LOG"
step "log: $LOG ($(wc -c < "$LOG") bytes); boot reason $(timeout 10 "$ADB" -s $S shell getprop ro.boot.bootreason | tr -d '\r')"
