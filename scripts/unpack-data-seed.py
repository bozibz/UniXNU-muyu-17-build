#!/usr/bin/env python3
"""Extract the packed Data seed (scratch-data.img) from a built payload image.

A board with `packed_data_ramdisk = true` has the payload builder copy the Data seed into the payload
FAT volume as EFI/UniXNU/Payloads/scratch-data.img and point the ADT's PackedRAMDisk mapping at it
(scripts/unixnu/payload.py). The seed itself only ever comes from a device that already has a Data
volume, so recovering it from an image that boots is the way to move it between builds - which is
what makes a new OS line possible without re-dumping the tablet.

The volume is FAT12/16/32; the file is found by its short directory entry and read through the FAT
chain, so a fragmented or padded image reads the same way.

Usage: unpack-data-seed.py IMAGE [--output FILE] [--name SCRATCH-DATAIMG]
"""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512


def short_name(name):
    """The 11-byte FAT short name of a file name ('scratch-data.img' -> b'SCRATCH-DATIMG')."""
    stem, _, suffix = name.partition('.')
    return (stem.upper()[:8].ljust(8) + suffix.upper()[:3].ljust(3)).encode('ascii')


class Volume:
    def __init__(self, data, base):
        self.data, self.base = data, base
        bpb = data[base:base + 512]
        # Every arithmetic step below divides by the sector size, so an implausible BPB bails out here
        # instead of raising on the thousands of sectors that merely happen to end in 0x55AA.
        self.valid = struct.unpack_from('<H', bpb, 0x0B)[0] in (512, 1024, 2048, 4096)
        self.bytes_per_sector = struct.unpack_from('<H', bpb, 0x0B)[0] if self.valid else 512
        self.sectors_per_cluster = bpb[0x0D]
        self.reserved = struct.unpack_from('<H', bpb, 0x0E)[0]
        self.fats = bpb[0x10]
        self.root_entries = struct.unpack_from('<H', bpb, 0x11)[0]
        self.total = struct.unpack_from('<H', bpb, 0x13)[0] or struct.unpack_from('<I', bpb, 0x20)[0]
        self.fat_sectors = struct.unpack_from('<H', bpb, 0x16)[0] or struct.unpack_from('<I', bpb, 0x24)[0]
        self.root_cluster = struct.unpack_from('<I', bpb, 0x2C)[0]
        self.cluster_bytes = self.bytes_per_sector * self.sectors_per_cluster
        self.fat_at = base + self.reserved * self.bytes_per_sector
        self.data_at = base + (self.reserved + self.fats * self.fat_sectors) * self.bytes_per_sector
        self.root_at = self.data_at + (self.root_entries * 32 + self.bytes_per_sector - 1) // self.bytes_per_sector * self.bytes_per_sector
        self.bits = 32 if self.fat_sectors == struct.unpack_from('<I', bpb, 0x24)[0] and struct.unpack_from('<I', bpb, 0x24)[0] else 16

    def sane(self):
        return (self.valid and self.bytes_per_sector in (512, 1024, 2048, 4096) and self.sectors_per_cluster
                and self.sectors_per_cluster & (self.sectors_per_cluster - 1) == 0 and 1 <= self.fats <= 2
                and 0 < self.fat_sectors and self.total and self.data_at > self.base)

    def cluster_offset(self, cluster):
        return self.data_at + (cluster - 2) * self.cluster_bytes

    def next_cluster(self, cluster):
        at = self.fat_at + cluster * 4 if self.bits == 32 else self.fat_at + cluster * 2
        value, = struct.unpack_from('<I' if self.bits == 32 else '<H', self.data, at)
        if self.bits == 32:
            value &= 0x0FFFFFFF
            return None if value >= 0x0FFFFFF8 else value
        return None if value >= 0xFFF8 else value

    def chain(self, first, limit=1 << 20):
        cluster, seen = first, set()
        while cluster and cluster not in seen and len(seen) < limit:
            seen.add(cluster)
            yield cluster
            cluster = self.next_cluster(cluster)

    def entries(self, cluster):
        """Directory entries of one cluster directory (FAT32 subdirectories)."""
        blob = self.data[self.cluster_offset(cluster):self.cluster_offset(cluster) + self.cluster_bytes]
        for at in range(0, len(blob), 32):
            yield blob[at:at + 32]

    def root_entries_flat(self):
        if self.root_entries:
            count = self.root_entries * 32
            blob = self.data[self.root_at:self.root_at + count]
        else:
            blob = b''
            for cluster in self.chain(self.root_cluster):
                blob += self.data[self.cluster_offset(cluster):self.cluster_offset(cluster) + self.cluster_bytes]
        for at in range(0, len(blob), 32):
            yield blob[at:at + 32]

    def named_entries(self, entries):
        """(short entry, long name) for every real directory entry, assembling the LFN runs.

        A name that does not fit 8.3 is stored as a run of long-name entries followed by a mangled
        short one (scratch-data.img becomes SCRATCH~1.IMG), so matching the short name alone misses
        exactly the files this tool is for.
        """
        long, parts = '', []
        for item in entries:
            if len(item) < 32 or item[0] == 0xE5:
                continue
            if item[11] == 0x0F:
                chunk = (item[1:11] + item[14:26] + item[28:32]).decode('utf-16-le', 'replace')
                parts.append((item[0] & 0x1F, chunk))
                continue
            if item[0] in (0x00, 0xE5):
                continue
            if parts:
                long = ''.join(chunk for _, chunk in sorted(parts))
                long = long.split('\uffff')[0].split('\x00')[0]
            yield item, long
            parts, long = [], ''

    def find(self, name, path=('EFI', 'UNIXNU', 'PAYLOADS')):
        """The directory entry for the file called `name` under `path`, as (first cluster, size)."""
        entries = list(self.root_entries_flat())
        for component in path:
            found = None
            for item, long in self.named_entries(entries):
                if not item[11] & 0x10:
                    continue
                if long.lower() == component.lower() or \
                        item[0:11].rstrip(b' ') == short_name(component).rstrip(b' '):
                    found = struct.unpack_from('<H', item, 26)[0] | \
                        (struct.unpack_from('<H', item, 20)[0] << 16)
                    break
            if not found:
                return None, 0
            entries = list(self.entries(found))
        for item, long in self.named_entries(entries):
            if item[11] & 0x18:
                continue
            if long.lower() == name.lower():
                cluster = struct.unpack_from('<H', item, 26)[0] | (struct.unpack_from('<H', item, 20)[0] << 16)
                size, = struct.unpack_from('<I', item, 28)
                return cluster, size
        return None, 0


def candidates(image):
    """Every sector in the image that looks like a FAT boot sector."""
    for at in range(0, len(image) - SECTOR, SECTOR):
        if image[at + 510:at + 512] != b'\x55\xaa':
            continue
        volume = Volume(image, at)
        if volume.sane():
            yield volume


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--name', default='scratch-data.img')
    args = parser.parse_args()

    image = args.image.read_bytes()
    for volume in candidates(image):
        cluster, size = volume.find(args.name)
        if not cluster or not size:
            continue
        blob = bytearray()
        for index in volume.chain(cluster):
            start = volume.cluster_offset(index)
            blob += image[start:start + volume.cluster_bytes]
            if len(blob) >= size:
                break
        if len(blob) < size:
            print(f'{args.image}: chain of cluster {cluster} gave {len(blob)} of {size} bytes', file=sys.stderr)
            continue
        blob = bytes(blob[:size])
        output = args.output or args.image.with_name('data-seed.img')
        output.write_bytes(blob)
        print(f'{args.image}: FAT{volume.bits} at {volume.base:#x}, {args.name} cluster {cluster}, '
              f'{size} bytes -> {output}')
        return 0
    print(f'{args.image}: no payload volume carries {args.name}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
