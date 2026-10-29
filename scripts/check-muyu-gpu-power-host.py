#!/usr/bin/env python3
"""Host test for muyu's gated GPU power observation sequence (package/MuyuGpuPowerBoard.h).

Compiles package/MuyuGpuPowerBoardTests.c against a software register peer and runs it. Nothing here
touches hardware: it proves the gating (no GPUCC read while gcc_gpu_cfg_ahb is disabled, no SMMU read
before CX is on and its vote clock runs, no GPU read before GX is on), that mode 1 never writes, and
that mode 2 makes exactly the one write Linux's gcc_cliffs probe makes.

usage: check-muyu-gpu-power-host.py [--cc clang]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args(argv[1:])
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "muyu-gpu-power"
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                        "-fsanitize=address,undefined",
                        "-I", str(ROOT / "package"),
                        str(ROOT / "package/MuyuGpuPowerBoardTests.c"), "-o", str(exe)], check=True)
        print(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
