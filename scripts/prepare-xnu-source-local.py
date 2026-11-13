#!/usr/bin/env python3
"""Pre-seeded replacement for UniXNU's prepare-xnu-source.py.

prepare-xnu-source.py restores Apple's public headers into .cache/xnu-work/ over the
GitHub API. Hosted CI runners share an anonymous quota that is exhausted, and even the
own token can be rate-limited on a busy repository, so this line carries the 26 files in
fw/xnu-foundation/ and restores them from there; the build never needs the network for
this step again.
"""
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def device_root():
    p = Path(__file__).resolve()
    while True:
        candidate = p / 'UniXNU-muyu' / 'fw' / 'xnu-foundation'
        if candidate.is_dir():
            return candidate
        if p == p.parent:
            raise SystemExit('UniXNU-muyu/fw/xnu-foundation not found from ' + str(Path(__file__).resolve()))
        p = p.parent


SRC = device_root()
dst_driverkit = ROOT / '.cache/xnu-work/iokit/DriverKit'
dst_crypto = ROOT / '.cache/xnu-work/iokit/DriverKit/crypto'
dst_trustcache = ROOT / '.cache/xnu-work/EXTERNAL_HEADERS/TrustCache'
for path in (dst_driverkit, dst_crypto, dst_trustcache):
    path.mkdir(parents=True, exist_ok=True)
shutil.copytree(SRC / 'iokit' / 'DriverKit', dst_driverkit, dirs_exist_ok=True)
shutil.copytree(SRC / 'EXTERNAL_HEADERS' / 'TrustCache', dst_trustcache, dirs_exist_ok=True)
print(f'Pre-restored Apple public headers from {SRC}; network restore skipped')
