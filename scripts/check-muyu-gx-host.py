#!/usr/bin/env python3
"""Check muyu's GX power-up unit (package/MuyuGx.h) without touching hardware.

Three independent checks:
  1. every register offset and bit position agrees with a6xx_gmu.xml (A7XX variant of
     GMU_SPTPRAC_PWR_CLK_STATUS, the three interrupt registers) and with the vendor
     driver's OOB table in fw/android-harvest/msm-drv/a6xx_gmu.c;
  2. the C unit compiles and its software-register peer passes on the host;
  3. the same header still compiles as freestanding AArch64.

Exits 1 on any failure.
"""
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "UniXNU-muyu"
HEADER = BOARD / "package/MuyuGx.h"
TESTS = BOARD / "package/MuyuGxHostTests.c"
XML = ROOT / "fw/android-harvest/msm-regs/a6xx_gmu.xml"
DRIVER = ROOT / "fw/android-harvest/msm-drv/a6xx_gmu.c"
NS = {"r": "http://nouveau.freedesktop.org/"}


def cc():
    return shutil.which("clang") or "/usr/bin/clang"


def header_defines():
    out = {}
    for line in HEADER.read_text().splitlines():
        m = re.match(r"#define\s+(MUYU_GX_[A-Z0-9_]+)\s+(.+)", line)
        if not m:
            continue
        value = m.group(2).split("//")[0].strip().strip("()").strip()
        shift = re.match(r"1u\s*<<\s*(\d+)", value)
        if shift:
            out[m.group(1)] = 1 << int(shift.group(1))
            continue
        hexa = re.match(r"(0x[0-9a-fA-F]+)u?", value)
        if hexa:
            out[m.group(1)] = int(hexa.group(1), 16)
    return out


def xml_registers():
    tree = ET.parse(XML).getroot()
    out = {}
    for domain in tree.findall("r:domain", NS):
        for reg in domain.findall("r:reg32", NS):
            name = reg.attrib.get("name")
            variant = reg.attrib.get("variants")
            if name in ("GMU_GMU2HOST_INTR_INFO", "GMU_GMU2HOST_INTR_CLR",
                        "GMU_HOST2GMU_INTR_SET"):
                out[name] = int(reg.attrib["offset"], 0)
            if name == "GMU_SPTPRAC_PWR_CLK_STATUS" and variant == "A7XX":
                out[name] = int(reg.attrib["offset"], 0)
                out[name + ".bits"] = {f.attrib["name"]: int(f.attrib["pos"])
                                       for f in reg.findall("r:bitfield", NS)}
    return out


