"""
The WPA2 PMK of a join file, for wifiprobe12: PBKDF2-HMAC-SHA1 of the passphrase with the SSID as
salt, 4096 rounds, 32 bytes (or the passphrase itself when it is 64 hex digits), written raw to OUT.
The file is parsed as the probes parse it: an optional UTF-8 BOM, the SSID up to the first CR or LF,
one line break (CRLF or LF), the passphrase up to the next CR or LF.

    python -I pixelboot/tools/wpa_pmk.py JOIN_FILE OUT

Computed here because on the phone (MMU off, every access to Device memory) 8192 rounds of HMAC
take seconds the probe can't pet the watchdog in. Prints lengths only.
"""
import hashlib
import string
import sys


def line(data, start):
    end = start
    while end < len(data) and data[end] not in b"\r\n":
        end += 1
    return data[start:end], end


def main():
    data = open(sys.argv[1], "rb").read()
    pos = 3 if data.startswith(b"\xef\xbb\xbf") else 0
    ssid, pos = line(data, pos)
    if data[pos:pos + 1] == b"\r":
        pos += 1
    if data[pos:pos + 1] == b"\n":
        pos += 1
    passphrase, _ = line(data, pos)
    if len(passphrase) == 64 and all(chr(c) in string.hexdigits for c in passphrase):
        pmk = bytes.fromhex(passphrase.decode())
    else:
        pmk = hashlib.pbkdf2_hmac("sha1", passphrase, ssid, 4096, 32)
    with open(sys.argv[2], "wb") as f:
        f.write(pmk)
    print(f"wpa_pmk: {len(pmk)} bytes, SSID of {len(ssid)} bytes, passphrase of {len(passphrase)}")


main()
