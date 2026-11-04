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
    mode) must stay null in `memory`: filling them there by guesswork is the mistake this check exists
    to prevent. The driver-facing copy of those values comes from `scripts/data/adreno-profiles.json`
    (a verbatim pin of the upstream profile table) and is checked against the board's own windows here:
    gmu_gpu_delta must equal gmu_base - gpu_base, and the firmware names must be the ones this board
    ships.

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
    'gpucc': 0x3d90000, 'gcc': 0x100000, 'sids': (0, 2, 5),
    'opps': 9, 'top_hz': 950000000, 'initial': 8,
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

sids = (board['sid_user'], board['sid_secure'], board['sid_gmu'])
require(sids == UKE['sids'],
        f'the three SMMU stream ids are {sids}, the same 0/2/5 uke uses '
        f'(a decoded mask would have made them all 0)')

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

profile = record.get('profile')
require(profile is not None, 'the pinned chip profile was merged into the record')
if profile:
    require(profile['name'] == 'Adreno732', 'the profile is the A732 entry for chip 0x43030b00')
    require(profile['gmu_gpu_delta'] ==
            board['gmu']['gmu']['base'] - board['gpu']['kgsl_3d0_reg_memory']['base'],
            'profile gmu_gpu_delta equals the DTB window distance (gmu_base - gpu_base)')
    require(profile['ubwc_version'] == memory['ubwc_mode'],
            'profile ubwc_version equals the DTB ubwc mode')
    require(profile['firmware']['sqe'] == record['firmware']['sqe'] and
            profile['firmware']['gmu'] == record['firmware']['gmu'] and
            profile['firmware']['zap'] == record['firmware']['zap'],
            'profile firmware names match the gen71100 files this board ships')
    require(profile['gmem_bytes'] == 0x180000 and profile['gmu_chip_id'] == 0x70b0000 and
            profile['preempt_record_bytes'] == 0x225000,
            'profile carries 1.5 MiB GMEM, gmu chip id 0x70b0000 and a 2196 KiB preemption record')
    require(profile['ubwc_swizzle'] == 6 and profile['highest_bank_bit'] == 16,
            'profile carries UBWC swizzle 6 / highest bank bit 16')
    require(profile['acd'] and profile['preemption'] and not profile['ifpc'],
            'profile enables ACD and preemption, keeps IFPC off (OEM gen7_11_0 behaviour)')
    source = profile['source']
    require(source['repository'].endswith('NativeAdreno.git') and len(source['commit']) == 40 and
            len(source['sha256']) == 64,
            'the profile records its repository, commit and file digest')

if FIRMWARE.is_dir():
    import hashlib
    for name, prefix in (('gen71100_sqe.fw', UKE['sqe_sha']), ('gen71100_gmu.bin', UKE['gmu_sha'])):
        path = FIRMWARE / name
        digest = hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else ''
        require(digest.startswith(prefix), f'{name} is byte-identical to uke\'s ({prefix})')
else:
    print(f'skip {FIRMWARE} is not present')

# board.toml's [gpu] table is generated from the record above; drift between the two is what a hand-edit
# looks like, so the generator's own check runs here.
table = subprocess.run([sys.executable, str(ROOT / 'scripts/gen-muyu-gpu-table.py'), '--check'],
                       capture_output=True, text=True)
require(table.returncode == 0, (table.stdout + table.stderr).strip() or 'board.toml [gpu] matches the inputs')

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
