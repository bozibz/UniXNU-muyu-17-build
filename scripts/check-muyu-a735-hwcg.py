#!/usr/bin/env python3
"""Verify the A735 hwcg table and its application code.

  - the generated header still matches the harvested catalog/XMLs (re-runs the generator with --check)
  - the C module compiles and the host tests pass (ASan/UBSan)

usage: check-muyu-a735-hwcg.py [--no-compile]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / "UniXNU-muyu/package"
GEN = ROOT / "UniXNU-muyu/scripts/gen-muyu-a735-hwinit.py"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-compile", action="store_true")
    args = ap.parse_args(argv[1:])

    fails = []
    rc = subprocess.run([sys.executable, str(GEN), "--check"], capture_output=True, text=True)
    print(rc.stdout.strip() or rc.stderr.strip())
    if rc.returncode != 0:
        fails.append("generated header is stale")

    if not args.no_compile:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "a735-hwcg"
            cmd = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(PKG),
                   str(PKG / "MuyuA735Hwcg.c"), str(PKG / "MuyuA735HwcgHostTests.c"),
                   "-o", str(exe)]
            build = subprocess.run(cmd, capture_output=True, text=True)
            if build.returncode != 0:
                fails.append("host compile failed:\n" + build.stderr)
            else:
                run = subprocess.run([str(exe)], capture_output=True, text=True)
                print(run.stdout.strip())
                if run.returncode != 0:
                    fails.append("host test failed:\n" + run.stdout + run.stderr)

    print("A735 hwcg:", "OK" if not fails else "FAIL")
    for f in fails:
        print("  -", f)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
