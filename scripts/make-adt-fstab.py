#!/usr/bin/env python3
"""Regenerate scripts/adt-filesystems.bin from the stock iPad7,1 device tree.

Why this exists: launchd mounts volumes through `mount`, which asks the user device tree for
`/filesystems/fstab`. The synthetic ADT had no such node, so `mount_apfs` exited with EX_NOINPUT
(66) and launchd's panic-on-non-zero-exit policy turned that into the critical-exit panic
(`CRITEXIT pid 3 comm mount_apfs ppid 1 sig 0 code 66`). The node is now carried in the ADT; this
script is how it is produced, so the next person can re-derive it instead of trusting a blob.

The stock fstab lists volumes a real iPad has: xART, System, Preboot, Data, Update, Hardware (plus
two ephemeral fstab variants). `mount` walks them in `vol.fs_mntorder`, and this board exposes no
APFS volume role to userland at all - every one of them answers
`DT_get_fstab_entries:12284: failed to get volume for role: N` (256 xART, 16 Preboot, 64 Data,
1 System). Dropping them one by one was tried to the point of an empty fstab and the boot still
failed, so the checked-in blob is the stock set; --drop stays for experiments.

Usage:
    scripts/make-adt-fstab.py [--dt PATH] [--drop xart-vol,update-vol,hardware-vol] [--out PATH]

The default --dt is the decoded DeviceTree of the pinned IPSW; pass --fdt to skip the im4p+lzfse
step when a decoded tree is already around.
"""
from pathlib import Path
import argparse
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
DEFAULT_DT = WORKSPACE / 'UniXNU/out/ipsw/21H461/DeviceTree.j120ap.im4p'
MASK = 0x7fffffff          # bit 31 of a property length marks a syscfg reference


def decode_im4p(path, tmp):
    """Pull the lzfse stream out of an IM4P and decode it with Apple's compression_tool."""
    data = path.read_bytes()
    start = data.find(b'IM4P')
    if start < 0:
        raise SystemExit(f'{path}: no IM4P header')
    body = data[start + 20:]
    stream = body.find(b'bvx2')
    if stream < 0:
        raise SystemExit(f'{path}: no lzfse stream (already decoded? pass --fdt)')
    packed = tmp / 'dt.lzfse'
    packed.write_bytes(body[stream:])
    raw = tmp / 'dt.fdt'
    subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse',
                    '-i', str(packed), '-o', str(raw)], check=True, capture_output=True)
    return raw.read_bytes()


def parse(buf, off):
    """Return (end offset, name, [(key, data)], [child offsets]) for the node at off."""
    props, children = struct.unpack_from('<II', buf, off)
    p = off + 8
    name = None
    values = []
    for _ in range(props):
        key, length = struct.unpack_from('<32sI', buf, p)
        p += 36
        length &= MASK
        data = buf[p:p + length]
        p += length + (-length % 4)
        key = key.split(b'\0')[0]
        values.append((key, data))
        if key == b'name':
            name = data.rstrip(b'\0').decode('latin1')
    kids = []
    for _ in range(children):
        end, _, _, _ = parse(buf, p)
        kids.append((p, end))
        p = end
    return p, name, values, kids


def add_property(node, name, data):
    """Return `node` (one serialized node) with one more property on its root object."""
    props, count = struct.unpack_from('<II', node, 0)
    p = 8
    for _ in range(props):
        _key, length = struct.unpack_from('<32sI', node, p)
        p += 36 + (length & MASK) + (-(length & MASK) % 4)
    head = struct.pack('<II', props + 1, count)
    prop = struct.pack('<32sI', name.encode(), len(data)) + data + bytes(-len(data) % 4)
    return head + node[8:p] + prop + node[p:]


def rebuild(buf, off, drop):
    """Re-emit the node at off, dropping child nodes whose name is in `drop`."""
    end, name, props, kids = parse(buf, off)
    kept = []
    for start, child_end in kids:
        _e, child_name, _p, _k = parse(buf, start)
        if child_name in drop:
            continue
        kept.append(rebuild(buf, start, drop))
    if name is not None and name in drop:
        return b''
    out = struct.pack('<II', len(props), len(kept))
    for key, data in props:
        out += struct.pack('<32sI', key, len(data))
        out += data + bytes(-len(data) % 4)
    return out + b''.join(kept)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dt', type=Path, default=DEFAULT_DT, help='DeviceTree IM4P of the IPSW')
    parser.add_argument('--fdt', type=Path, help='already decoded device tree (skips --dt)')
    parser.add_argument('--drop', default='',
                        help='comma separated node names to drop; empty keeps the stock set. '
                             'Dropping was tried for every volume in turn and did not fix the boot '
                             '(see the workspace HANDOFF-2026-11-02 section 24), so the default is stock')
    parser.add_argument('--out', type=Path, default=ROOT / 'scripts/adt-filesystems.bin')
    parser.add_argument('--do-not-use-sep', action='store_true',
                        help='add DoNotUseSEP to the /filesystems node. APFS reads it there to decide '
                             '"insecure boot is enabled" (apfs_vfsops), which is what lets it mount an '
                             'unencrypted Data-role volume - the seed we ship is unencrypted, and without '
                             'this APFS refuses it with "unencrypted data volume is not allowed"')
    args = parser.parse_args()

    if args.fdt:
        buf = args.fdt.read_bytes()
    else:
        with tempfile.TemporaryDirectory() as tmp:
            buf = decode_im4p(args.dt, Path(tmp))

    _end, _name, _props, kids = parse(buf, 0)
    filesystems = None
    for start, child_end in kids:
        _e, child_name, _p, _k = parse(buf, start)
        if child_name == 'filesystems':
            filesystems = (start, child_end)
            break
    if filesystems is None:
        raise SystemExit('no /filesystems node in the device tree')

    drop = {name for name in args.drop.split(',') if name}
    node = rebuild(buf, filesystems[0], drop)
    if args.do_not_use_sep:
        node = add_property(node, 'DoNotUseSEP', struct.pack('<I', 1))
    args.out.write_bytes(node)

    _e, _n, _p, _k = parse(node, 0)
    kept = [parse(node, start)[1] for start, _ in parse(node, 0)[3]]
    print(f'{args.out}: {len(node)} bytes, fstab children kept: {kept}, dropped: {sorted(drop)}')


if __name__ == '__main__':
    main()