def main():
    failures = []
    d = header_defines()
    x = xml_registers()
    expect = {
        "MUYU_GX_REG_PWR_STATUS": ("GMU_SPTPRAC_PWR_CLK_STATUS", x.get("GMU_SPTPRAC_PWR_CLK_STATUS")),
        "MUYU_GX_REG_INTR_CLR": ("GMU_GMU2HOST_INTR_CLR", x.get("GMU_GMU2HOST_INTR_CLR")),
        "MUYU_GX_REG_INTR_INFO": ("GMU_GMU2HOST_INTR_INFO", x.get("GMU_GMU2HOST_INTR_INFO")),
        "MUYU_GX_REG_INTR_SET": ("GMU_HOST2GMU_INTR_SET", x.get("GMU_HOST2GMU_INTR_SET")),
    }
    for macro, (reg, value) in expect.items():
        if value is None:
            failures.append(f"{reg} missing from {XML.name}")
        elif d.get(macro) != value:
            failures.append(f"{macro} {d.get(macro):#x} != {reg} {value:#x}")
    bits = x.get("GMU_SPTPRAC_PWR_CLK_STATUS.bits", {})
    if bits.get("GX_HM_GDSC_POWER_OFF") != 0 or bits.get("GX_HM_CLK_OFF") != 1:
        failures.append(f"A7XX GX_HM bits changed: {bits}")
    if d.get("MUYU_GX_STATUS_GX_OFF") != 0x3:
        failures.append("MUYU_GX_STATUS_GX_OFF is not bits 0|1")
    # gx_clkctl_gx_gdsc: DT reg 0x3d68024 is byte 0x24 of the GMU core window (0x3d68000),
    # which under the word*4 - CORE_GPU_DELTA(0x68000) convention is dword 0x1a009.
    expected_dword = (0x3d68024 - 0x3d68000 + 0x68000) // 4
    if d.get("MUYU_GX_REG_CLKCTL_GDSC") != expected_dword:
        failures.append(f"MUYU_GX_REG_CLKCTL_GDSC {d.get('MUYU_GX_REG_CLKCTL_GDSC'):#x} != "
                        f"{expected_dword:#x}")
    dtb = ROOT / "fw/stock/dtb_3.dtb"
    try:
        reg = [int(v, 16) for v in subprocess.check_output(
            ["fdtget", "-t", "x", str(dtb), "/soc/qcom,gdsc@3d68024", "reg"], text=True).split()]
        if reg[0] != 0x3d68024:
            failures.append(f"DT gx_clkctl_gdsc reg is {reg[0]:#x}")
        name = subprocess.check_output(
            ["fdtget", "-t", "s", str(dtb), "/soc/qcom,gdsc@3d68024", "regulator-name"],
            text=True).strip()
        if name != "gx_clkctl_gx_gdsc":
            failures.append(f"DT regulator-name is {name!r}")
    except (OSError, subprocess.CalledProcessError):
        failures.append("DT gx_clkctl_gx_gdsc node missing")

    # Vendor OOB table: non-legacy GPU_SET is set_new=30 / ack_new=31 / clear_new=31.
    driver = DRIVER.read_text()
    gpu_set = re.search(r"\[GMU_OOB_GPU_SET\] = \{(.*?)\},", driver, re.S)
    if not gpu_set:
        failures.append("GMU_OOB_GPU_SET not found in the vendor OOB table")
    else:
        body = gpu_set.group(1)
        fields = {k: int(v) for k, v in re.findall(r"\.(set|ack|set_new|ack_new|clear|clear_new) = (\d+)", body)}
        want = dict(set_new=30, ack_new=31, clear_new=31)
        for k, v in want.items():
            if fields.get(k) != v:
                failures.append(f"OOB {k}={fields.get(k)} != {v}")
        if d.get("MUYU_GX_OOB_SET") != 1 << want["set_new"]:
            failures.append("MUYU_GX_OOB_SET is not 1<<30")
        if d.get("MUYU_GX_OOB_ACK") != 1 << want["ack_new"]:
            failures.append("MUYU_GX_OOB_ACK is not 1<<31")
        if d.get("MUYU_GX_OOB_CLEAR") != 1 << want["clear_new"]:
            failures.append("MUYU_GX_OOB_CLEAR is not 1<<31")
    if not failures:
        print(f"  register/bits agree with a6xx_gmu.xml and the vendor OOB table "
              f"(GPU_SET set_new=30 ack_new=31)")

    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "muyu-gx"
        subprocess.run([cc(), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        f"-I{BOARD / 'package'}", str(TESTS), "-o", str(exe)],
                       check=True, capture_output=True, text=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if run.returncode:
            failures.append("host GX tests failed: " + run.stderr.strip())
        else:
            print("  " + run.stdout.strip())

    # The unit is header-only, so compile a tiny consumer freestanding for AArch64.
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "gx-use.c"
        src.write_text('#include "MuyuGx.h"\n'
                       'MuyuGxResult gx_use(const MuyuGxIo *io) { return muyu_gx_power_on(io, 100); }\n')
        subprocess.run([cc(), "-target", "aarch64-none-elf", "-ffreestanding", "-std=c11",
                        "-fsyntax-only", f"-I{BOARD / 'package'}", str(src)],
                       check=True, capture_output=True, text=True)
    print("  freestanding aarch64 syntax check clean")

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("PASS: GX OOB GPU_SET unit is register-accurate, host-tested and kernel-compilable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
