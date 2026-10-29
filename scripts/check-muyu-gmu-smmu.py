#!/usr/bin/env python3
"""Host checks for muyu's GMU stage-1 SMMU plan; no device I/O.

Three independent things, all offline:

  1. the board constants in package/MuyuGmuSmmu.h agree with ShengGmu.c's own validator
     (the five sizes, the three fixed IOVAs) and with the generated SMMU binding
     (base/size, GMU stream 5, GPU banks 0..1 so the GMU needs bank >= 2);
  2. package/ShengGpuSmmu.c still passes its complete lifecycle test with software
     register peers (ShengGpuSmmuHostTests.c), which is the library the plan drives;
  3. package/MuyuGmuSmmu.c builds and passes its own plan test (MuyuGmuSmmuHostTests.c)
     and compiles freestanding for aarch64 with the kernel's restrictions.

usage: check-muyu-gmu-smmu.py [--no-generate]

The two generated headers (build/gpu/ShengGmuSource.h, build/gpu/ShengGpuSmmuSource.h)
are regenerated first unless --no-generate is given, so the check never reads a stale
binding.
"""
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
OUT = ROOT / 'build/muyu-gmu-smmu'
KIT = WORK / 'UniXNU/kits/ArmSmmu4K'
BOARD = ROOT / 'package'
GENERATED = ROOT / 'build/gpu'
SMMU_KIT = ['ArmSmmu4K', 'ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KStreams',
            'ArmSmmu4KMmio', 'ArmSmmu4KAudit', 'ArmSmmu4KTableMemory', 'ArmSmmu4KLive']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, **kwargs):
    kwargs.setdefault('check', True)
    return subprocess.run(command, **kwargs)


def generate():
    run([sys.executable, str(ROOT / 'scripts/gen-muyu-gmu-source.py')])
    run([sys.executable, str(ROOT / 'scripts/gen-muyu-gpu-smmu-source.py')])


