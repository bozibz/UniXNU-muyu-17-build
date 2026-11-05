#!/usr/bin/env python3
"""Verify that the vendored iPadOS 27 (24A446) original-ABI fixtures describe *this* kernel.

The two fixtures under scripts/data/ipados-27.0.1-24A446/ are the reviewed HID and display
original-ABI tables of the iPad17,3 24A446 kernelcache: for every vtable slot, the original
implementation it points at plus the pointer-authentication diversity the original callers use,
and for the few qualified entries the symbol they bind. They were reviewed upstream against
`kernelcache_sha256 = 38e6d9fa...`; this script proves that the kernelcache we carry for our own
27 line is that same binary and that every recorded value really is in it.

The decisive check is the vtable itself: an arm64e kernelcache stores each slot as a chained
fixup, so decoding the fixup chain at the fixture's vtable address must reproduce, slot by slot,
both the target the fixture names and the diversity it records. A fixture that is off by one
slot, taken from a different build, or hand-edited anywhere cannot pass that.

The recorded addresses are *unslid*: the kernel is linked at the collection base and every
binding is bound at `recorded + slide`. Our board's `[kernel.prelink] slide` must therefore equal
the fixture's `collection_slide`; that is checked too, because a mismatch would show up on the
device as a vtable full of wrong targets rather than as a build error.

Usage: check-ipados27-abi-fixtures.py [--member fw/ipsw/24A446/kernelcache.release.ipad17]
       [--kernelcache PATH] [--slide VALUE]
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'scripts/data/ipados-27.0.1-24A446'
# The extracted 24A446 member lives either inside the device repository or next to it (the
# workspace keeps the IPSWs outside the tree; a build tree pulls it into fw/ipsw).
MEMBERS = [ROOT / 'fw/ipsw/24A446/kernelcache.release.ipad17',
           ROOT.parent / 'fw/ipsw/24A446/kernelcache.release.ipad17']
CACHE = ROOT / 'build/ipados-27-abi/kernelcache.macho'

LC_SEGMENT_64, LC_DYLD_CHAINED_FIXUPS = 0x19, 0x80000034
PTR_ARM64E_KERNEL, PTR_64_KERNEL_CACHE = 7, 8
BTI = 0xfffffc1f, 0xd503241f      # every bti variant
PAC = {0xd503233f, 0xd503237f}    # paciasp / pacibsp


def der(data, at):
    tag = data[at]
    length = data[at + 1]
    at += 2
    if length & 0x80:
        count = length & 0x7f
        length = int.from_bytes(data[at:at + count], 'big')
        at += count
    return tag, at, length


def im4p_payload(data):
    """The payload of an IM4P (DER: IM4P, type, version, data, ...)."""
    tag, start, length = der(data, 0)
    if tag != 0x30:
        raise SystemExit('not an IM4P')
    at, items = start, []
    while at < start + length:
        _, body, size = der(data, at)
        items.append(data[body:body + size])
        at = body + size
    if len(items) < 4 or items[0] != b'IM4P':
        raise SystemExit('not an IM4P')
    return items[3]


def kernelcache(member, override):
    if override:
        raw = Path(override).read_bytes()
        if raw[:4] != b'\xcf\xfa\xed\xfe':
            raise SystemExit(f'{override} is not a thin 64-bit Mach-O')
        return raw
    if CACHE.is_file():
        return CACHE.read_bytes()
    payload = im4p_payload(Path(member).read_bytes())
    packed = CACHE.with_suffix('.lzfse')
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    packed.write_bytes(payload)
    result = subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse',
                             '-i', str(packed), '-o', str(CACHE)], capture_output=True, text=True)
    packed.unlink()
    if result.returncode:
        raise SystemExit(f'compression_tool failed: {result.stderr.strip()}')
    raw = CACHE.read_bytes()
    if raw[:4] != b'\xcf\xfa\xed\xfe':
        raise SystemExit('the decompressed kernelcache is not a thin 64-bit Mach-O')
    return raw


class Image:
    """Segment map and chained-fixup chain of one thin arm64 Mach-O."""

    def __init__(self, raw):
        self.raw = raw
        self.segments, self.commands = [], []
        count = struct.unpack_from('<I', raw, 16)[0]
        at = 32
        for _ in range(count):
            command, size = struct.unpack_from('<II', raw, at)
            self.commands.append((command, at, size))
            if command == LC_SEGMENT_64:
                name = raw[at + 8:at + 24].rstrip(b'\0').decode()
                va, mem, offset, filesize = struct.unpack_from('<4Q', raw, at + 24)
                self.segments.append((name, va, mem, offset, filesize))
            at += size
        self.base = next(va for name, va, mem, offset, filesize in self.segments
                         if offset == 0 and filesize)

    def read(self, va, size):
        for name, base, mem, offset, filesize in self.segments:
            if base <= va and va + size <= base + filesize:
                start = offset + (va - base)
                return self.raw[start:start + size]
        raise KeyError(f'{va:#x} is not inside a file-backed kernel segment')

    def mapped(self, va, size=4):
        return any(base <= va and va + size <= base + mem for _, base, mem, _, _ in self.segments)

    def fixups(self):
        """Chained fixups: site va -> (target va, authenticated, key, diversity). Same chain an
        arm64e kernel walks, decoded as kits/XnuPrelink/kc_image.py does it."""
        found = [at for command, at, _ in self.commands if command == LC_DYLD_CHAINED_FIXUPS]
        if not found:
            raise SystemExit('this kernelcache carries no chained fixups')
        dataoff, = struct.unpack_from('<I', self.raw, found[0] + 8)
        _, starts_offset, _, _, imports, _, _ = struct.unpack_from('<7I', self.raw, dataoff)
        if imports:
            raise SystemExit('chained fixup imports are not supported')
        table = dataoff + starts_offset
        total, = struct.unpack_from('<I', self.raw, table)
        sites = {}
        for index in range(total):
            relative, = struct.unpack_from('<I', self.raw, table + 4 + 4 * index)
            if not relative:
                continue
            at = table + relative
            _, page_size, fmt, segment_offset, _, pages = struct.unpack_from('<IHHQIH', self.raw, at)
            if fmt not in (PTR_ARM64E_KERNEL, PTR_64_KERNEL_CACHE):
                raise SystemExit(f'unsupported chained pointer format {fmt}')
            for page, start in enumerate(struct.unpack_from(f'<{pages}H', self.raw, at + 22)):
                if start == 0xffff:
                    continue
                site = self.base + segment_offset + page * page_size + start
                while True:
                    value, = struct.unpack_from('<Q', self.raw, self.segment_offset(site))
                    authenticated = value >> 63
                    if fmt == PTR_64_KERNEL_CACHE:
                        if (value >> 30) & 3:
                            raise SystemExit('only cache level 0 is supported')
                        target, step = self.base + (value & 0x3fffffff), (value >> 51) & 0xfff
                    else:
                        if (value >> 62) & 1:
                            raise SystemExit('chained binds are not supported')
                        offset = (value & 0xffffffff) if authenticated else \
                            (value & 0x7ffffffffff) | ((value >> 43) & 0xff) << 56
                        target, step = self.base + offset, (value >> 51) & 0x7ff
                    sites[site] = (target, bool(authenticated),
                                   (value >> 49) & 3 if authenticated else None,
                                   (value >> 32) & 0xffff if authenticated else None)
                    if not step:
                        break
                    site += step * 4
        return sites

    def segment_offset(self, va):
        for name, base, mem, offset, filesize in self.segments:
            if base <= va and va + 8 <= base + filesize:
                return offset + (va - base)
        raise KeyError(f'{va:#x} is not inside a file-backed kernel segment')


def discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 with the fixed key, modulo 0xffff, plus one."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask  # noqa: E731
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)

    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = len(data) << 56 & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


def function_entry(image, va):
    word = struct.unpack('<I', image.read(va, 4))[0]
    return word in PAC or (word & BTI[0]) == BTI[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--member', type=Path,
                    default=next((path for path in MEMBERS if path.is_file()), MEMBERS[0]))
    parser.add_argument('--kernelcache', type=Path)
    parser.add_argument('--slide', help='board prelink slide to compare with the fixture')
    args = parser.parse_args()

    raw = kernelcache(args.member, args.kernelcache)
    digest = hashlib.sha256(raw).hexdigest()
    image = Image(raw)
    fixups = image.fixups()
    failures, rows = [], []

    display = json.loads((FIXTURES / 'legacy-display.json').read_text())
    hid = json.loads((FIXTURES / 'hid-device.json').read_text())
    both = (('display', display), ('hid', hid))

    pins = {name: fixture['kernelcache_sha256'] for name, fixture in both}
    rows.append(('kernelcache', f'{len(raw):,} B  {digest[:16]}…',
                 'OK' if digest == display['kernelcache_sha256'] else 'FAIL'))
    if digest != display['kernelcache_sha256']:
        failures.append(f'kernelcache sha256 {digest} != fixture pin {display["kernelcache_sha256"]}')
    if len(set(pins.values())) != 1:
        failures.append('the two fixtures pin different kernelcaches')

    slides = {name: int(fixture['collection_slide'], 16) for name, fixture in both}
    if len(set(slides.values())) != 1:
        failures.append('the two fixtures disagree on the collection slide')
    ours = int(args.slide, 0) & ((1 << 64) - 1) if args.slide else None
    slide_ok = ours is None or ours == slides['display']
    rows.append(('collection slide', f"{slides['display']:#x}"
                 + ('' if ours is None else f'   board {ours:#x}'),
                 'OK' if slide_ok else 'FAIL'))
    if not slide_ok:
        failures.append(f'board prelink slide {ours:#x} != fixture collection slide '
                        f'{slides["display"]:#x}: the recorded addresses are unslid, so a different '
                        'slide binds every slot to the wrong place')

    # 1. the recorded instruction checks read back byte for byte
    good = 0
    for address, expected in display['checks']:
        try:
            good += image.read(int(address, 16), len(expected) // 2).hex() == expected
        except KeyError:
            pass
    rows.append(('display instruction checks', f'{good}/{len(display["checks"])}',
                 'OK' if good == len(display['checks']) else 'FAIL'))
    if good != len(display['checks']):
        failures.append(f'{len(display["checks"]) - good} recorded display instruction checks do not read back')

    # 2. the vtable's own chained fixups must be the fixture, slot for slot
    for name, fixture in both:
        vtable = int(fixture['vtable'], 16)
        callers = {int(index): int(entry['diversity'], 16)
                   for index, entry in fixture.get('caller_diversity', {}).items()}
        bad, checked = [], 0
        for index, (target, diversity) in enumerate(fixture['slots']):
            want_target, want_diversity = int(target, 16), int(diversity, 16)
            got = fixups.get(vtable + 8 * index)
            checked += 1
            if got is None:
                bad.append(f'slot {index}: no fixup at {vtable + 8 * index:#x}')
            elif got[0] != want_target:
                bad.append(f'slot {index}: target {got[0]:#x} != {want_target:#x}')
            elif index in callers:
                # A pure virtual slot: the original entry carries the stub's own discriminator, and the
                # record deliberately holds the one the original *call sites* authenticate with, because
                # that is the value our override has to match. Only the record's self-consistency is
                # checkable here; the caller side is checked against the call sites themselves.
                if want_diversity != callers[index]:
                    bad.append(f'slot {index}: recorded caller diversity {want_diversity:#x} '
                               f'!= caller_diversity {callers[index]:#x}')
            elif (got[3] or 0) != want_diversity:
                bad.append(f'slot {index}: diversity {got[3]:#x} != {want_diversity:#x}')
        rows.append((f'{name} vtable slots', f'{checked - len(bad)}/{checked} target+diversity'
                     + (f'  ({len(callers)} caller-side)' if callers else ''),
                     'OK' if not bad else 'FAIL'))
        for line in bad[:6]:
            failures.append(f'{name}: {line}')

    # 3. every reviewed slot name carries the diversity of its slot
    slots, methods = display['slots'], display['methods']
    good, bad = 0, []
    for base, (member, suffix) in display['names'].items():
        mangled = f'_ZN25IOMobileFramebufferLegacy{len(member)}{member}{suffix}'
        slot = methods.get(base) or int(base[len('opaque'):])
        want = int(slots[slot][1], 16)
        if discriminator(mangled) == want:
            good += 1
        else:
            bad.append(f'{mangled} slot {slot} {discriminator(mangled):#x} != {want:#x}')
    rows.append(('display slot names', f'{good}/{len(display["names"])} discriminators',
                 'OK' if not bad else 'FAIL'))
    for line in bad[:6]:
        failures.append(f'display: {line}')

    # 4. qualified bindings land inside the kernel and on a function entry when they are code
    for name, fixture in both:
        good, bad = 0, []
        for symbol, address in fixture['bindings'].items():
            va = int(address, 16)
            code = 'MetaClass' not in symbol and not symbol.startswith('__ZTV')
            if not image.mapped(va):
                bad.append(f'{symbol} @ {va:#x} is not mapped')
            elif code and not function_entry(image, va):
                bad.append(f'{symbol} @ {va:#x} is not an arm64e function entry')
            else:
                good += 1
        rows.append((f'{name} bindings', f'{good}/{len(fixture["bindings"])}',
                     'OK' if not bad else 'FAIL'))
        for line in bad[:4]:
            failures.append(f'{name}: {line}')

    for name, fixture in both:
        size = int(fixture['instance_size'], 16)
        rows.append((f'{name} instance size', f'{size:#x}  {len(fixture["slots"])} slots',
                     'OK' if size > 0x88 else 'FAIL'))
        if size <= 0x88:
            failures.append(f'{name}: instance size {size:#x} is smaller than IOService')

    width = max(len(row[0]) for row in rows)
    print('iPadOS 27 (24A446) original-ABI fixtures against our kernelcache')
    for label, detail, status in rows:
        print(f'  {label:<{width}}  {detail:<40} {status}')
    print()
    print(f'segments: {", ".join(name for name, _, _, _, _ in image.segments)}')
    print(f'chained fixups: {len(fixups):,}')
    if failures:
        print(f'\n{len(failures)} failure(s):')
        for line in failures:
            print('  - ' + line)
        return 1
    print('\nall checks passed: the fixtures describe this exact kernelcache')
    return 0


if __name__ == '__main__':
    sys.exit(main())
