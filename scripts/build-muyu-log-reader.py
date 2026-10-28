#!/usr/bin/env python3
"""Build the fastboot-boot-only muyu log reader.

Ported from UniXNU-sheng/package/linux-log/build-linux-log-boot.py. It takes any Android boot image
(header v0 through v4) whose kernel can drive this board's UFS and USB device controller, keeps the
kernel and DTB bytes untouched, and appends an initramfs with busybox plus package/linux-log/init:
the init brings up a CDC ACM gadget, dd's the last 8192 blocks of userdata into RAM and serves a shell,
and scripts/read-sheng-log.py --decode turns the fetched window into text.

Nothing on the device is written; the medium is only read.

usage: build-muyu-log-reader.py --boot <kernel boot image> --busybox <arm64 busybox> --out log-reader.img
       [--init PATH] [--module FILE ...] [--append-dtb <dtb>]
"""
import argparse
import gzip
import hashlib
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
CMDLINE = b'rdinit=/init console=tty0 loglevel=4 consoleblank=0'


def cpio(entries):
    """newc archive; entries are (path, mode, data) with directories first."""
    out = bytearray()
    for ino, (name, mode, data) in enumerate(entries + [('TRAILER!!!', 0, b'')], 1):
        path = name.encode() + b'\0'
        out += b'070701' + b''.join(b'%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(path), 0)) + path
        out += b'\0' * (-len(out) % 4) + data
        out += b'\0' * (-len(out) % 4)
    return bytes(out)


def parse_boot(blob):
    """Return (kernel, dtb) from an Android boot image, v0 or v3/v4."""
    if blob[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    header_version = struct.unpack_from('<I', blob, 40)[0] if len(blob) >= 44 else 0
    if header_version >= 3:
        # v3/v4 have no page_size field (the spec fixes 4096) and put dtb_size right after cmdline.
        kernel_size = struct.unpack_from('<I', blob, 8)[0]
        ramdisk_size = struct.unpack_from('<I', blob, 12)[0]
        os_version = struct.unpack_from('<I', blob, 16)[0]
        page_size = 4096
        dtb_size = struct.unpack_from('<I', blob, 1580)[0] if header_version >= 4 else 0
        kernel_addr = 0x00008000
        tags = 0x00000100
    else:
        (kernel_size, kernel_addr, ramdisk_size, _raddr, _second_size, _saddr,
         tags, page_size, header_version, os_version) = struct.unpack_from('<10I', blob, 8)
        header_size = page_size
        dtb_size = 0
    if page_size < 512 or page_size & (page_size - 1):
        raise SystemExit(f'implausible page size {page_size}')
    off = page_size
    kernel = blob[off:off + kernel_size]
    off += (kernel_size + page_size - 1) // page_size * page_size
    off += (ramdisk_size + page_size - 1) // page_size * page_size if ramdisk_size else 0
    dtb = blob[off:off + dtb_size] if dtb_size else b''
    return kernel, dtb, kernel_addr, tags, os_version


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--boot', required=True, type=Path)
    ap.add_argument('--busybox', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--init', type=Path, default=HERE.parent / 'package/linux-log/init')
    ap.add_argument('--module', action='append', default=[], type=Path)
    ap.add_argument('--append-dtb', type=Path, help='append this DTB to the kernel when the image carries none')
    a = ap.parse_args()

    kernel, dtb, kernel_addr, tags, os_version = parse_boot(a.boot.read_bytes())
    if not dtb and a.append_dtb:
        dtb = a.append_dtb.read_bytes()
    if dtb and kernel[-len(dtb):] != dtb:
        kernel = kernel + b'\0' * (-len(kernel) % 8) + dtb

    dirs = ['bin', 'dev', 'proc', 'sys', 'run', 'tmp', 'lib', 'lib/modules']
    entries = [(d, 0o40755, b'') for d in dirs]
    entries += [('dev/console', 0o20600, b''), ('init', 0o100755, a.init.read_bytes()),
                ('bin/busybox', 0o100755, a.busybox.read_bytes())]
    entries += [('lib/modules/' + m.name, 0o100644, m.read_bytes()) for m in a.module]
    ramdisk = gzip.compress(cpio(entries), 9, mtime=0)

    # Emit header v4: every image this bootloader already accepts on this board is v4, and a v0 image was
    # rejected with "Failed to load/authenticate boot image: Load Error".
    page = 4096
    pad = lambda b: b + b'\0' * (-len(b) % page)
    header = bytearray(1584)
    header[:8] = b'ANDROID!'
    struct.pack_into('<I', header, 8, len(kernel))
    struct.pack_into('<I', header, 12, len(ramdisk))
    struct.pack_into('<I', header, 16, 0)
    struct.pack_into('<I', header, 20, 1584)          # header_size
    struct.pack_into('<I', header, 40, 4)             # header_version
    header[44:44 + len(CMDLINE)] = CMDLINE
    struct.pack_into('<I', header, 1580, 0)           # dtb_size: the DTB comes from vendor_boot
    image = bytes(header) + pad(kernel) + pad(ramdisk)
    image = image + b'\0' * (-len(image) % 4096)   # whole image page-aligned
    a.out.write_bytes(image)
    print(f'BUILT {a.out} ({len(image)} bytes, {len(image)/1048576:.2f} MiB) '
          f'kernel {len(kernel)/1048576:.2f} MiB ramdisk {len(ramdisk)/1048576:.2f} MiB')
    print(f'sha256 {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
