#!/usr/bin/env python3
"""Read the APFS volume table out of a muyu image, offline.

Boot bring-up on this port keeps asking the same question with a different wording:
"which volume roles does this image really carry, and in which container". An APFS
volume's role is not one number in one place:

  - `diskutil apfs list` reads the container's live state,
  - block 162 of a seed is only the *first* volume superblock ever written,
  - `changeVolumeRole` writes the role into a **later checkpoint**, so diffing the
    original block-162 copies shows nothing even though the role is set.

That is exactly the mistake §30 of the workspace handoff made (2026-11-03: "the role
was never written"). This tool reports what APFS itself would resolve at the
container's current transaction id: it scans every *stored* block for checksum-valid
volume superblocks and keeps the newest one per file-system oid.

Why a scan and not the object map B-tree: this kernel's APFS uses an `omap_phys`
layout whose B-tree node key/value offsets differ from the published reference, and a
hand-rolled walker that is subtly wrong silently reports "no such volume" -- the exact
failure mode this tool exists to end. Fletcher64 over the 4096-byte block (zeroing the
checksum field) is a hard filter: a stale or torn block cannot pass it.

Inputs it understands:

  --sparse-image PATH --window OS    Android sparse image of the flashed `userdata`
                                     (SHNGROOT manifest -> the named window -> GPT ->
                                     the APFS partition; this is what fastboot writes)
  --packed PATH                      DBRDISK1 packed seed (`packed_ramdisk.regions`)
  --raw PATH [--offset BYTES]        a plain image, optional byte offset to the container

Examples:

  python3 scripts/inspect-apfs-image.py --packed ../fw/data-seed/muyu-data-200g-data-role.dbr
  python3 scripts/inspect-apfs-image.py --sparse-image ../fw/userdata-backup/userdata-root.sparse --window OS

Nothing here writes to an image.
"""
from pathlib import Path
import argparse
import json
import struct
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
PACKED_RAMDISK = ROOT.parent / 'UniXNU/drivers/DarwinBoot/Test'

APFS_PARTITION_TYPE = '7c3457ef-0000-11aa-aa11-00306543ecac'
BLOCK = 4096
CHUNK = 8 * 1024 * 1024
MAX_SCAN = 8 * 1024 ** 3


def fletcher64(block):
    """The APFS object checksum (`obj_phys_t.o_cksum`) over one 4096-byte block."""
    modulus = 0xFFFFFFFF
    first = second = 0
    for (word,) in struct.iter_unpack('<I', block[8:]):
        first = (first + word) % modulus
        second = (second + first) % modulus
    low = modulus - (first + second) % modulus
    high = modulus - (first + low) % modulus
    return struct.pack('<II', low, high) == block[:8]


def object_type(block):
    return struct.unpack_from('<I', block, 24)[0] & 0xFFFF


# --- sources: each yields its stored (non-hole) spans at logical offsets ---

class Reads:
    """A source: `blocks_at` yields (offset, bytes) for stored spans; `read` is a window."""

    def read(self, offset, length):
        out = bytearray(length)
        for low, piece in self.blocks_at(offset, length):
            out[low - offset:low - offset + len(piece)] = piece
        return bytes(out)


