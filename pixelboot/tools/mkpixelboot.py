#!/usr/bin/env python3
"""
A boot image laid out like cheetah's stock boot.img, for `fastboot boot`:

  header v4 (page 4096, no ramdisk, os_version 0, no boot signature) | kernel | padding
  | vbmeta (AVB, algorithm NONE: one hash descriptor for "boot") | padding | AVB footer

padded to the boot partition's size (64 MB), as Google's own image is. An unlocked bootloader takes
the unsigned vbmeta (orange state). Written from the AVB and boot image formats, without avbtool.

  python -I pixelboot/tools/mkpixelboot.py KERNEL OUT [--cmdline "..."] [--partition-size BYTES]
"""
import argparse, hashlib, os, struct

PAGE = 4096

def align(n, a=PAGE):
    return (n + a - 1) // a * a

def boot_v4(kernel, cmdline):
    hdr = struct.pack('<8sIIII4I I1536sI', b'ANDROID!', len(kernel), 0, 0, 1584, 0, 0, 0, 0, 4,
                      cmdline.encode(), 0)
    assert len(hdr) == 1584
    img = hdr.ljust(PAGE, b'\0') + kernel
    return img.ljust(align(len(img)), b'\0')

def hash_descriptor(image, name, salt):
    digest = hashlib.sha256(salt + image).digest()
    body = struct.pack('>Q32sIIII60s', len(image), b'sha256', len(name), len(salt), len(digest), 0, b'')
    body += name + salt + digest
    body = body.ljust(align(len(body), 8), b'\0')
    return struct.pack('>QQ', 2, len(body)) + body          # tag 2: hash descriptor

def vbmeta(descriptors):
    aux = descriptors.ljust(align(len(descriptors), 64), b'\0')
    hdr = struct.pack('>4sIIQQIQQQQQQQQQQQII48s80s', b'AVB0', 1, 0,
                      0, len(aux),                           # authentication block (none), auxiliary block
                      0,                                     # algorithm NONE
                      0, 0, 0, 0, 0, 0, 0, 0,                # hash, signature, public key, key metadata
                      0, len(descriptors),                   # descriptors at the start of the aux block
                      0, 0, 0,                               # rollback index, flags, rollback location
                      b'pixelboot mkpixelboot.py', b'')
    assert len(hdr) == 256
    return hdr + aux

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kernel')
    ap.add_argument('out')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--partition-size', type=lambda s: int(s, 0), default=64 << 20)
    a = ap.parse_args()
    image = boot_v4(open(a.kernel, 'rb').read(), a.cmdline)
    vb = vbmeta(hash_descriptor(image, b'boot', os.urandom(32)))
    footer = struct.pack('>4sIIQQQ28s', b'AVBf', 1, 0, len(image), len(image), len(vb), b'')
    assert len(footer) == 64
    total = len(image) + len(vb)
    assert total + len(footer) <= a.partition_size, 'does not fit the partition'
    out = image + vb + b'\0' * (a.partition_size - total - len(footer)) + footer
    open(a.out, 'wb').write(out)
    print(f'{a.out}: kernel {os.path.getsize(a.kernel)} bytes, image {len(image)}, vbmeta {len(vb)}, total {len(out)}')

main()
