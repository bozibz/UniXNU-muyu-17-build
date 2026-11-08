#!/usr/bin/env python3
"""Wrap the muyu log-reader initramfs in the board's own init_boot header.

Why this exists
---------------
`scripts/build-muyu-log-reader.py` builds a whole boot image from scratch and writes a header that
this bootloader does not parse: it puts the kernel at offset 1584 (right after its 1584-byte header)
while the ABL reads the kernel at the first page, 4096.  The result is a boot that fails before any
of our code runs and the bootloader falls back to recovery - measured 2026-11-08 14:59.

The fix is not to reverse-engineer the header: `fw/stock/init_boot.img` is the partition Android
itself uses for the generic ramdisk on this device, and its header is by definition one the
bootloader accepts.  So take that header verbatim, keep the ramdisk at page 1, and only change the
ramdisk bytes and the size field (offset 12 on this header layout - verified against both stock
boot.img and stock init_boot.img, whose os_version/header_size words are identical because they come
from the same build).

The ABL concatenates the init_boot ramdisk with the vendor_boot vendor ramdisk before boot, so the
stock 347 modules still land in the rootfs and the init script's insmod loop finds them.

usage: build-initboot-reader.py --template <stock init_boot.img> --busybox <arm64 busybox>
                                --init <init script> --out <init_boot-reader.img>
"""
import argparse
import gzip
import hashlib
import struct
from pathlib import Path

RAMDISK_OFF = 4096
SIZE_FIELD = 12          # ramdisk_size in this header layout


def cpio(entries):
    """newc archive; entries are (path, mode, data)."""
    out = bytearray()
    for ino, (name, mode, data) in enumerate(entries + [('TRAILER!!!', 0, b'')], 1):
        path = name.encode() + b'\0'
        out += b'070701' + b''.join(
            b'%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(path), 0)) + path
        out += b'\0' * (-len(out) % 4) + data
        out += b'\0' * (-len(out) % 4)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--template', required=True, type=Path)
    ap.add_argument('--busybox', required=True, type=Path)
    ap.add_argument('--init', required=True, type=Path)
    ap.add_argument('--rb', type=Path, help='optional reboot-with-reason helper (bin/rb)')
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()

    tpl = bytearray(a.template.read_bytes())
    if tpl[:8] != b'ANDROID!':
        raise SystemExit('template is not an Android image')
    if tpl[RAMDISK_OFF:RAMDISK_OFF + 2] == b'\x1f\x8b':
        raise SystemExit('template ramdisk is already gzip - refusing to guess its length')

    dirs = ['bin', 'dev', 'proc', 'sys', 'run', 'tmp', 'lib', 'lib/modules']
    entries = [(d, 0o40755, b'') for d in dirs]
    entries += [('dev/console', 0o20600, b''),
                ('init', 0o100755, a.init.read_bytes()),
                ('bin/busybox', 0o100755, a.busybox.read_bytes())]
    if a.rb:
        entries.append(('bin/rb', 0o100755, a.rb.read_bytes()))
    ramdisk = gzip.compress(cpio(entries), 9, mtime=0)

    cap = len(tpl) - RAMDISK_OFF
    if len(ramdisk) > cap:
        raise SystemExit(f'ramdisk {len(ramdisk)} does not fit in {cap} bytes')

    image = bytearray(tpl)
    image[RAMDISK_OFF:RAMDISK_OFF + len(ramdisk)] = ramdisk
    image[RAMDISK_OFF + len(ramdisk):] = b'\0' * (cap - len(ramdisk))
    struct.pack_into('<I', image, SIZE_FIELD, len(ramdisk))
    a.out.write_bytes(image)
    print(f'BUILT {a.out} ({len(image)} bytes) ramdisk {len(ramdisk)} bytes '
          f'size_field={struct.unpack_from("<I", image, SIZE_FIELD)[0]}')
    print(f'sha256 {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
