#!/usr/bin/env python3
"""Rebuild the flashed `userdata` image from a patched root partition.

`fw/userdata-backup/userdata-root.sparse` is the only copy of the image this tablet boots from:
an Android sparse image whose logical content is a SHNGROOT manifest, the iPadOS window
("OS", a GPT disk holding the APFS container with the System volume) and the SystemOS window
("SystemOS", a raw APFS container). Some fixes have to live in the root filesystem itself --
today's is `/private/preboot/<boot-manifest-hash>/usr/standalone/firmware`, which `mount -P 1`
bind-mounts over `/usr/standalone/firmware` and treats an ENOENT on as fatal (exit 66). The
root volume is mounted read-only at runtime (`vfs_subr.c`: `MNT_RDONLY | MNT_ROOTFS`), so that
directory has to be baked into the image.

    # 1. extract the APFS partition and patch it (hdiutil attach -readwrite, mkdir, copy)
    scripts/inspect-apfs-image.py --sparse-image <sparse> --window OS --extract /tmp/root.raw
    # 2. put it back
    scripts/rebuild-userdata-image.py --source <sparse> --root-partition /tmp/root.raw \\
        --output <new sparse>

Only the changed window moves: its GPT prefix and trailing padding are copied from the source,
the APFS partition comes from `--root-partition`, and the other window is copied verbatim. The
manifest's per-window SHA-256 values and its own trailing checksum are recomputed.

The output is written as plain RAW chunks (no FILL/DONT_CARE), so it is about the size of the
logical image; `fastboot` and `tools/muyu-flash-mode.sh` accept it exactly like the stock sparse
file. Nothing here writes to `--source`.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import struct
import sys

BLOCK = 4096
CHUNK_BLOCKS = 1024                 # 4 MiB per RAW chunk, the size the stock image uses
MANIFEST_WINDOW = 1024 * 1024       # the manifest block, zero-padded to 1 MiB
SPARSE_MAGIC = 0xED26FF3A
COPY = 8 * 1024 * 1024


def load_inspector():
    path = Path(__file__).resolve().parent / 'inspect-apfs-image.py'
    spec = importlib.util.spec_from_file_location('inspect_apfs', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Sparse:
    """Read-only view of an Android sparse image (RAW, FILL, DONT_CARE, CRC32)."""

    def __init__(self, path):
        self.path = Path(path)
        self.handle = open(self.path, 'rb')
        magic, major, minor, header_size, chunk_header, self.block_size, blocks, chunks, _ = \
            struct.unpack('<IHHHHIIII', self.handle.read(28))
        if magic != SPARSE_MAGIC or major != 1 or header_size != 28 or chunk_header != 12:
            raise SystemExit(f'{self.path}: not an Android sparse image')
        if self.block_size != BLOCK:
            raise SystemExit(f'{self.path}: block size {self.block_size} is not {BLOCK}')
        self.blocks = blocks
        self.spans = []
        position = 0
        for _ in range(chunks):
            kind, _, span, total = struct.unpack('<HHII', self.handle.read(12))
            payload = self.handle.tell()
            if kind == 0xCAC1:
                self.spans.append((position * self.block_size, span * self.block_size, ('file', payload)))
                self.handle.seek(payload + span * BLOCK)
                position += span
            elif kind == 0xCAC2:
                self.spans.append((position * self.block_size, span * self.block_size, ('fill', self.handle.read(4))))
                position += span
            elif kind == 0xCAC3:
                self.spans.append((position * self.block_size, span * self.block_size, ('zero', b'')))
                position += span
            elif kind == 0xCAC4:
                self.handle.read(4)
            else:
                raise SystemExit(f'{self.path}: unknown sparse chunk {kind:#x}')

    def copy_range(self, out, offset, length, digest):
        """Copy length bytes from logical `offset` to `out` while feeding `digest`."""
        remaining, position = length, offset
        while remaining:
            span = next((s for s in self.spans if s[0] <= position < s[0] + s[1]), None)
            if span is None:
                # Sparse images may end (or gap) without an explicit DONT_CARE chunk: the
                # uncovered range is zero by definition.
                following = min((s[0] for s in self.spans if s[0] > position), default=None)
                take = remaining if following is None else min(remaining, following - position)
                data = bytes(take)
                out.write(data)
                digest.update(data)
                remaining -= take
                position += take
                continue
            start, size, (kind, source) = span
            take = min(remaining, start + size - position)
            if kind == 'file':
                self.handle.seek(source + (position - start))
                data = self.handle.read(take)
            elif kind == 'fill':
                data = (source * ((take + 3) // 4))[:take]
            else:
                data = bytes(take)
            out.write(data)
            digest.update(data)
            remaining -= take
            position += take


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=Path, required=True, help='the sparse image being modified')
    parser.add_argument('--root-partition', type=Path, required=True,
                        help='patched APFS partition of the window (raw, without the GPT prefix)')
    parser.add_argument('--output', type=Path, required=True, help='new sparse image')
    parser.add_argument('--window', default='OS', help='window whose APFS partition is replaced')
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit(f'refusing to overwrite {args.output}')

    inspect_apfs = load_inspector()
    source = Sparse(args.source)
    manifest = bytearray()
    import io
    head = io.BytesIO()
    source.copy_range(head, 0, MANIFEST_WINDOW, hashlib.sha256())
    manifest = bytearray(head.getvalue())
    if manifest[:8] != b'SHNGROOT':
        raise SystemExit('the source has no SHNGROOT manifest')
    _, version, count, image_bytes = struct.unpack_from('<8sIIQ', manifest, 0)

    windows = {}
    for index in range(count):
        name, offset, size, block_size, flags, digest = \
            struct.unpack_from('<32sQQII32s', manifest, 24 + 128 * index)
        windows[name.split(b'\0')[0].decode()] = {
            'offset': offset, 'size': size, 'block_size': block_size, 'flags': flags,
            'manifest_at': 24 + 128 * index}
    if args.window not in windows:
        raise SystemExit(f'the source has no {args.window!r} window; have {sorted(windows)}')

    target = windows[args.window]
    window = inspect_apfs.Window(inspect_apfs.SparseImage(args.source), args.window)
    partition_offset = window.apfs_partition()
    superblock = inspect_apfs.Slice(window, partition_offset).read(0, BLOCK)
    container_bytes = (struct.unpack_from('<Q', superblock, 40)[0] *
                       struct.unpack_from('<I', superblock, 36)[0])
    if partition_offset + container_bytes > target['size']:
        raise SystemExit('the container does not fit in its window')
    if args.root_partition.stat().st_size != container_bytes:
        raise SystemExit(f'{args.root_partition} is {args.root_partition.stat().st_size} bytes; the '
                         f'container is {container_bytes}')

    total_blocks = image_bytes // BLOCK
    total_chunks = (total_blocks + CHUNK_BLOCKS - 1) // CHUNK_BLOCKS
    print(f'{args.output}: {image_bytes} bytes, {total_blocks} blocks, {total_chunks} chunks')

    digests = {}
    with args.output.open('wb') as out:
        # Sparse header first; the chunk headers follow as each 4 MiB RAW chunk is written.
        out.write(struct.pack('<IHHHHIIII', SPARSE_MAGIC, 1, 0, 28, 12, BLOCK,
                              total_blocks, total_chunks, 0))
        chunk_header = struct.pack('<HHII', 0xCAC1, 0, CHUNK_BLOCKS, 12 + CHUNK_BLOCKS * BLOCK)
        chunk_bytes = CHUNK_BLOCKS * BLOCK
        zeros = bytes(chunk_bytes)
        pending = bytearray()
        position = 0

        def emit(data):
            """Buffer bytes and flush whole 4 MiB RAW chunks; `position` tracks the logical offset."""
            nonlocal position
            pending.extend(data)
            position += len(data)
            while len(pending) >= chunk_bytes:
                out.write(chunk_header)
                out.write(pending[:chunk_bytes])
                del pending[:chunk_bytes]

        def pad_to(offset):
            if offset < position:
                raise SystemExit(f'internal: overran {offset} at {position}')
            while position < offset:
                emit(zeros[:offset - position])

        for name, info in windows.items():
            start, size = info['offset'], info['size']
            pad_to(start)
            digest = hashlib.sha256()
            if name == args.window:
                source.copy_range(_Sink(emit, digest), start, partition_offset, digest)
                with args.root_partition.open('rb') as handle:
                    remaining = container_bytes
                    while remaining:
                        data = handle.read(min(COPY, remaining))
                        if not data:
                            raise SystemExit('short read from the replacement partition')
                        emit(data)
                        digest.update(data)
                        remaining -= len(data)
                source.copy_range(_Sink(emit, digest), start + partition_offset + container_bytes,
                                  size - partition_offset - container_bytes, digest)
            else:
                source.copy_range(_Sink(emit, digest), start, size, digest)
            pad_to(start + size)
            digests[name] = (digest.hexdigest(), start, size)
            print(f'  {name}: {size} bytes at {start:#x} sha256 {digests[name][0]}')

        pad_to(image_bytes)
        if pending:
            raise SystemExit(f'internal: {len(pending)} bytes of padding are not a whole chunk')

        # Re-emit the manifest with the new per-window hashes, at the very beginning.
        for name, info in windows.items():
            struct.pack_into('<32sQQII32s', manifest, info['manifest_at'], name.encode(),
                             info['offset'], info['size'], info['block_size'], info['flags'],
                             bytes.fromhex(digests[name][0]))
        manifest[4064:4096] = hashlib.sha256(bytes(manifest[:4064])).digest()
        # The manifest is the first 4096 bytes of the *logical* image, which is the payload of the
        # first RAW chunk: sparse header (28) + chunk header (12).
        out.seek(28 + 12)
        out.write(manifest)

    digest = hashlib.sha256()
    with args.output.open('rb') as handle:
        for block in iter(lambda: handle.read(COPY), b''):
            digest.update(block)
    report = {'output': str(args.output), 'bytes': args.output.stat().st_size,
              'sha256': digest.hexdigest(), 'source': str(args.source),
              'root_partition': str(args.root_partition),
              'windows': {name: {'offset': start, 'size': size, 'sha256': sha}
                          for name, (sha, start, size) in digests.items()},
              'note': 'SHNGROOT + GPT/APFS rebuilt from an edited root partition'}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


class _Sink:
    """`out.write` for Sparse.copy_range: feed the emitter and the digest together."""

    def __init__(self, emit, digest):
        self.emit, self.digest = emit, digest

    def write(self, data):
        self.emit(data)
        return len(data)


if __name__ == '__main__':
    main()
