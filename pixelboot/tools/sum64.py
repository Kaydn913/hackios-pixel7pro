#!/usr/bin/env python3
"""The sum usbtest/xnuboot log for a download: little-endian 64-bit words of the file, a short tail
zero-padded, mod 2^64.

  python -I pixelboot/tools/sum64.py FILE
"""
import array, sys

total, size = 0, 0
with open(sys.argv[1], 'rb') as f:
    while chunk := f.read(64 << 20):
        size += len(chunk)
        if len(chunk) % 8:
            chunk += b'\0' * (-len(chunk) % 8)
        words = array.array('Q')
        words.frombytes(chunk)
        total += sum(words)
print(f'size=0x{size:x} sum64=0x{total & (2 ** 64 - 1):016x}')
