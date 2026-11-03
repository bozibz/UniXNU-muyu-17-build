#!/usr/bin/env python3
"""Host check: the offline APFS volume-table reader reports the seeds' live roles.

The point of `inspect-apfs-image.py` is that a role can be visible in one place and absent
from another. These assertions pin the two answers this port has already argued about:

  - `muyu-data-200g-data-role.dbr` carries role Data (0x40) in its **current checkpoint**
    (block 545587, xid 6), even though the first volume superblock ever written (block 162,
    xid 2) says role 0. The handoff's §30 ("the role was never written") is wrong because it
    only compared block 162.
  - `owner-data-32.dbr` / `muyu-data-200g.dbr` carry role 0, and the flashed root image's
    OS window carries exactly one volume, role System (0x1).

The root-image assertions read `../fw/userdata-backup/userdata-root.sparse`; they are skipped
when that backup is absent (it is not in Git).
"""
from pathlib import Path
import importlib.util
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('inspect_apfs', HERE / 'inspect-apfs-image.py')
apfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apfs)

FAIL = []


def require(condition, message):
    print(('ok   ' if condition else 'FAIL ') + message)
    if not condition:
        FAIL.append(message)


def summary(source, label):
    return apfs.Container(source, label).summary(apfs.MAX_SCAN)


WORKSPACE = HERE.parents[1]
seeds = WORKSPACE / 'fw/data-seed'
role = summary(apfs.PackedDisk(seeds / 'muyu-data-200g-data-role.dbr'), 'data-role seed')
require(role['volumes'][0]['role'] == 0x40, 'the Data seed reports role 0x40')
require(role['volumes'][0]['block'] == 545587 and role['volumes'][0]['xid'] == 6,
        'the role comes from the xid-6 checkpoint at block 545587, not block 162')
plain = summary(apfs.PackedDisk(seeds / 'owner-data-32.dbr'), 'owner-data-32')
require(plain['volumes'][0]['role'] == 0, 'the unmodified 32 MiB seed reports role 0')
require(plain['volumes'][0]['uuid'] == role['volumes'][0]['uuid'],
        'both seeds describe the same volume uuid')

root = WORKSPACE / 'fw/userdata-backup/userdata-root.sparse'
if root.exists():
    window = apfs.Window(apfs.SparseImage(root), 'OS')
    os_container = summary(apfs.Slice(window, window.apfs_partition()), 'OS window')
    names = [(v['name'], v['role'], v['uuid']) for v in os_container['volumes']]
    require(names == [('DawnUpdate21H461.J120J121J207J208OS', 1,
                       '04769d26-2ca0-4825-b443-e2e5f88300f2')],
            f'the boot container holds only the System volume: {names}')
    window = apfs.Window(apfs.SparseImage(root), 'SystemOS')
    cryptex = summary(apfs.Slice(window, 0), 'SystemOS window')
    require([cryptex['volumes'][0]['uuid'], cryptex['volumes'][0]['role']] ==
            ['8846fe78-2910-4297-af97-d59bb6f8fc16', 0],
            'the SystemOS window holds the role-0 cryptex volume')
else:
    print(f'skip {root.name} is not present')

sys.exit(1 if FAIL else 0)
