#!/usr/bin/env python3
"""
Wrap a file in the lz4 legacy format (what the Pixel's stock boot.img kernel uses), without an lz4
library: every block is one literals-only sequence, which is valid LZ4 that does not compress.

  python -I pixelboot/tools/lz4legacy.py IN OUT
"""
import struct, sys

MAGIC = 0x184C2102
BLOCK = 8 << 20        # the legacy format's uncompressed block size

def literal_block(data):
    n = len(data)
    out = bytearray([min(n, 15) << 4])
    if n >= 15:
        rest = n - 15
        out += b'\xff' * (rest // 255) + bytes([rest % 255])
    return bytes(out) + data

def main():
    data = open(sys.argv[1], 'rb').read()
    out = bytearray(struct.pack('<I', MAGIC))
    for i in range(0, len(data), BLOCK):
        blk = literal_block(data[i:i + BLOCK])
        out += struct.pack('<I', len(blk)) + blk
    open(sys.argv[2], 'wb').write(out)

main()