class SparseImage(Reads):
    """Android sparse image (the format `fastboot flash userdata` consumes).

    RAW chunks carry `chunk_blocks * block_size` bytes, FILL chunks a 4-byte pattern,
    DONT_CARE chunks are holes and CRC32 chunks carry no logical range. Reading only
    the stored spans is what keeps a 6 GiB container.raw scan cheap.
    """

    def __init__(self, path):
        self.path = Path(path)
        self.handle = open(self.path, 'rb')
        magic, major, minor, header_size, chunk_header, self.block_size, blocks, chunks, _ = \
            struct.unpack('<IHHHHIIII', self.handle.read(28))
        if magic != 0xED26FF3A or major != 1 or header_size != 28 or chunk_header != 12:
            raise SystemExit(f'{self.path}: not an Android sparse image')
        self.blocks = blocks
        self.chunks = []
        position = 0
        for _ in range(chunks):
            chunk_type, _, chunk_blocks, total = struct.unpack('<HHII', self.handle.read(12))
            payload = self.handle.tell()
            if chunk_type == 0xCAC1:                 # RAW
                self.chunks.append((chunk_type, position, chunk_blocks, payload, b''))
                self.handle.seek(payload + chunk_blocks * self.block_size)
            elif chunk_type == 0xCAC2:               # FILL
                self.chunks.append((chunk_type, position, chunk_blocks, payload, self.handle.read(4)))
            elif chunk_type == 0xCAC3:               # DONT_CARE (hole)
                self.chunks.append((chunk_type, position, chunk_blocks, payload, b''))
            elif chunk_type == 0xCAC4:               # CRC32, no logical range
                self.chunks.append((chunk_type, position, 0, payload, self.handle.read(4)))
            else:
                raise SystemExit(f'{self.path}: unknown sparse chunk {chunk_type:#x}')
            position += chunk_blocks
        self.extents = self._extents()

    def _extents(self):
        spans = []
        for chunk_type, position, blocks, payload, pattern in self.chunks:
            size = blocks * self.block_size
            if chunk_type == 0xCAC1:
                spans.append((position * self.block_size, ('file', payload), size))
            elif chunk_type == 0xCAC2 and pattern != bytes(4):
                spans.append((position * self.block_size, ('fill', pattern), size))
        return spans

    def blocks_at(self, offset, length):
        for start, (kind, source), size in self.extents:
            low, high = max(offset, start), min(offset + length, start + size)
            if low >= high:
                continue
            if kind == 'file':
                self.handle.seek(source + (low - start))
                piece = self.handle.read(high - low)
            else:
                piece = (source * ((high - low + 3) // 4))[:high - low]
            yield low, piece


class PackedDisk(Reads):
    """DBRDISK1 (`packed_ramdisk.py`): a zero-page-packed block image."""

    def __init__(self, path):
        sys.path.insert(0, str(PACKED_RAMDISK))
        import packed_ramdisk
        self.logical, extents = packed_ramdisk.regions(Path(path).read_bytes())
        self.extents = [(offset, memoryview(data)) for offset, data in extents]
        self.path = Path(path)

    def blocks_at(self, offset, length):
        for start, data in self.extents:
            low, high = max(offset, start), min(offset + length, start + len(data))
            if low < high:
                yield low, bytes(data[low - start:high - start])


class Raw(Reads):
    def __init__(self, path, base=0):
        self.path, self.base = Path(path), base
        self.size = self.path.stat().st_size - base

    def blocks_at(self, offset, length):
        with open(self.path, 'rb') as handle:
            handle.seek(self.base + offset)
            data = handle.read(min(length, self.size - offset))
        if data:
            yield offset, data


class Window(Reads):
    """One SHNGROOT window of the flashed `userdata` image."""

    def __init__(self, image, name):
        self.image = image
        data = b''.join(piece for _, piece in image.blocks_at(0, CHUNK))[:BLOCK]
        if data[:8] != b'SHNGROOT':
            raise SystemExit(f'{image.path}: no SHNGROOT manifest')
        _, version, count, _ = struct.unpack_from('<8sIIQ', data, 0)
        windows = {}
        for index in range(count):
            window, offset, size, _, _, _ = struct.unpack_from('<32sQQII32s', data, 24 + 128 * index)
            windows[window.split(b'\0')[0].decode()] = (offset, size)
        if name not in windows:
            raise SystemExit(f'{image.path}: no window {name!r}; have {sorted(windows)}')
        self.base, self.size = windows[name]

    def blocks_at(self, offset, length):
        for low, piece in self.image.blocks_at(self.base + offset, length):
            yield low - self.base, piece

    def apfs_partition(self):
        header = self.read(512, 92)
        if header[:8] != b'EFI PART':
            raise SystemExit(f'{self.base:#x}: the window has no GPT header')
        entries_lba, count, entry_size = struct.unpack_from('<QII', header, 72)
        table = self.read(entries_lba * 512, count * entry_size)
        for index in range(count):
            entry = table[index * entry_size:(index + 1) * entry_size]
            if str(uuid.UUID(bytes_le=entry[0:16])) != APFS_PARTITION_TYPE:
                continue
            first, last = struct.unpack_from('<QQ', entry, 32)
            if last >= first:
                return first * 512
        raise SystemExit('the window has no APFS partition')


class Slice(Reads):
    def __init__(self, source, base):
        self.source, self.base = source, base

    def blocks_at(self, offset, length):
        for low, piece in self.source.blocks_at(self.base + offset, length):
            yield low - self.base, piece


# --- APFS ------------------------------------------------------------------

ROLE_NAMES = {0x0: 'None', 0x1: 'System', 0x2: 'User', 0x4: 'Recovery', 0x8: 'VM',
              0x10: 'Preboot', 0x20: 'Installer', 0x40: 'Data', 0x80: 'Baseband',
              0x100: 'xART', 0x200: 'Hardware', 0x400: 'Backup', 0x800: 'Enterprise'}


class Container:
    def __init__(self, source, label=''):
        self.source, self.label = source, label
        superblock = source.read(0, BLOCK)
        if superblock[32:36] != b'NXSB' or not fletcher64(superblock):
            raise SystemExit(f'{label}: block 0 is no checksum-valid NX superblock')
        self.superblock = superblock
        self.block_size = struct.unpack_from('<I', superblock, 36)[0]
        if self.block_size != BLOCK:
            raise SystemExit(f'{label}: unsupported block size {self.block_size}')
        self.block_count = struct.unpack_from('<Q', superblock, 40)[0]
        self.uuid = uuid.UUID(bytes=superblock[72:88])
        self.xid = struct.unpack_from('<Q', superblock, 16)[0]
        self.desc_base, = struct.unpack_from('<Q', superblock, 112)
        self.desc_index, self.desc_len = struct.unpack_from('<II', superblock, 136)
        self.max_file_systems = struct.unpack_from('<I', superblock, 180)[0]
        self.fs_oids = [oid for oid in
                        (struct.unpack_from('<Q', superblock, 184 + 8 * i)[0]
                         for i in range(min(self.max_file_systems, 100))) if oid]
        self.checkpoint_block = self.newest_superblock()

    def newest_superblock(self):
        """Block number of the current checkpoint's NX superblock (the ring's last block)."""
        start = self.desc_base + self.desc_index
        for ring in range(start, start + max(self.desc_len, 1)):
            block = self.source.read(ring * BLOCK, BLOCK)
            if block[32:36] == b'NXSB' and fletcher64(block):
                return ring
        raise SystemExit(f'{self.label}: no checksum-valid NX superblock in the checkpoint ring')

    def volumes(self, limit=MAX_SCAN):
        """Newest checksum-valid volume superblock per file-system oid."""
        newest, scanned = {}, 0
        for start, piece in self.source.blocks_at(0, min(self.block_count * BLOCK, limit)):
            scanned += len(piece)
            cursor = 0
            while True:
                found = piece.find(b'APSB', cursor)
                if found < 0:
                    break
                cursor = found + 1
                if (start + found) % BLOCK != 32:
                    continue
                low = start + found - 32
                block = self.source.read(low, BLOCK)
                if len(block) != BLOCK or not fletcher64(block) or object_type(block) != 0x0D:
                    continue
                oid, = struct.unpack_from('<Q', block, 8)
                xid, = struct.unpack_from('<Q', block, 16)
                if oid not in self.fs_oids:
                    continue
                if oid not in newest or xid > newest[oid][0]:
                    newest[oid] = (xid, low, block)
        result = []
        for oid in self.fs_oids:
            if oid not in newest:
                result.append({'fs_oid': oid, 'error': 'no checksum-valid superblock found'})
                continue
            xid, low, block = newest[oid]
            result.append({
                'fs_oid': oid,
                'block': low // self.block_size,
                'xid': xid,
                'name': block[704:960].split(b'\0')[0].decode('utf-8', 'replace'),
                'uuid': str(uuid.UUID(bytes=block[240:256])),
                'volume_group': str(uuid.UUID(bytes=block[1008:1024])),
                'role': struct.unpack_from('<H', block, 964)[0],
                'fs_index': struct.unpack_from('<I', block, 36)[0],
                'files': struct.unpack_from('<Q', block, 184)[0],
                'flags': struct.unpack_from('<Q', block, 264)[0],
            })
        return result, scanned

    def summary(self, limit):
        volumes, scanned = self.volumes(limit)
        return {'label': self.label, 'uuid': str(self.uuid), 'xid': self.xid,
                'block_count': self.block_count, 'bytes': self.block_count * BLOCK,
                'checkpoint_block': self.checkpoint_block,
                'scanned_bytes': scanned, 'volumes': volumes}


def render(summary):
    lines = [f"container {summary['uuid']} xid {summary['xid']} "
             f"{summary['bytes'] / 2**30:.2f} GiB checkpoint block {summary['checkpoint_block']}"]
    for volume in summary['volumes']:
        if 'error' in volume:
            lines.append(f"  fs_oid {volume['fs_oid']}: {volume['error']}")
            continue
        role = f"{ROLE_NAMES.get(volume['role'], '?')} ({volume['role']:#x})"
        lines.append(f"  {volume['name']:<22} role {role:<17} uuid {volume['uuid']} "
                     f"group {volume['volume_group']} block {volume['block']} xid {volume['xid']}")
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sparse-image', type=Path, help='Android sparse image of `userdata`')
    parser.add_argument('--window', default='OS', help='SHNGROOT window to inspect (default OS)')
    parser.add_argument('--packed', type=Path, help='DBRDISK1 packed seed')
    parser.add_argument('--raw', type=Path, help='plain disk image')
    parser.add_argument('--offset', type=lambda value: int(value, 0), default=None,
                        help='byte offset of the APFS container inside --raw')
    parser.add_argument('--scan-mib', type=int, default=None,
                        help=f'give up after this many MiB (default: the whole container, cap {MAX_SCAN // 2**20})')
    parser.add_argument('--extract', type=Path, default=None,
                        help='also write the APFS container bytes to this file (sparse: holes stay holes)')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()

    if args.sparse_image:
        window = Window(SparseImage(args.sparse_image), args.window)
        container = Container(Slice(window, window.apfs_partition()),
                              label=f'{args.sparse_image.name}:{args.window}')
    elif args.packed:
        container = Container(PackedDisk(args.packed), label=args.packed.name)
    elif args.raw:
        container = Container(Slice(Raw(args.raw), args.offset or 0), label=args.raw.name)
    else:
        parser.error('one of --sparse-image, --packed, --raw is required')
    limit = args.scan_mib * 2**20 if args.scan_mib else MAX_SCAN

    if args.extract:
        written = 0
        with args.extract.open('wb') as out:
            out.truncate(container.block_count * BLOCK)
            for low, piece in container.source.blocks_at(0, container.block_count * BLOCK):
                out.seek(low)
                out.write(piece)
                written += len(piece)
        print(f'{args.extract}: {container.block_count * BLOCK} bytes, {written} stored', file=sys.stderr)

    summary = container.summary(limit)
    print(json.dumps(summary, indent=2) if args.json else render(summary))


if __name__ == '__main__':
    main()
