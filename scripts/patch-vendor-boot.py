#!/usr/bin/env python3
"""Put the UniXNU payload into an Android vendor_boot v4 image.

The uke ABL delivers the vendor_boot vendor ramdisk and ignores the boot image
ramdisk, so the UniXNU payload lives here. A payload size change also moves the
DTB, the vendor ramdisk table and the bootconfig; this repacks them instead of
patching bytes at a fixed offset.

Two layouts:

  (default)          the payload REPLACES the vendor ramdisk. Smallest image, but it
                     throws away the 33.8 MB Android ramdisk that the bootloader's
                     charge-only path uses - measured 2026-10-26: with this image on
                     the tablet the battery stops charging anywhere.
  --fragments        keep the stock ramdisk as fragment 0 (type PLATFORM, byte for
                     byte at offset 0) and append the payload as fragment 1 (type
                     NONE, named "unixnu_payload"). Android still finds its ramdisk in
                     the v4 fragment table, so charging keeps working; the UEFI
                     payload driver learns to find the payload through the table.
                     Costs the stock ramdisk's size in the 96 MiB partition, so the
                     payload must be <= ~61.9 MiB (size_mib = 60 in board.toml).
"""
import argparse
import struct
from pathlib import Path


def align(value, page):
    return (value + page - 1) // page * page


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base', type=Path, required=True, help='vendor_boot v4 image to take the header/DTB/table from')
parser.add_argument('--payload', type=Path, required=True, help='vendor ramdisk (the UniXNU payload FAT image)')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--fragments', action='store_true',
                    help='keep the stock vendor ramdisk as fragment 0 and append the payload as '
                         'fragment 1 (keeps the Android ramdisk the charge path needs)')
parser.add_argument('--fragment-name', default='unixnu_payload', help='name of the payload fragment')
args = parser.parse_args()

base = args.base.read_bytes()
payload = args.payload.read_bytes()
magic, header_version, page, kernel_addr, ramdisk_addr = struct.unpack_from('<8sIIII', base, 0)
if magic != b'VNDRBOOT' or header_version != 4:
    raise SystemExit('expected an Android vendor_boot v4 image')
header_size = struct.unpack_from('<I', base, 2096)[0]
if header_size != 2128 or page < 512 or page & (page - 1):
    raise SystemExit('unexpected vendor_boot header layout')
old_ramdisk = struct.unpack_from('<I', base, 24)[0]
dtb_size = struct.unpack_from('<I', base, 2100)[0]
table_size, table_entries, table_entry_size, bootconfig_size = struct.unpack_from('<4I', base, 2112)
old_dtb = align(page + old_ramdisk, page)
old_table = align(old_dtb + dtb_size, page)
old_bootconfig = old_table + table_size
dtb = base[old_dtb:old_dtb + dtb_size]
table = bytearray(base[old_table:old_table + table_size])
bootconfig = base[old_bootconfig:old_bootconfig + bootconfig_size]
if len(dtb) != dtb_size or len(table) != table_size or len(bootconfig) != bootconfig_size:
    raise SystemExit('base image is shorter than its header describes')
if not args.fragments:
    if table_entries and table_entry_size >= 8:
        struct.pack_into('<II', table, 0, len(payload), 0)
    section = payload
    new_table = None
else:
    # Fragment 0 stays the stock ramdisk, untouched, at offset 0; fragment 1 is the payload,
    # page aligned right after it. Type 1 is VENDOR_RAMDISK_TYPE_PLATFORM (what Android's
    # first stage consumes); type 0 is VENDOR_RAMDISK_TYPE_NONE, which Android ignores.
    if old_ramdisk == 0 or not table_entries:
        raise SystemExit('base image has no stock vendor ramdisk/table to keep')
    stock_entry = bytes(base[old_table:old_table + table_entry_size])
    stock_size, stock_offset, stock_type = struct.unpack_from('<III', stock_entry, 0)
    if stock_size != old_ramdisk or stock_offset != 0:
        raise SystemExit('stock vendor ramdisk table entry does not describe the ramdisk')
    payload_offset = align(old_ramdisk, page)
    section = bytearray(payload_offset + len(payload))
    section[:old_ramdisk] = base[page:page + old_ramdisk]
    section[payload_offset:payload_offset + len(payload)] = payload
    entries = bytearray(2 * table_entry_size)
    entries[:table_entry_size] = stock_entry
    name = args.fragment_name.encode()[:32].ljust(32, b'\0')
    struct.pack_into('<III', entries, table_entry_size, len(payload), payload_offset, 0)
    entries[table_entry_size + 12:table_entry_size + 44] = name
    table = entries
    new_table = None

new_dtb = align(page + len(section), page)
new_table = align(new_dtb + dtb_size, page)
if new_table is None:
    new_table = align(new_dtb + dtb_size, page)
new_bootconfig = new_table + len(table)
size = align(new_bootconfig + bootconfig_size, page)
if size > len(base):
    raise SystemExit(f'the new image is {size} bytes but the vendor_boot partition holds {len(base)}; '
                     f'shrink the payload (board.toml [payload] size_mib)')
image = bytearray(size)
image[:header_size] = base[:header_size]
struct.pack_into('<I', image, 24, len(section))
if args.fragments:
    struct.pack_into('<III', image, 2112, len(table), 2, table_entry_size)
image[page:page + len(section)] = section
image[new_dtb:new_dtb + dtb_size] = dtb
image[new_table:new_table + len(table)] = table
image[new_bootconfig:new_bootconfig + bootconfig_size] = bootconfig
args.output.write_bytes(image)
layout = 'fragments (stock ramdisk + payload)' if args.fragments else 'payload replaces the ramdisk'
print(f'vendor_boot {args.output}: {len(image)} bytes [{layout}]; ramdisk section {len(section)} at {page:#x}; '
      f'dtb {dtb_size} at {new_dtb:#x}; table {len(table)} at {new_table:#x} '
      f'({2 if args.fragments else 1} entries); bootconfig {bootconfig_size} at {new_bootconfig:#x}')
