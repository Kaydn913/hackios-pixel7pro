#!/bin/bash
# Print the phone's serial number: $SERIAL if set, else the one phone attached (adb, then fastboot).
# pixelboot programs answer fastboot with it (common/usbdev.c, PHONE_SERIAL), so run.sh can find them.
set -u
if [ -n "${SERIAL:-}" ]; then echo "$SERIAL"; exit 0; fi
ADB=${ADB:-adb} FB=${FB:-fastboot}
s=$("$ADB" devices 2>/dev/null | awk 'NR > 1 && $2 == "device" { print $1; exit }')
[ -n "$s" ] || s=$("$FB" devices 2>/dev/null | awk 'NF { print $1; exit }')
[ -n "$s" ] || { echo "serial.sh: no phone found (adb or fastboot); set SERIAL=<your phone's serial>" >&2; exit 1; }
echo "$s"
