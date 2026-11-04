#!/usr/bin/env python3
"""Derive the muyu (SM8635 cliffs, Adreno 735) GPU inputs from the live vendor DTB.

This is muyu's analogue of uke's `scripts/prepare-uke-gmu.py --derive`: read the board's own device
tree and record the facts the NativeAdreno profile needs, instead of copying another device's numbers.
What it derives (all from `fw/dtb_04.dtb`, the variant that carries the kgsl node with the chip id):

  * chip id, GPU model, compatible string        -> the profile selector
  * GPU / GMU / GMU-PDC / SMMU / GCC / GPUCC windows (base + size, from `reg` and `reg-names`)
  * the 11 `qcom,gpu-pwrlevels` entries (freq, bus-min/max index, ACD level) and `initial-pwrlevel`
  * the DDR/CNOC bandwidth tables
  * UBWC mode and minimum access length
  * the SMMU stream ids of `gfx3d_user` / `gfx3d_secure` / the GMU node
  * the DTB's sha256, so a later boot can prove the inputs still describe the same tree

The facts this DTB does *not* carry (GMEM size, GMU chip id/CGC mode, preemption record size, UBWC
swizzle/highest-bank-bit) stay null in `memory` -- that map is DTB-derived and only DTB-derived. They
are taken instead from `scripts/data/adreno-profiles.json`, a verbatim copy of the profile table in the
pinned NativeAdreno source, keyed by `qcom,chipid`: same chip id, same chip-level constants. The
derivation cross-checks the copy against the board's own windows (gmu_gpu_delta must equal
gmu_base - gpu_base) and against the UBWC mode and the firmware names, so a wrong entry cannot slip in.

    scripts/derive-muyu-gpu-inputs.py [--dtb fw/dtb_04.dtb] [--out scripts/data/muyu-gpu-inputs.json]

Host-only, no device I/O.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
DEFAULT_DTB = WORKSPACE / 'fw/dtb_04.dtb'
DEFAULT_OUT = ROOT / 'scripts/data/muyu-gpu-inputs.json'
PROFILES = ROOT / 'scripts/data/adreno-profiles.json'
GPU_NODE = '/soc/qcom,kgsl-3d0@3d00000'
GMU_NODE = '/soc/qcom,gmu@3d69000'
SMMU_NODE = '/soc/qcom,kgsl-iommu@3da0000'
PRESENT = {'version', 'dtb_sha256', 'soc', 'source'}


def probe(dtb):
    """(node, property, type) -> list of values; missing properties raise."""
    def fdtget(node, prop, kind):
        out = subprocess.run(['fdtget', '-t', kind, str(dtb), node, prop],
                             capture_output=True, text=True, check=True).stdout.strip()
        return [int(value, 16) for value in out.split()]
    return fdtget


def cells(dtb, node, prop):
    values = probe(dtb)(node, prop, 'x')
    return values


def one(dtb, node, prop):
    values = cells(dtb, node, prop)
    if len(values) != 1:
        raise SystemExit(f'{node}:{prop} has {len(values)} cells, expected 1')
    return values[0]


def text(dtb, node, prop):
    out = subprocess.run(['fdtget', '-t', 's', str(dtb), node, prop],
                         capture_output=True, text=True, check=True).stdout
    # fdtget -t s separates the strings of a string list with spaces.
    return out.split()


def try_props(dtb, node, names):
    """Decode the cells a node actually carries; None for the ones it does not."""
    result = {}
    for name in names:
        try:
            result[name] = cells(dtb, node, name)
        except subprocess.CalledProcessError:
            result[name] = None
    return result


def count_prop(dtb, node, prop, default):
    try:
        return one(dtb, node, prop)
    except (subprocess.CalledProcessError, SystemExit):
        return default


def windows(dtb, node):
    """{reg-name: {base, bytes}} for a node whose reg/reg-names line up.

    #address-cells/#size-cells are inherited from /soc on Qualcomm DTs (1/1 here), so rather than
    trust one pair, accept the stride that divides `reg` into exactly one window per reg-name.
    """
    reg = cells(dtb, node, 'reg')
    names = None
    try:
        names = text(dtb, node, 'reg-names')
    except subprocess.CalledProcessError:
        pass
    pairs = [(2, 2), (1, 1), (2, 1), (1, 2)]
    for address_cells, size_cells in pairs:
        stride = address_cells + size_cells
        if stride <= 0 or len(reg) % stride:
            continue
        if names is not None and len(reg) // stride != len(names):
            continue
        break
    else:
        raise SystemExit(f'{node}: cannot split {len(reg)} reg cells into windows with {len(names)} names')
    stride = address_cells + size_cells
    shapes = []
    for start in range(0, len(reg), stride):
        base = 0
        for cell in range(address_cells):
            base = (base << 32) | reg[start + cell]
        size = 0
        for cell in range(size_cells):
            size = (size << 32) | reg[start + address_cells + cell]
        shapes.append({'base': base, 'bytes': size})
    result = {}
    for index, shape in enumerate(shapes):
        result[names[index] if names and index < len(names) else f'window{index}'] = shape
    return result


def first_window(dtb, node):
    """The node's first reg window; clock controllers carry no reg-names."""
    if not node:
        return None
    return list(windows(dtb, node).values())[0]


