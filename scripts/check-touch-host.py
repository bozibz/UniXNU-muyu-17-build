#!/usr/bin/env python3
"""Host tests for the touch stack: GENI SPI register model, NVT protocol/chip model, touch core, board helpers.

Run without the cross-compiler on PATH. These are model tests; none touches hardware.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # the kit (UniXNU-muyu)
REPO = ROOT.parent                                  # the workspace root
# The GENI SPI host library lives in the main checkout, not in the kit: this script used to assume the sheng
# layout (drivers/QcomGeniSpi inside the kit) and failed before it reached the touch cases.
GENI = REPO / 'UniXNU/drivers/QcomGeniSpi'
FW = ROOT / '.cache/firmware/novatek/novatek_nt36532_n81a_fw_csot.bin'
CASES = [
    ('geni-spi', [GENI / 'Test/geni-spi.c', GENI / 'GeniSpi.c', '-I', str(GENI)], []),
    ('nvt-protocol', [ROOT / 'drivers/ShengNovatekTouch/Test/nvt-protocol.c',
                      ROOT / 'drivers/ShengNovatekTouch/NvtProtocol.c'],
     [str(FW)] if FW.is_file() else []),
    ('touch-core', [ROOT / 'drivers/ShengNovatekTouch/Test/touch-core.c',
                    ROOT / 'drivers/ShengNovatekTouch/NvtTouchCore.c'], []),
    ('touch-board', [ROOT / 'package/TouchBoardTests.c'], []),
]

with tempfile.TemporaryDirectory() as tmp:
    for name, sources, args in CASES:
        exe = Path(tmp) / name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined',
                        *[str(s) for s in sources], '-o', str(exe)], check=True)
        out = subprocess.run([str(exe), *args], check=True, capture_output=True, text=True).stdout.strip().splitlines()
        print(f'{name}: {out[-1]}')
print('PASS: touch host tests (register/chip models only)')