def check_constants(report):
    header = (BOARD / 'MuyuGmuSmmu.h').read_text()
    gmu = (BOARD / 'ShengGmu.c').read_text()
    generated = (GENERATED / 'ShengGpuSmmuSource.h').read_text()

    def define(name):
        match = re.search(r'^#define\s+' + name + r'\s+(UINT64_C\(\s*)?(0x[0-9a-fA-F]+|\d+)[uUlL]*',
                          header, re.M)
        if not match:
            raise SystemExit('MuyuGmuSmmu.h does not define ' + name)
        return int(match.group(2), 0)

    sizes = {name: define(name) for name in
             ('MUYU_GMU_ICACHE_BYTES', 'MUYU_GMU_DUMMY_BYTES', 'MUYU_GMU_DEBUG_BYTES',
              'MUYU_GMU_LOG_BYTES', 'MUYU_GMU_HFI_BYTES')}
    iovas = {name: define(name) for name in
             ('MUYU_GMU_ICACHE_IOVA', 'MUYU_GMU_DUMMY_IOVA', 'MUYU_GMU_LOG_IOVA',
              'MUYU_GMU_HFI_IOVA', 'MUYU_GMU_DEBUG_IOVA')}

    # ShengGmu.c's validate(): the five extents, in this order and with these sizes.
    validator = re.search(r'const size_t sizes\[\]=\{([^}]*)\}', gmu)
    if not validator:
        raise SystemExit('ShengGmu.c size table not found')
    wanted = [int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]+', validator.group(1))]
    ours = [sizes['MUYU_GMU_ICACHE_BYTES'], sizes['MUYU_GMU_DUMMY_BYTES'],
            sizes['MUYU_GMU_DEBUG_BYTES'], sizes['MUYU_GMU_LOG_BYTES'],
            sizes['MUYU_GMU_HFI_BYTES']]
    if wanted != ours:
        raise SystemExit('extent sizes disagree with ShengGmu.c: %s vs %s'
                         % ([hex(v) for v in wanted], [hex(v) for v in ours]))
    fixed = re.search(r'c->icache\.iova!=(\w+)\s*\|\|\s*c->dummy\.iova!=(\w+)\s*\|\|\s*'
                      r'c->debug\.iova!=(\w+)', gmu)
    if not fixed:
        raise SystemExit('ShengGmu.c fixed-IOVA check not found')
    for name, literal in zip(('icache', 'dummy', 'debug'), fixed.groups()):
        if int(literal, 16) != iovas['MUYU_GMU_%s_IOVA' % name.upper()]:
            raise SystemExit('%s IOVA disagrees with ShengGmu.c' % name)
    # log/hfi must stay inside the vendor driver's dynamic range and not overlap anything.
    ranges = sorted((iovas[iova], sizes[size]) for iova, size in
                    (('MUYU_GMU_ICACHE_IOVA', 'MUYU_GMU_ICACHE_BYTES'),
                     ('MUYU_GMU_DUMMY_IOVA', 'MUYU_GMU_DUMMY_BYTES'),
                     ('MUYU_GMU_LOG_IOVA', 'MUYU_GMU_LOG_BYTES'),
                     ('MUYU_GMU_HFI_IOVA', 'MUYU_GMU_HFI_BYTES'),
                     ('MUYU_GMU_DEBUG_IOVA', 'MUYU_GMU_DEBUG_BYTES')))
    for (iova, size), (next_iova, _) in zip(ranges, ranges[1:]):
        if iova + size > next_iova:
            raise SystemExit('extents overlap at %#x' % iova)
    for name in ('MUYU_GMU_LOG_IOVA', 'MUYU_GMU_HFI_IOVA'):
        if not 0x60001000 <= iovas[name] < 0x80000000:
            raise SystemExit(name + ' outside the vendor dynamic window')
    if max(iova + size for iova, size in ranges) >= 1 << 32:
        raise SystemExit('an extent leaves the 32-bit input space')

    base = int(re.search(r'#define SHENG_GPU_SMMU_BASE UINT64_C\((0x[0-9a-fA-F]+)\)',
                         generated).group(1), 16)
    size = int(re.search(r'#define SHENG_GPU_SMMU_BYTES (\d+)u', generated).group(1))
    sid = int(re.search(r'#define SHENG_GPU_SMMU_GMU_SID (\d+)u', generated).group(1))
    bank_min = int(re.search(r'#define SHENG_GPU_SMMU_GMU_BANK_MIN (\d+)u', generated).group(1))
    gpu_banks = int(re.search(r'#define SHENG_GPU_SMMU_GPU_BANKS (\d+)u', generated).group(1))
    if (base, size) != (0x3DA0000, 0x40000):
        raise SystemExit('unexpected GPU SMMU range from the generator')
    if sid != 5 or bank_min != gpu_banks or bank_min < 2:
        raise SystemExit('GMU stream/bank policy changed upstream')
    if define('MUYU_GMU_INPUT_BITS') != 32 or define('MUYU_GMU_OUTPUT_BITS') != 40:
        raise SystemExit('unexpected address widths')
    report['constants'] = dict(base=hex(base), bytes=size, gmu_sid=sid,
                               gmu_bank_min=bank_min, gpu_banks=gpu_banks,
                               sizes={k: hex(v) for k, v in sizes.items()},
                               iovas={k: hex(v) for k, v in iovas.items()})


