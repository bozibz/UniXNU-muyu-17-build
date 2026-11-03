#!/usr/bin/env python3
"""Add a directory tree to the Data seed (the RAM disk mounted at /private/var).

Some of Apple's userland boot glue bind-mounts directories that live on a volume this port does not
have. `mount -P 2` (launchd's `mount-phase-2`) ends with `bindfs` mounts whose sources are

    /private/var/hardware/Pearl/System/Library/Pearl/ReferenceFrames        (optional)
    /private/var/hardware/FactoryData/System/Library/Caches/com.apple.factorydata   (mandatory)

On a real device those live on the Hardware volume; here /private/var is the Data seed, which the
kernel mounts read-write, so the directories can simply be written into the seed image.

    scripts/add-seed-tree.py --base fw/data-seed/muyu-data-200g.dbr \\
        --output fw/data-seed/muyu-data-200g-factory.dbr \\
        --path /private/var/hardware/FactoryData/System/Library/Caches/com.apple.factorydata \\
        [--from /some/host/directory]

Without --from the directory is created empty, which is enough to satisfy the lookup (the mount then
shadowes the target with an empty directory); with --from its contents are copied so the bind is a
no-op. Refuses to overwrite the output and refuses paths outside /private/var.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKED_RAMDISK = ROOT.parent / 'UniXNU/drivers/DarwinBoot/Test'

def attach(raw):
    """Attach read-write and mount the APFS volume; return (device, mount point)."""
    import plistlib
    plist = subprocess.run(['hdiutil', 'attach', '-nomount', '-owners', 'off', '-nobrowse',
                            '-imagekey', 'diskimage-class=CRawDiskImage', '-plist', str(raw)],
                           capture_output=True, check=True).stdout
    entities = plistlib.loads(plist)['system-entities']
    device = next((entity['dev-entry'] for entity in entities
                   if entity.get('content-hint', '').startswith('41504653')), entities[-1]['dev-entry'])
    subprocess.run(['diskutil', 'mount', device], check=True, capture_output=True)
    info = plistlib.loads(subprocess.run(['diskutil', 'info', '-plist', device],
                                         capture_output=True, check=True).stdout)
    return device, Path(info['MountPoint'])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--base', type=Path, required=True, help='seed to extend (DBRDISK1)')
    parser.add_argument('--output', type=Path, required=True, help='new seed')
    parser.add_argument('--path', required=True, help='absolute path inside the seed')
    parser.add_argument('--from', dest='source', type=Path, default=None,
                        help='directory whose contents are copied into --path (default: empty)')
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit(f'refusing to overwrite {args.output}')
    if not args.path.startswith('/private/var/'):
        raise SystemExit('the path must live under /private/var: that is where the seed is mounted')
    inside = args.path[len('/private/var/'):]
    if '..' in inside.split('/'):
        raise SystemExit('the path cannot contain ..')

    sys.path.insert(0, str(PACKED_RAMDISK))
    import packed_ramdisk
    logical, extents = packed_ramdisk.regions(args.base.read_bytes())

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='muyu-seed-', dir=str(ROOT / 'build')) as temporary:
        work = Path(temporary)
        image = work / 'seed.raw'
        with image.open('wb') as handle:
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        device, mountpoint = attach(image)
        try:
            target = mountpoint / inside
            target.mkdir(parents=True, exist_ok=True)
            files = 0
            if args.source is not None:
                if not args.source.is_dir():
                    raise SystemExit(f'{args.source} is not a directory')
                for entry in sorted(args.source.rglob('*')):
                    destination = target / entry.relative_to(args.source)
                    if entry.is_dir():
                        destination.mkdir(parents=True, exist_ok=True)
                    else:
                        shutil.copyfile(entry, destination)
                        files += 1
            print(f'created {args.path} ({files} files)')
        finally:
            subprocess.run(['diskutil', 'unmountDisk', 'force', device], check=True, capture_output=True)
            subprocess.run(['hdiutil', 'detach', device], check=True, capture_output=True)
        if image.read_bytes()[32:36] != b'NXSB':
            raise SystemExit('the edited seed is no longer an APFS container')
        metadata = packed_ramdisk.pack(image, args.output)

    report = {'output': args.output.name, 'sha256': hashlib.sha256(args.output.read_bytes()).hexdigest(),
              'bytes': args.output.stat().st_size, 'path': args.path,
              'source': str(args.source) if args.source else None,
              'note': 'Data seed fixture for mount -P 2 bindfs; see HANDOFF-2026-11-02.md section 34'}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
