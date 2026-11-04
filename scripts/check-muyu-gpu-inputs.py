#!/usr/bin/env python3
"""Host check: muyu's derived GPU inputs still describe the board we measured.

Pins what `scripts/derive-muyu-gpu-inputs.py` produced from `fw/dtb_04.dtb` and what uke's accepted
Adreno 732 profile says, so a drift in either place fails here instead of on the tablet:

  * the identity must stay Adreno735 / 0x43030b00 / qcom,adreno-gpu-gen7-11-0 (the same chip id uke
    records for its A732, which is why their profile is the starting point);
  * the register windows must match uke's A732 table exactly (GPU 0x3d00000+0x40000,
    GMU 0x3d68000+0x37000, GMU PDC 0xb280000+0x10000, GPUCC 0x3d90000, GCC 0x100000);
  * the OPP table is where the two boards differ: 11 levels topping out at 1.1 GHz here, 9 levels at
    950 MHz there, with initial_pwrlevel pointing at the slowest level (10 here, 8 there);
  * the SQE and GMU firmware are byte-identical to uke's, so only the ZAP image is board-specific;
  * the values this DTB does not carry (GMEM, preemption record, UBWC swizzle/HBB, GMU chip id/CGC
    mode) must stay null: filling them by guesswork is the mistake this check exists to prevent.

    scripts/check-muyu-gpu-inputs.py
"""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
RECORD = ROOT / 'scripts/data/muyu-gpu-inputs.json'
DERIVE = ROOT / 'scripts/derive-muyu-gpu-inputs.py'
DTB = WORKSPACE / 'fw/dtb_04.dtb'
FIRMWARE = WORKSPACE / 'fw/gpu-firmware'

# uke's accepted Adreno 732 profile (docs/GPU-PORT.md at 2026-11-03), for the comparison table.
UKE = {
    'chip_id': '0x43030b00', 'model': 'Adreno732',
    'gpu': (0x3d00000, 0x40000), 'gmu': (0x3d68000, 0x37000), 'gmu_pdc': (0xb280000, 0x10000),
    'gpucc': 0x3d90000, 'gcc': 0x100000, 'opps': 9, 'top_hz': 950000000, 'initial': 8,
    'sqe_sha': 'b6df660d', 'gmu_sha': '435c7bac',
}
FAIL = []


def require(condition, message):
    print(('ok   ' if condition else 'FAIL ') + message)
    if not condition:
        FAIL.append(message)


record = json.loads(RECORD.read_text())
chip, board, opp, memory = record['chip'], record['board'], record['opp'], record['memory']

require(chip['model'] == 'Adreno735' and chip['chip_id'] == '0x43030b00',
        f'identity is Adreno735 / 0x43030b00 (got {chip["model"]} / {chip["chip_id"]})')
require(chip['compatible'] == 'qcom,adreno-gpu-gen7-11-0', 'compatible is the gen7-11-0 string')
require(chip['chip_id'] == UKE['chip_id'],
        "chip id equals uke's A732 profile, so their profile is the right starting point")

# The three register windows live in different derived maps: the GPU's own node, the GMU node, and
# the GMU PDC window that is a child range of the GMU node.
windows = {
    'gpu': board['gpu']['kgsl_3d0_reg_memory'],
    'gmu': board['gmu']['gmu'],
    'gmu_pdc': board['gmu']['gmu_pdc'],
}
for name, window in windows.items():
    got = (window['base'], window['bytes'])
    require(got == UKE[name], f'{name} window {got} matches uke ({UKE[name]})')

require(board['gpucc']['base'] == UKE['gpucc'] and board['gcc']['base'] == UKE['gcc'],
        f'GPUCC 0x{board["gpucc"]["base"]:x} / GCC 0x{board["gcc"]["base"]:x} match uke')

levels = opp['levels']
require(len(levels) == 11, f'11 OPP levels (got {len(levels)})')
require(max(level['hz'] for level in levels) == 1100000000,
        'top OPP is 1.1 GHz (uke tops at 950 MHz, so the tables are not interchangeable)')
require(opp['initial_pwrlevel'] == len(levels) - 1 == 10,
        'initial_pwrlevel points at the slowest level (10)')
require([level['hz'] for level in levels][:3] == [1100000000, 1000000000, 950000000],
        'first three OPPs are 1100/1000/950 MHz')
require(memory['ubwc_mode'] == 4, 'UBWC mode 4 (same as uke)')
require(all(memory[key] is None for key in
            ('gmem_bytes', 'preempt_record_bytes', 'ubwc_swizzle', 'highest_bank_bit',
             'gmu_chip_id', 'gmu_cgc_mode')),
        'facts this DTB does not carry stay null (no guessed values)')

if FIRMWARE.is_dir():
    import hashlib
    for name, prefix in (('gen71100_sqe.fw', UKE['sqe_sha']), ('gen71100_gmu.bin', UKE['gmu_sha'])):
        path = FIRMWARE / name
        digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else ''
        require(digest.startswith(prefix), f'{name} is byte-identical to uke\'s ({prefix})')
else:
    print(f'skip {FIRMWARE} is not present')

if DTB.is_file():
    with tempfile.TemporaryDirectory() as temporary:
        fresh = Path(temporary) / 'muyu-gpu-inputs.json'
        subprocess.run([sys.executable, str(DERIVE), '--dtb', str(DTB), '--out', str(fresh)],
                       check=True, capture_output=True)
        require(json.loads(fresh.read_text()) == record, 'the committed record re-derives from the DTB')
else:
    print(f'skip {DTB} is not present')

print()
print('%-14s %-16s %-16s' % ('field', 'muyu / A735', 'uke / A732'))
for label, mine, theirs in (
        ('top OPP', '1100 MHz', '%d MHz' % (UKE['top_hz'] // 1000000)),
        ('OPP levels', str(len(levels)), str(UKE['opps'])),
        ('initial_pwrlevel', str(opp['initial_pwrlevel']), str(UKE['initial'])),
        ('chip id', chip['chip_id'], UKE['chip_id']),
        ('gpu window', '0x3d00000+0x40000', '0x3d00000+0x40000'),
        ('gmu window', '0x3d68000+0x37000', '0x3d68000+0x37000'),
        ('gpucc / gcc', '0x3d90000 / 0x100000', '0x3d90000 / 0x100000'),
        ('sqe/gmu firmware', 'identical', 'identical'),
        ('zap firmware', 'board-specific', 'board-specific')):
    print('%-14s %-16s %-16s' % (label, mine, theirs))
sys.exit(1 if FAIL else 0)
