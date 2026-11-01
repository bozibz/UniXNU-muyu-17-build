#!/usr/bin/env python3
"""Publish the prelinked kernel as the reference candidate the display audits read.

The display audits (audit-mobile-graphics-legacy.py and neighbours) look for
build/xnu-prelink/native-owner-mobile-graphics-candidate.{macho,json}, a name this board stopped
producing when the prelink stage moved to kits/XnuPrelink/emit_prelink.py. The emit step already
writes every field those audits verify (candidate_sha256, original_executable_sha256, extents) in
the kernel plan, so this only republishes it under the expected name; it never regenerates or
reinterprets anything.

usage: publish-prelink-candidate.py [--plan kernel.json] [--tree DIR]
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = 'native-owner-mobile-graphics-candidate'
REQUIRED = ('candidate_sha256', 'original_executable_sha256', 'extents')
FAMILY = 'com.apple.iokit.IOMobileGraphicsFamily'


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', type=Path, default=ROOT / 'build/xnu/kernel.json')
    ap.add_argument('--tree', type=Path, default=ROOT)
    args = ap.parse_args(argv[1:])

    plan_path = args.plan
    if not plan_path.is_file():
        # kernel.json sits beside the kernel the payload step consumes.
        candidates = sorted(ROOT.glob('build/**/kernel.json'), key=lambda p: p.stat().st_mtime)
        if not candidates:
            print('publish-prelink-candidate: no kernel plan found', file=sys.stderr)
            return 1
        plan_path = candidates[-1]

    plan = json.loads(plan_path.read_text())
    missing = [k for k in REQUIRED if k not in plan]
    if missing:
        print('publish-prelink-candidate: plan lacks %s' % ', '.join(missing), file=sys.stderr)
        return 1
    if FAMILY not in plan['original_executable_sha256']:
        print('publish-prelink-candidate: plan has no %s executable hash' % FAMILY, file=sys.stderr)
        return 1

    out = args.tree / 'build/xnu-prelink'
    out.mkdir(parents=True, exist_ok=True)
    target_json = out / (NAME + '.json')
    shutil.copyfile(plan_path, target_json)

    # The matchable blob is the prelinked kernel the plan describes.
    macho = plan_path.with_suffix('.macho')
    if not macho.is_file():
        macho = plan_path.parent / 'kernel.macho'
    if not macho.is_file():
        print('publish-prelink-candidate: no kernel image beside %s' % plan_path, file=sys.stderr)
        return 1
    shutil.copyfile(macho, out / (NAME + '.macho'))
    print('published %s from %s' % (out / NAME, plan_path))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
