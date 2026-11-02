#!/usr/bin/env python3
"""Build the muyu thermal survey against this source kernel's actual ABI.

Mirrors build-muyu-usb-debug.py: the probe includes <sys/param.h>/<sys/systm.h> and
<pexpert/pexpert.h>, so it has to compile with the BSD-side record (kern_exec.o) rather than the
osfmk one - the two type universes collide otherwise.

Read-only probe: nothing here touches a device.
"""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/muyu-thermal'
OUT.mkdir(parents=True, exist_ok=True)

record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))

source = ROOT / 'kits/muyu/MuyuThermalProbe.c'
obj = OUT / 'MuyuThermalProbe.o'
command = []
args = record['arguments']
i = 0
while i < len(args):
    arg = args[i]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        i += 1
    elif arg == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not arg.startswith('-W'):
        command.append(arg)
    i += 1
command += ['-I', str(source.parent), '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-Wno-incompatible-sysroot']
subprocess.run(command, cwd=record['directory'], check=True, timeout=120)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')

archive = OUT / 'libMuyuThermal.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'header_sha256': hashlib.sha256((source.parent / 'MuyuThermal.h').read_bytes()).hexdigest(),
    'object_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
}, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 muyu thermal survey build')