def build_and_run(name, sources, includes, sanitize=True):
    binary = OUT / ('test-' + name)
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
    if sanitize:
        flags += ['-fsanitize=address,undefined']
    run(['xcrun', 'clang', *flags, *includes, *map(str, sources), '-o', str(binary)])
    result = run([str(binary)], capture_output=True, text=True)
    print(result.stdout, end='')
    if result.returncode:
        raise SystemExit(result.stderr)
    return result.stdout


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-generate', action='store_true')
    args = ap.parse_args(argv[1:])
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'report.json').unlink(missing_ok=True)
    if not args.no_generate:
        generate()
    report = {'passed': False}

    check_constants(report)

    includes = ['-I', str(BOARD), '-I', str(KIT), '-I', str(GENERATED)]
    kit_sources = [KIT / (name + '.c') for name in SMMU_KIT]

    # 2. The library lifecycle the plan drives.
    lifecycle = build_and_run('smmu-lifecycle',
                              kit_sources + [BOARD / 'ShengGpuSmmu.c',
                                             BOARD / 'ShengGpuSmmuHostTests.c'], includes)
    for marker in ('GPU/GMU lower/upper native4K lifecycle', 'live arenas'):
        if marker not in lifecycle:
            raise SystemExit('lifecycle test did not report ' + marker)
    counts = re.search(r'(\d+) startup and (\d+) stop I/O failures', lifecycle)

    # 3. The board plan.
    plan_sources = kit_sources + [BOARD / 'ShengGpuSmmu.c', BOARD / 'MuyuGmuSmmu.c',
                                  BOARD / 'MuyuGmuSmmuHostTests.c']
    plan = build_and_run('gmu-plan', plan_sources, includes)
    tables = re.search(r'PASS: (\d+) table pages', plan)
    if not tables or int(tables.group(1)) != 774:
        raise SystemExit('unexpected table pool size: ' + plan)
    failures = re.search(r'PASS: (\d+) startup and (\d+) map and (\d+) observe I/O failures', plan)
    if not failures or min(int(group) for group in failures.groups()) < 8:
        raise SystemExit('failure coverage too small: ' + plan)

    # 4. The kernel build is freestanding aarch64 with no SIMD; compile the board unit the
    #    same way the kernel does (and the probe includes it as plain C).
    kernel_flags = ['-target', 'arm64-apple-ios17.0', '-O2', '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-incompatible-sysroot', '-ffreestanding',
                    '-mgeneral-regs-only', *includes]
    for source in (BOARD / 'MuyuGmuSmmu.c', BOARD / 'ShengGpuSmmu.c'):
        run(['xcrun', 'clang', *kernel_flags, '-c', str(source),
             '-o', str(OUT / (source.stem + '.kernel.o'))])
    objdump = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin/llvm-objdump'
    for source in ('MuyuGmuSmmu', 'ShengGpuSmmu'):
        asm = subprocess.check_output([str(objdump), '-d', str(OUT / (source + '.kernel.o'))], text=True)
        operands = '\n'.join(re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+[0-9a-f]{8}[ \t]+\w+[ \t]+([^\n]+)', asm, re.M))
        if re.search(r'\b(?:[qds][0-9]+|v[0-9]+(?:\.[0-9a-z]+)?)\b', operands):
            raise SystemExit('SIMD found in ' + source)

    report.update(
        passed=True,
        lifecycle_startup_io_failures=int(counts.group(1)),
        lifecycle_stop_io_failures=int(counts.group(2)),
        plan_startup_io_failures=int(failures.group(1)),
        plan_map_io_failures=int(failures.group(2)),
        plan_observe_io_failures=int(failures.group(3)),
        table_pages=int(tables.group(1)),
        sources={str(path.relative_to(WORK)): sha(path) for path in
                 list(kit_sources) + [BOARD / name for name in
                                      ('ShengGpuSmmu.c', 'ShengGpuSmmuHostTests.c',
                                       'MuyuGmuSmmu.c', 'MuyuGmuSmmu.h',
                                       'MuyuGmuSmmuHostTests.c', 'ShengGmu.c')]},
        generated={str(path.relative_to(WORK)): sha(path) for path in
                   (GENERATED / 'ShengGmuSource.h', GENERATED / 'ShengGpuSmmuSource.h')},
        freestanding_aarch64=True, simd_free=True, kernel_integrated=False,
        active_hardware_mapping_qualified=False)
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: board constants agree with ShengGmu.c and the generated binding; '
          'library lifecycle and the GMU plan pass with software peers; freestanding aarch64, no SIMD')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
