#!/usr/bin/env python3
"""Verify muyu's generated GMU OPP/BCM policy without touching hardware.

Four independent checks:

  1. the checked-in build output is exactly what the generator produces now (no drift,
     no stale header after the DT or the catalog changed);
  2. the generated table equals an independent re-derivation straight from
     fw/stock/dtb_3.dtb (fdtget, not the generator's own code path);
  3. the shared ShengGmuVotes.c compiles and its logic accepts this board's table under
     every GMxC/ACD/IFPC profile (host build + run of MuyuGmuVotesHostTests.c);
  4. the same sources still compile as freestanding AArch64 for the kernel archive.

Any failure exits 1. Nothing here reads or writes a device.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "UniXNU-muyu"
OUT = BOARD / "build/gpu"
DTB = ROOT / "fw/stock/dtb_3.dtb"
CATALOG = ROOT / "fw/android-harvest/msm-drv/a7xx_catalog.c"
KIT = ROOT / "UniXNU/kits/AdrenoFirmware"
GPU_NODE = "/soc/qcom,kgsl-3d0@3d00000"
GMU_NODE = "/soc/qcom,gmu@3d69000"
CMD_DB_NODE = "/reserved-memory/aop_cmd_db_region@81c60000"


def cc():
    return shutil.which("clang") or "/usr/bin/clang"


def fdtget(*args):
    return subprocess.check_output(["fdtget", *args], text=True).split()


def cells(path, prop):
    return [int(v, 16) for v in fdtget("-t", "x", str(DTB), path, prop)]


def one(path, prop):
    values = cells(path, prop)
    if len(values) != 1:
        raise SystemExit(f"{path}/{prop}: expected one cell")
    return values[0]


def parse_header(path):
    text = Path(path).read_text()

    def array(name):
        body = re.search(rf"static const \w+ {name}\[\]=\{{(.*?)\}};", text, re.S)
        if not body:
            raise SystemExit(f"{name} missing from {path}")
        return [tuple(int(v) for v in row.split(",")) for row in
                re.findall(r"\{([0-9,]+)\}", body.group(1))]

    defines = {k: int(v, 0) for k, v in
               re.findall(r"#define (SHENG_[A-Z0-9_]+) ([0-9a-fA-Fx]+)u?", text)}
    return dict(defines=defines, gpu=array("sheng_gpu_opps"), gmu=array("sheng_gmu_opps"),
                bandwidths=[v[0] for v in array("sheng_gpu_bandwidths")] if False else None,
                text=text)


def independent_expectations():
    """Re-derive the table from the board DT with a separate code path."""
    table = GPU_NODE + "/qcom,gpu-pwrlevels"
    bus = cells(GPU_NODE, "qcom,bus-table-ddr")
    children = sorted(fdtget("-l", str(DTB), table), key=lambda n: int(n.rsplit("@", 1)[1]))
    gpu = []
    for index, child in enumerate(children):
        node = f"{table}/{child}"
        if one(node, "reg") != index:
            raise SystemExit(f"{child}: reg mismatch")
        props = fdtget("-p", str(DTB), node)
        gpu.append((one(node, "qcom,gpu-freq"), one(node, "qcom,level"),
                    bus[one(node, "qcom,bus-freq")],
                    1 if "qcom,acd-level" in props else 0,
                    one(node, "qcom,acd-level") if "qcom,acd-level" in props else 0))
    gpu.sort()
    pairs = cells(GMU_NODE, "qcom,gmu-freq-table")
    gmu = sorted((pairs[i], pairs[i + 1], 0, 0, 0) for i in range(0, len(pairs), 2))
    bandwidths = sorted({0} | {row[2] for row in gpu})   # the off/0 entry comes first
    return dict(chip=one(GPU_NODE, "qcom,chipid"),
                model=fdtget("-t", "s", str(DTB), GPU_NODE, "qcom,gpu-model")[0],
                gpu=[(0, 0, 0, 0, 0)] + gpu,
                gmu=[(0, 0, 0, 0, 0)] + gmu,
                bandwidths=bandwidths, bus=bus,
                cmd_db=(lambda r: ((r[0] << 32) | r[1], (r[2] << 32) | r[3]))(
                    cells(CMD_DB_NODE, "reg")))


def main():
    failures = []

    # 1. generator determinism / no stale output.
    if not OUT.exists():
        raise SystemExit("build/gpu missing; run scripts/gen-muyu-gmu-votes.py first")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([sys.executable, str(BOARD / "scripts/gen-muyu-gmu-votes.py"),
                        "--out", tmp, "--dtb", str(DTB), "--catalog", str(CATALOG)],
                       check=True, capture_output=True, text=True)
        for name in ("ShengGmuVotesSource.h", "ShengGmuCommandDbSource.h"):
            if (Path(tmp) / name).read_bytes() != (OUT / name).read_bytes():
                failures.append(f"{name} is stale; re-run gen-muyu-gmu-votes.py")
    print("  generator deterministic, build/gpu up to date" if not failures else "  STALE")

    # 2. independent re-derivation.
    want = independent_expectations()
    got = parse_header(OUT / "ShengGmuVotesSource.h")
    if got["defines"]["SHENG_GMU_VOTES_CHIP_ID"] != want["chip"]:
        failures.append("chip id differs from the DT")
    if got["gpu"] != want["gpu"]:
        failures.append("GPU OPP rows differ from the DT")
    if got["gmu"] != want["gmu"]:
        failures.append("GMU OPP rows differ from the DT")
    m = re.search(r"sheng_gpu_bandwidths\[\]=\{([0-9,]*)\}", got["text"])
    if [int(v) for v in m.group(1).split(",")] != want["bandwidths"]:
        failures.append("bandwidth list differs from the DT")
    db = (OUT / "ShengGmuCommandDbSource.h").read_text()
    base = int(re.search(r"SHENG_GMU_COMMAND_DB_BASE UINT64_C\((0x[0-9a-f]+)\)", db).group(1), 16)
    size = int(re.search(r"SHENG_GMU_COMMAND_DB_BYTES (\d+)u", db).group(1))
    version = int(re.search(r"SHENG_GMU_COMMAND_DB_VERSION (\d+)u", db).group(1))
    if (base, size) != want["cmd_db"]:
        failures.append(f"command-db reservation {base:#x}+{size:#x} differs from the DT")
    if version != 1:
        failures.append("command-db version is not the pinned board value 1")
    if not failures:
        print(f"  DT re-derivation matches: {want['model']} {want['chip']:#x}, "
              f"{len(want['gpu']) - 1} GPU OPPs, {len(want['gmu']) - 1} GMU OPPs, "
              f"{len(want['bandwidths'])} bandwidths, cmd-db {base:#x}+{size:#x}")

    # 3. host logic test.
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "muyu-votes"
        subprocess.run([cc(), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-DMUYU_VOTES_STANDALONE",
                        f"-I{BOARD / 'package'}", f"-I{KIT}", f"-I{OUT}",
                        str(BOARD / "package/ShengGmuVotes.c"), str(KIT / "AdrenoHfiVotes.c"),
                        str(KIT / "AdrenoHfiStartup.c"), str(KIT / "AdrenoHfi.c"),
                        str(BOARD / "package/MuyuGmuVotesHostTests.c"), "-o", str(exe)],
                       check=True, capture_output=True, text=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if run.returncode:
            failures.append("host vote tests failed: " + run.stderr.strip())
        else:
            print("  " + run.stdout.strip())

    # 4. freestanding AArch64 syntax check (the kernel/archive path).
    subprocess.run([cc(), "-target", "aarch64-none-elf", "-ffreestanding", "-std=c11",
                    "-fsyntax-only", f"-I{BOARD / 'package'}", f"-I{KIT}", f"-I{OUT}",
                    str(BOARD / "package/ShengGmuVotes.c")],
                   check=True, capture_output=True, text=True)
    print("  freestanding aarch64 syntax check clean")

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: muyu GMU OPP/BCM policy is generated, DT-consistent, host-tested and kernel-compilable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
