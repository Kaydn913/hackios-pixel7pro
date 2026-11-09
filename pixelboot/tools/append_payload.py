#!/usr/bin/env python3
"""
Put an XNUP payload (xnuloader/mkpayload.py) behind a pixelboot Image, where the program finds it
at _end: the Image is padded to the size its header asks for (bss included), the payload follows,
and the header's image_size grows to cover it, so the bootloader leaves room for all of it.

  python -I pixelboot/tools/append_payload.py IMAGE PAYLOAD OUT
"""
import struct, sys

image = bytearray(open(sys.argv[1], 'rb').read())
payload = open(sys.argv[2], 'rb').read()
assert image[56:60] == b'ARM\x64', 'not an arm64 Image'
assert payload[:4] == b'XNUP', 'not an XNUP payload'
size, = struct.unpack_from('<Q', image, 16)
assert len(image) <= size, 'the Image is bigger than its header says'
image += b'\0' * (size - len(image))
image += payload
image += b'\0' * (-len(image) % 4096)
struct.pack_into('<Q', image, 16, len(image))
open(sys.argv[3], 'wb').write(image)
print(f'{sys.argv[3]}: program {size} + payload {len(payload)} = {len(image)} bytes')
