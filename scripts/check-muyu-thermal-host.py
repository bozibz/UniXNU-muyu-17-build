#!/usr/bin/env python3
"""Verify muyu's TSENS survey without touching hardware.

Three independent checks:

  1. the board DT dump (fw/muyu_dt.txt) still describes the controllers and the sensor map
     the probe hard-codes - base addresses, sensor counts, and which tsens/sensor id each
     cpu-*/gpuss-*/ddr thermal zone draws from;
  2. the decode and the panel row pass their host tests (package/MuyuThermalHostTests.c);
  3. the same header still compiles freestanding for AArch64, which is the archive path.

Nothing here reads or writes a device. Exit status 1 on any failure.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "UniXNU-muyu"
PACKAGE = BOARD / "package"
DT = ROOT / "fw/muyu_dt.txt"

BLOCK_NAMES = ["t0", "t1", "t2"]

# What fw/muyu_dt.txt says, re-derived from the dump rather than from the header: the zones that
# matter for a thermal policy and the (controller, sensor id) each one draws from.
EXPECTED_ZONES = {
    "cpu-0-0-0": ("tsens1", 1),
    "cpu-0-1-0": ("tsens1", 2),
    "cpu-0-2-0": ("tsens1", 3),
    "cpu-1-0-0": ("tsens0", 3),
    "cpu-2-0-2": ("tsens0", 13),
    "gpuss-0": ("tsens1", 8),
    "gpuss-3": ("tsens1", 11),
    "ddr": ("tsens1", 13),
}

EXPECTED_CONTROLLERS = {  # (tm base, srot base, sensors)
    "tsens0": (0x0C228000, 0x0C222000, 14),
    "tsens1": (0x0C229000, 0x0C223000, 16),
    "tsens2": (0x0C22A000, 0x0C224000, 5),
}


def cc():
    return shutil.which("clang") or "/usr/bin/clang"


def dt_controllers():
    """Return {name: (tm, srot, sensors, phandle)} for every soc/tsensN@<addr> node."""
    text = DT.read_text().splitlines()
    out = {}
    name = None
    for line in text:
        m = re.match(r"^soc/(tsens\d)@([0-9a-f]+)\s*$", line)
        if m:
            name = m.group(1)
            out[name] = {"addr": int(m.group(2), 16)}
            continue
        if name is None:
            continue
        stripped = line.strip()
        if stripped.startswith("reg "):
            words = stripped.split()[-1]
            cells = [int(words[i:i + 8], 16) for i in range(0, len(words), 8)]
            if len(cells) < 4:
                raise SystemExit(f"{name}: reg should be <TM addr, size, SROT addr, size>")
            out[name]["tm"], out[name]["srot"] = cells[0], cells[2]
        elif stripped.startswith("#qcom,sensors "):
            out[name]["sensors"] = int(stripped.split()[-1], 16)
        elif stripped.startswith("phandle "):
            out[name]["phandle"] = int(stripped.split()[-1], 16)
            name = None
        elif name and re.match(r"^soc/", stripped):
            name = None
    return out


def dt_zones(phandles):
    """Return {zone: (controller name, sensor id)} for the zones with a tsens provider."""
    text = DT.read_text().splitlines()
    zones = {}
    zone = None
    for line in text:
        m = re.match(r"^thermal-zones/([^\s/]+)\s*$", line)
        if m:
            zone = m.group(1)
            continue
        if zone and line.strip().startswith("thermal-sensors "):
            value = line.split()[-1].strip("<>")
            try:
                cells = [int(value[i:i + 8], 16) for i in range(0, len(value), 8)]
            except ValueError:
                zone = None
                continue
            if len(cells) >= 2 and cells[0] in phandles:
                zones[zone] = (phandles[cells[0]], cells[1])
            zone = None
    return zones


def header_blocks():
    """Parse kMuyuTsensBlocks out of package/MuyuThermal.h."""
    text = (PACKAGE / "MuyuThermal.h").read_text()
    body = re.search(r"kMuyuTsensBlocks\[MUYU_THERM_BLOCKS\] = \{(.*?)\n\};", text, re.S)
    if not body:
        raise SystemExit("kMuyuTsensBlocks missing from MuyuThermal.h")
    blocks = []
    for row in re.findall(r'\{"(\w+)",\s*(0x[0-9a-f]+)ull,\s*(0x[0-9a-f]+)ull,\s*(\d+),'
                          r'\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(0x[0-9a-f]+|\d+)u?\}',
                          body.group(1)):
        name, tm, srot, sensors, cpu_first, cpu_last, gpu_first, gpu_last, ddr = row
        blocks.append(dict(name=name, tm=int(tm, 16), srot=int(srot, 16),
                           sensors=int(sensors), cpu_first=int(cpu_first),
                           cpu_last=int(cpu_last), gpu_first=int(gpu_first),
                           gpu_last=int(gpu_last), ddr=int(ddr, 0)))
    return blocks


def main():
    failures = []

    controllers = dt_controllers()
    phandles = {info.get("phandle"): name for name, info in controllers.items()}
    for name, (tm, srot, sensors) in EXPECTED_CONTROLLERS.items():
        info = controllers.get(name)
        if not info:
            failures.append(f"{name} missing from {DT.name}")
            continue
        if (info.get("tm"), info.get("srot"), info.get("sensors")) != (tm, srot, sensors):
            failures.append(f"{name}: DT has {info.get('tm'):#x}/{info.get('srot'):#x}/"
                            f"{info.get('sensors')}, expected {tm:#x}/{srot:#x}/{sensors}")

    zones = dt_zones(phandles)
    for zone, want in EXPECTED_ZONES.items():
        got = zones.get(zone)
        if got != want:
            failures.append(f"thermal zone {zone}: DT says {got}, expected {want}")

    blocks = header_blocks()
    if [b["name"] for b in blocks] != BLOCK_NAMES:
        failures.append(f"header block list {[b['name'] for b in blocks]} != {BLOCK_NAMES}")
    for name, (tm, srot, sensors) in EXPECTED_CONTROLLERS.items():
        match = next((b for b in blocks if b["name"] == name), None)
        if not match:
            continue
        if (match["tm"], match["srot"], match["sensors"]) != (tm, srot, sensors):
            failures.append(f"header {name}: {match['tm']:#x}/{match['srot']:#x}/"
                            f"{match['sensors']} disagrees with the DT")
    # The CPU/GPU/DDR ids the header rolls up have to be the ones the DT hands those zones.
    header = {b["name"]: b for b in blocks}
    if header["t1"]["cpu_first"] != zones["cpu-0-0-0"][1] or \
       header["t1"]["cpu_last"] != zones["cpu-0-2-0"][1]:
        failures.append("header t1 CPU range disagrees with the cpu-0-* zones")
    if header["t0"]["cpu_first"] > zones["cpu-1-0-0"][1] or \
       header["t0"]["cpu_last"] < zones["cpu-2-0-2"][1]:
        failures.append("header t0 CPU range does not cover the cpu-1-*/cpu-2-* zones")
    if (header["t1"]["gpu_first"], header["t1"]["gpu_last"]) != \
       (zones["gpuss-0"][1], zones["gpuss-3"][1]):
        failures.append("header t1 GPU range disagrees with the gpuss-* zones")
    if header["t1"]["ddr"] != zones["ddr"][1]:
        failures.append("header t1 DDR id disagrees with the ddr zone")

    if not failures:
        print(f"  DT map matches: {len(controllers)} controllers, "
              f"{len(zones)} tsens-backed thermal zones")

    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "muyu-thermal"
        subprocess.run([cc(), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        f"-I{PACKAGE}", str(PACKAGE / "MuyuThermalHostTests.c"),
                        "-o", str(exe)], check=True, capture_output=True, text=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        if run.returncode:
            failures.append("host tests failed: " + run.stderr.strip())
        else:
            print("  " + run.stdout.strip())

        # Freestanding AArch64: the header must not need libc, only a snprintf declaration.
        probe = Path(tmp) / "freestanding.c"
        probe.write_text(
            "#include <stdint.h>\n#include <stddef.h>\n"
            "int snprintf(char *, size_t, const char *, ...);\n"
            '#include "MuyuThermal.h"\n'
            "int f(void) { return (int)muyu_tsens_status_to_mc(0x2000FAu); }\n")
        subprocess.run([cc(), "-target", "aarch64-none-elf", "-ffreestanding", "-std=c11",
                        "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                        f"-I{PACKAGE}", str(probe)], check=True, capture_output=True, text=True)
        print("  freestanding aarch64 syntax check clean")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("PASS: muyu TSENS survey is DT-consistent, host-tested and kernel-compilable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