def soc_node_with_compatible(dtb, compatible):
    listing = subprocess.run(['fdtget', '-l', str(dtb), '/soc'], capture_output=True, text=True,
                             check=True).stdout.split()
    for child in listing:
        node = f'/soc/{child}'
        try:
            if compatible in text(dtb, node, 'compatible'):
                return node
        except subprocess.CalledProcessError:
            continue
    return None


def smmu_stream(dtb, node):
    """The stream id an `iommus` entry names: <&apps_smmu SID MASK>, so the SID is the cell after the
    phandle. Reading the last cell instead returns the mask, which is 0 here for every GPU node and
    would have published three SIDs of zero."""
    try:
        values = cells(dtb, node, 'iommus')
    except subprocess.CalledProcessError:
        return None
    if len(values) < 2:
        return None
    return values[1] & 0xffff


def pwrlevels(dtb, node):
    listing = subprocess.run(['fdtget', '-l', str(dtb), node], capture_output=True, text=True,
                             check=True).stdout.split()
    entries = [child for child in listing if 'pwrlevel' in child]
    entries.sort(key=lambda child: int(child.rsplit('@', 1)[-1], 16))
    result = []
    for level, child in enumerate(entries):
        sub = f'{node}/{child}'
        try:
            freq = cells(dtb, sub, 'qcom,gpu-freq')[0]
        except subprocess.CalledProcessError:
            freq = 0
        entry = {'level': level, 'hz': freq}
        for key, mask in (('bus-min', 0xff), ('bus-max', 0xff), ('acd-level', None)):
            try:
                value = cells(dtb, sub, f'qcom,{key}')[0]
            except subprocess.CalledProcessError:
                continue
            entry[key.replace('-', '_')] = value if mask is None else value & mask
        result.append(entry)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dtb', type=Path, default=DEFAULT_DTB)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    dtb = args.dtb
    if not dtb.is_file():
        raise SystemExit(f'{dtb} is missing: pass --dtb with the vendor DTB that carries the kgsl node')

    compatible = text(dtb, GPU_NODE, 'compatible')
    model = text(dtb, GPU_NODE, 'qcom,gpu-model')[0]
    chip_id = one(dtb, GPU_NODE, 'qcom,chipid')
    gpucc = soc_node_with_compatible(dtb, 'qcom,cliffs-gpucc')
    gcc = soc_node_with_compatible(dtb, 'qcom,cliffs-gcc')

    gpu = windows(dtb, GPU_NODE)
    gmu = windows(dtb, GMU_NODE)
    smmu = windows(dtb, SMMU_NODE)
    gpucc_window = first_window(dtb, gpucc)
    gcc_window = first_window(dtb, gcc)

    opps = pwrlevels(dtb, f'{GPU_NODE}/qcom,gpu-pwrlevels')
    tables = try_props(dtb, GPU_NODE, ['qcom,bus-table-ddr', 'qcom,bus-table-cnoc'])
    opt = try_props(dtb, GPU_NODE, ['qcom,ubwc-mode', 'qcom,min-access-length', 'qcom,initial-pwrlevel'])

    profiles = json.loads(PROFILES.read_text()) if PROFILES.is_file() else {'profiles': {}}
    profile = profiles['profiles'].get(hex(chip_id))
    if profile is not None:
        delta = gmu['gmu']['base'] - gpu['kgsl_3d0_reg_memory']['base']
        if profile['gmu_gpu_delta'] != delta:
            raise SystemExit(f'{PROFILES}: gmu_gpu_delta 0x{profile["gmu_gpu_delta"]:x} does not match the '
                             f'DTB windows (gmu_base - gpu_base = 0x{delta:x})')
        if profile['ubwc_version'] != opt.get('qcom,ubwc-mode', [None])[0]:
            raise SystemExit(f'{PROFILES}: ubwc_version {profile["ubwc_version"]} does not match the DTB '
                             f'qcom,ubwc-mode {opt.get("qcom,ubwc-mode")}')

    record = {
        'version': 1,
        'source': 'derive-muyu-gpu-inputs.py --dtb ' + dtb.name,
        'dtb_sha256': hashlib.sha256(dtb.read_bytes()).hexdigest(),
        'soc': 'SM8635',
        'chip': {
            'chip_id': hex(chip_id),
            'model': model,
            'compatible': compatible[0],
            'family': 'ADRENO_7XX_GEN2',
        },
        'board': {
            'soc': 'SM8635 (cliffs)',
            'gpu': gpu,
            'gmu': gmu,
            'smmu': smmu,
            'gpucc': gpucc_window,
            'gcc': gcc_window,
            'sid_user': smmu_stream(dtb, f'{SMMU_NODE}/gfx3d_user'),
            'sid_secure': smmu_stream(dtb, f'{SMMU_NODE}/gfx3d_secure'),
            'sid_gmu': smmu_stream(dtb, GMU_NODE),
        },
        'opp': {
            'initial_pwrlevel': opt.get('qcom,initial-pwrlevel', [None])[0],
            'levels': opps,
            'bus_table_ddr': tables.get('qcom,bus-table-ddr'),
            'bus_table_cnoc': tables.get('qcom,bus-table-cnoc'),
        },
        'memory': {
            'ubwc_mode': opt.get('qcom,ubwc-mode', [None])[0],
            'min_access_length': opt.get('qcom,min-access-length', [None])[0],
            # Not in this DTB: the driver reads them from hardware, or uke's A732 profile is the
            # fallback once the read is confirmed on muyu. Deliberately null, never guessed.
            'gmem_bytes': None,
            'preempt_record_bytes': None,
            'ubwc_swizzle': None,
            'highest_bank_bit': None,
            'gmu_chip_id': None,
            'gmu_cgc_mode': None,
        },
        'firmware': {
            'sqe': 'gen71100_sqe.fw',
            'gmu': 'gen71100_gmu.bin',
            'zap': 'gen71100_zap.mbn',
            'note': 'uke ships byte-identical gen71100 SQE/GMU firmware; the ZAP image differs per device',
        },
        'profile': None if profile is None else {
            **{key: profile[key] for key in
               ('name', 'gmem_bytes', 'gmu_chip_id', 'gmu_cgc_mode', 'preempt_record_bytes',
                'ubwc_version', 'highest_bank_bit', 'ubwc_swizzle', 'gmu_gpu_delta',
                'firmware', 'acd', 'preemption', 'ifpc', 'catalog_note')},
            'source': {'file': 'scripts/data/adreno-profiles.json', **profiles['source']},
        },
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(record, indent=2) + '\n')

    print(f'{args.out}: {model} chip {record["chip"]["chip_id"]} {compatible[0]}')
    print('  windows:', ', '.join(f'{name}@0x{shape["base"]:x}+0x{shape["bytes"]:x}'
                                  for name, shape in {**gpu, **gmu}.items()))
    print(f'  opps: {len(opps)} levels, top {max(o["hz"] for o in opps) / 1e6:.0f} MHz, '
          f'initial_pwrlevel={record["opp"]["initial_pwrlevel"]}')
    print(f'  gpucc 0x{gpucc_window["base"]:x} gcc 0x{gcc_window["base"]:x}'
          if gpucc_window and gcc_window else '  gpucc/gcc node missing')
    print('  memory nulls:', ', '.join(k for k, v in record['memory'].items() if v is None))
    if profile is None:
        print(f'  profile: none pinned for {hex(chip_id)} (memory stays null, nothing is guessed)')
    else:
        print(f'  profile: {profile["name"]} from {profiles["source"]["file"]} '
              f'@ {profiles["source"]["commit"][:12]} '
              f'(gmem 0x{profile["gmem_bytes"]:x}, gmu chip id 0x{profile["gmu_chip_id"]:x})')


if __name__ == '__main__':
    main()
