#!/usr/bin/env python3
"""
Device trees out of a Pixel factory image, as text, without dtc.

  python -I pixelboot/tools/unpack_dt.py IMAGES_DIR OUT_DIR

IMAGES_DIR holds vendor_boot.img, dtbo.img and boot.img (from the factory zip's image-*.zip).
Writes OUT_DIR/vendor_boot-N.dts (the SoC trees the bootloader picks from), OUT_DIR/dtbo-N.dts (the
board overlays, with their id/rev) and prints what boot.img's kernel is compressed with.
"""
import os, struct, sys

def align(n, a):
    return (n + a - 1) // a * a

def split_dtbs(blob):
    """Concatenated FDTs (vendor_boot's dtb section holds one per SoC revision)."""
    out, off = [], 0
    while off + 8 <= len(blob):
        magic, size = struct.unpack_from('>II', blob, off)
        if magic != 0xd00dfeed:
            break
        out.append(blob[off:off + size])
        off = align(off + size, 4)
    return out

def is_text(v):
    if not v or v[-1] != 0:
        return False
    parts = v[:-1].split(b'\0')
    return all(p and all(32 <= c < 127 for c in p) for p in parts)

def fmt(name, v):
    if not v:
        return f'{name};'
    if is_text(v):
        return f'{name} = ' + ', '.join(f'"{p.decode()}"' for p in v[:-1].split(b'\0')) + ';'
    if len(v) % 4 == 0:
        return f'{name} = <' + ' '.join(f'0x{c:x}' for c in struct.unpack(f'>{len(v) // 4}I', v)) + '>;'
    return f'{name} = [' + ' '.join(f'{c:02x}' for c in v) + '];'

def to_dts(fdt):
    (magic, total, off_struct, off_strings, off_rsv, version, _, _, size_strings,
     size_struct) = struct.unpack_from('>10I', fdt, 0)
    assert magic == 0xd00dfeed
    strings = fdt[off_strings:off_strings + size_strings]
    lines, depth, p = [f'/* FDT v{version}, {total} bytes */'], 0, off_struct
    rsv = off_rsv
    while True:
        a, s = struct.unpack_from('>QQ', fdt, rsv)
        if a == 0 and s == 0:
            break
        lines.append(f'/memreserve/ 0x{a:x} 0x{s:x};')
        rsv += 16
    while p < off_struct + size_struct:
        tok, = struct.unpack_from('>I', fdt, p); p += 4
        if tok == 1:
            end = fdt.index(b'\0', p)
            name = fdt[p:end].decode(errors='replace') or '/'
            p = align(end + 1, 4)
            lines.append('\t' * depth + f'{name} {{')
            depth += 1
        elif tok == 2:
            depth -= 1
            lines.append('\t' * depth + '};')
        elif tok == 3:
            ln, nameoff = struct.unpack_from('>II', fdt, p); p += 8
            pname = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            lines.append('\t' * depth + fmt(pname, fdt[p:p + ln]))
            p = align(p + ln, 4)
        elif tok == 4:
            continue
        elif tok == 9:
            break
        else:
            raise ValueError(f'bad token {tok} at {p - 4}')
    return '\n'.join(lines) + '\n'

def vendor_boot_dtb(img):
    assert img[:8] == b'VNDRBOOT', 'not a vendor_boot image'
    version, page = struct.unpack_from('<II', img, 8)
    ramdisk_size, = struct.unpack_from('<I', img, 24)
    header_size, dtb_size = struct.unpack_from('<II', img, 2096)
    off = align(header_size, page) + align(ramdisk_size, page)
    print(f'vendor_boot: header v{version}, page {page}, ramdisk {ramdisk_size}, dtb {dtb_size} at {off:#x}')
    return img[off:off + dtb_size]

def dtbo_entries(img):
    magic, total, hsize, esize, count, eoff, page, version = struct.unpack_from('>8I', img, 0)
    assert magic == 0xd7b7ab1e, 'not a dtbo image'
    for i in range(count):
        size, off, id_, rev, *custom = struct.unpack_from('>8I', img, eoff + i * esize)
        yield i, id_, rev, custom, img[off:off + size]

def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    vb = open(os.path.join(src, 'vendor_boot.img'), 'rb').read()
    for i, fdt in enumerate(split_dtbs(vendor_boot_dtb(vb))):
        path = os.path.join(out, f'vendor_boot-{i}.dts')
        open(path, 'w').write(to_dts(fdt))
        print(f'  {path}')
    dtbo_path = os.path.join(src, 'dtbo.img')
    if os.path.exists(dtbo_path):
        for i, id_, rev, custom, fdt in dtbo_entries(open(dtbo_path, 'rb').read()):
            path = os.path.join(out, f'dtbo-{i}.dts')
            open(path, 'w').write(f'/* dtbo entry {i}: id {id_:#x} rev {rev:#x} custom {[hex(c) for c in custom]} */\n'
                                  + to_dts(fdt))
            print(f'  {path} (id {id_:#x}, rev {rev:#x})')
    boot_path = os.path.join(src, 'boot.img')
    if os.path.exists(boot_path):
        b = open(boot_path, 'rb').read(8192)
        ksize, = struct.unpack_from('<I', b, 8)
        hv, = struct.unpack_from('<I', b, 40)
        k = b[4096:4100]
        kind = {b'\x1f\x8b': 'gzip', b'\x02\x21': 'lz4 (legacy)', b'\x04\x22': 'lz4 (frame)'}.get(k[:2])
        if kind is None:
            kind = 'uncompressed Image' if b[4096 + 56:4096 + 60] == b'ARM\x64' else f'unknown ({k.hex()})'
        print(f'boot.img: header v{hv}, kernel {ksize} bytes, {kind}')

main()
