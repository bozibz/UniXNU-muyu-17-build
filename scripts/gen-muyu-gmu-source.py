#!/usr/bin/env python3
"""Generate muyu's GMU cold-start source header (ShengGmuSource.h equivalent) from pinned inputs.

ShengGmu.c is board-agnostic but includes "ShengGmuSource.h", a generated file that
sheng produces from a pinned Linux msm checkout plus sheng's own DTB. This script is
the muyu counterpart. Inputs, all local:

  fw/android-harvest/msm-regs/a6xx.xml, a6xx_gmu.xml   register offsets (torvalds/linux)
  fw/android-harvest/msm-drv/a6xx_gmu.c               a6xx_gmu_rpmh_init RSC/PDC sequence
  fw/stock/dtb_3.dtb                                   the five windows, in enum order
  fw/gpu-firmware/gen71100_gmu.bin                     A735 GMU firmware (size + sha256)

Everything is derived, not typed: the register offsets come from the XML, the
RSC/PDC write sequence is evaluated out of the driver for a non-A740 gen7 GPU
(pdc_in_aop, so the PDC uCode block is skipped, exactly as Linux does for a7xx),
and the windows come from muyu's devicetree.

The Adreno chip id is the one DT publishes (qcom,chipid 0x43030b00). The *firmware*
chip id is not a separate value on this GPU: Linux only uses .gmu_chipid when a
catalog entry provides one, and otherwise massages the GPU chip id into the GMU's
packed layout (a6xx_gmu.c, a6xx_gpu_gpu_start's write of REG_A6XX_GMU_GENERAL_10).
This script applies that same expression and records it in the report, so a wrong
guess surfaces as an early FW_INIT_RESULT failure rather than a silent one.

usage: gen-muyu-gmu-source.py [--out DIR] [--json PATH]
"""
import argparse
import ast
import hashlib
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NS = {"r": "http://nouveau.freedesktop.org/"}
WINDOW_ORDER = ["gmu", "rscc", "gmu_pdc", "kgsl_3d0_reg_memory", "cx_misc"]
WINDOW_ENUM = ["ShengGmuCore", "ShengGmuRscc", "ShengGmuPdc", "ShengGmuGpu", "ShengGmuCxMisc"]

# domain/name/variant triples: (symbol, domain, name, variant or None)
REGISTERS = [
    ("REG_GMU_CM3_ITCM_START", "A6XX", "GMU_CM3_ITCM_START", None),
    ("REG_GMU_CM3_DTCM_START", "A6XX", "GMU_CM3_DTCM_START", None),
    ("REG_GMU_CM3_SYSRESET", "A6XX", "GMU_CM3_SYSRESET", None),
    ("REG_GMU_CM3_BOOT_CONFIG", "A6XX", "GMU_CM3_BOOT_CONFIG", None),
    ("REG_GMU_CM3_FW_INIT_RESULT", "A6XX", "GMU_CM3_FW_INIT_RESULT", None),
    ("REG_GMU_CM3_CFG", "A6XX", "GMU_CM3_CFG", None),
    ("REG_GMU_HFI_QTBL_ADDR", "A6XX", "GMU_HFI_QTBL_ADDR", None),
    ("REG_GMU_HFI_QTBL_INFO", "A6XX", "GMU_HFI_QTBL_INFO", None),
    ("REG_GMU_HFI_CTRL_INIT", "A6XX", "GMU_HFI_CTRL_INIT", None),
    ("REG_GMU_HFI_CTRL_STATUS", "A6XX", "GMU_HFI_CTRL_STATUS", None),
    ("REG_GMU_HOST2GMU_INTR_SET", "A6XX", "GMU_HOST2GMU_INTR_SET", None),
    ("REG_GMU_SYS_BUS_CONFIG", "A6XX", "GMU_SYS_BUS_CONFIG", None),
    ("REG_GMU_ICACHE_CONFIG", "A6XX", "GMU_ICACHE_CONFIG", None),
    ("REG_GMU_DCACHE_CONFIG", "A6XX", "GMU_DCACHE_CONFIG", None),
    ("REG_GMU_AHB_FENCE_RANGE_0", "A6XX", "GMU_AHB_FENCE_RANGE_0", None),
    ("REG_GMU_GMU2HOST_INTR_INFO", "A6XX", "GMU_GMU2HOST_INTR_INFO", None),
    ("REG_GMU_AO_HOST_INTERRUPT_STATUS", "A6XX", "GMU_AO_HOST_INTERRUPT_STATUS", None),
    ("REG_GPU_GMU_CX_GMU_CX_FALNEXT_INTF", "A6XX", "GPU_GMU_CX_GMU_CX_FALNEXT_INTF", "A6XX"),
    ("REG_GPU_GMU_CX_GMU_CX_FAL_INTF", "A6XX", "GPU_GMU_CX_GMU_CX_FAL_INTF", "A6XX"),
    ("REG_GMU_GENERAL_8", "A6XX", "GMU_GENERAL_8", "A7XX"),
    ("REG_GMU_GENERAL_9", "A6XX", "GMU_GENERAL_9", "A7XX"),
    ("REG_GMU_GENERAL_10", "A6XX", "GMU_GENERAL_10", "A7XX"),
    ("REG_GPU_RSCC_RSC_STATUS0_DRV0", "A6XX", "GPU_RSCC_RSC_STATUS0_DRV0", None),
    ("REG_RSCC_PDC_SLAVE_ID_DRV0", "A6XX", "RSCC_PDC_SLAVE_ID_DRV0", None),
    ("REG_RSCC_HIDDEN_TCS_CMD0_ADDR", "A6XX", "RSCC_HIDDEN_TCS_CMD0_ADDR", None),
    ("REG_RSCC_HIDDEN_TCS_CMD0_DATA", "A6XX", "RSCC_HIDDEN_TCS_CMD0_DATA", None),
    ("REG_RSCC_OVERRIDE_START_ADDR", "A6XX", "RSCC_OVERRIDE_START_ADDR", None),
    ("REG_RSCC_PDC_SEQ_START_ADDR", "A6XX", "RSCC_PDC_SEQ_START_ADDR", None),
    ("REG_RSCC_PDC_MATCH_VALUE_LO", "A6XX", "RSCC_PDC_MATCH_VALUE_LO", None),
    ("REG_RSCC_PDC_MATCH_VALUE_HI", "A6XX", "RSCC_PDC_MATCH_VALUE_HI", None),
    ("REG_RSCC_SEQ_BUSY_DRV0", "A6XX", "RSCC_SEQ_BUSY_DRV0", None),
    ("REG_RSCC_SEQ_MEM_0_DRV0", "A6XX", "RSCC_SEQ_MEM_0_DRV0", None),
    ("REG_RSCC_SEQ_MEM_0_DRV0_A740", "A6XX", "RSCC_SEQ_MEM_0_DRV0_A740", "A7XX"),
    ("REG_CX_MISC_TCM_RET_CNTL", "A6XX_CX_MISC", "CX_MISC_TCM_RET_CNTL", "A7XX-"),
    ("REG_GBIF_QSB_SIDE0", "A6XX", "GBIF_QSB_SIDE0", None),
    ("REG_PDC_GPU_SEQ_START_ADDR", "A6XX_PDC", "GPU_SEQ_START_ADDR", None),
    ("REG_PDC_GPU_ENABLE_PDC", "A6XX_PDC", "GPU_ENABLE_PDC", None),
]
FATAL_AO = ("WDOG_BITE", "FENCE_ERR", "HOST_AHB_BUS_ERROR")
FATAL_CM3 = "CM3_FAULT"

# a6xx_gmu.c conditions on this board: A735 is gen7 like A7xx but not the A740 family.
CONDITIONS = {
    "adreno_is_a740_family(adreno_gpu)": False,
    "adreno_is_a722(adreno_gpu)": False,
    "adreno_is_a618(adreno_gpu)": False,
    "adreno_is_a619(adreno_gpu)": False,
    "adreno_is_a650_family(adreno_gpu)": False,
    "adreno_is_a8xx(adreno_gpu)": False,
}


def parse_xmls(paths):
    trees = [ET.parse(p).getroot() for p in paths]
    regs = {}

    def reg32(domain, name, variant=None):
        matches = []
        for tree in trees:
            for d in tree.findall("r:domain", NS):
                if d.attrib["name"] != domain:
                    continue
                matches += [n for n in d.findall("r:reg32", NS)
                            if n.attrib.get("name") == name
                            and (variant is None or n.attrib.get("variants") == variant)]
        if len(matches) != 1:
            raise SystemExit(f"ambiguous or missing register {domain}/{name}/{variant}")
        return int(matches[0].attrib["offset"], 0)

    for symbol, domain, name, variant in REGISTERS:
        regs[symbol] = reg32(domain, name, variant)

    def bitfield(domain, name, field):
        for tree in trees:
            for d in tree.findall("r:domain", NS):
                if d.attrib["name"] != domain:
                    continue
                for reg in d.findall("r:reg32", NS):
                    if reg.attrib.get("name") != name:
                        continue
                    for f in reg.findall("r:bitfield", NS):
                        if f.attrib.get("name") == field:
                            return 1 << int(f.attrib["pos"])
        raise SystemExit(f"missing bitfield {domain}/{name}/{field}")

    ao = sum(bitfield("A6XX", "GMU_AO_HOST_INTERRUPT_STATUS", b) for b in FATAL_AO)
    cm3 = bitfield("A6XX", "GMU_GMU2HOST_INTR_INFO", FATAL_CM3)
    return regs, ao, cm3


def _c_ternary_to_python(expression):
    """Rewrite `cond ? a : b` (with nesting and parentheses) into a Python conditional."""
    depth = 0
    q = None
    for i, ch in enumerate(expression):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "?" and depth == 0:
            q = i
            break
    if q is None:
        return expression
    depth = 0
    colon = None
    for i in range(q + 1, len(expression)):
        ch = expression[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == ":" and depth == 0:
            colon = i
            break
    if colon is None:
        raise SystemExit("unbalanced ternary: " + expression)
    cond = expression[:q].strip()
    while cond.startswith("(") and cond.endswith(")"):
        cond = cond[1:-1].strip()
    then = _c_ternary_to_python(expression[q + 1:colon].strip())
    other = _c_ternary_to_python(expression[colon + 1:].strip())
    return f"(({then}) if ({cond}) else ({other}))"


def evaluate(expression, regs, extra=None):
    """Evaluate one C expression from rpmh_init for this board (A735: gen7, not A740 family)."""
    expression = expression.strip().rstrip(",")
    extra = dict(extra or {})
    for name, value in CONDITIONS.items():
        expression = expression.replace(name, "1" if value else "0")
    expression = re.sub(r"BIT\((\d+)\)", lambda m: str(1 << int(m[1])), expression)
    expression = expression.replace("seqmem0_drv0_reg", str(extra.get("seqmem", 0)))
    for symbol, value in regs.items():
        expression = re.sub(r"\bREG_A6XX_" + symbol.replace("REG_", "", 1) + r"\b", str(value), expression)
        expression = re.sub(r"\b" + symbol + r"\b", str(value), expression)
    expression = expression.replace("true", "1").replace("false", "0")
    expression = _c_ternary_to_python(expression)
    expression = expression.replace("||", " or ").replace("&&", " and ")
    tree = ast.parse(expression, mode="eval")
    allowed = (ast.Expression, ast.Constant, ast.BinOp, ast.BoolOp, ast.UnaryOp, ast.IfExp,
               ast.Add, ast.Sub, ast.Mult, ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd, ast.Invert,
               ast.And, ast.Or, ast.Not, ast.USub)
    if any(not isinstance(n, allowed) for n in ast.walk(tree)):
        raise SystemExit("unsupported expression: " + expression)
    return int(eval(compile(tree, "<rpmh_init>", "eval"), {"__builtins__": {}}))


def parse_setup(driver, regs):
    text = Path(driver).read_text()
    start = text.index("static void a6xx_gmu_rpmh_init(")
    end = text.index("\n}\n", start)
    body = text[start:end]
    if "if (adreno_is_a800(adreno_gpu))" in body or "adreno_is_a8xx" in body.split("pdc_in_aop")[0]:
        # Not fatal: a8xx returns before touching RSC; A735 is not a8xx and the branch below proves it.
        pass
    # A7xx keeps the PDC tables in AOP: Linux jumps straight to setup_pdc.
    if "pdc_in_aop = true" not in body or "goto setup_pdc" not in body:
        raise SystemExit("rpmh_init no longer skips the PDC uCode for a7xx")
    rsc_part = body[body.index("/* Disable SDE clock gating */"):body.index("/* The second spin")]
    # A735 is a7xx, so the first branch of the sequencer-uCode if/else is the one that runs.
    seq_if = body[body.index("/* Load RSC sequencer uCode"):body.index("if (pdc_in_aop)", body.index("/* Load RSC sequencer uCode"))]
    if "adreno_is_a7xx(adreno_gpu)" not in seq_if:
        raise SystemExit("rpmh_init no longer branches on a7xx for the RSC sequencer")
    seq_part = seq_if[seq_if.index("gmu_write_rscc"):seq_if.index("} else {")]
    pdc_part = body[body.index("setup_pdc:"):]

    setup = []
    for expr, value in re.findall(r"gmu_write_rscc\(gmu,\s*([^,]+),\s*([\s\S]*?)\);", rsc_part):
        setup.append((1, 4 * evaluate(expr, regs), evaluate(value, regs)))
    seqmem = regs["REG_RSCC_SEQ_MEM_0_DRV0"]
    for expr, value in re.findall(r"gmu_write_rscc\(gmu,\s*([^,]+),\s*([^;]+)\);", seq_part):
        setup.append((1, 4 * evaluate(expr, regs, {"seqmem": seqmem}), evaluate(value, regs)))
    for expr, value in re.findall(r"pdc_write\(pdcptr,\s*([^,]+),\s*([^;]+)\);", pdc_part):
        setup.append((2, 4 * evaluate(expr, regs), evaluate(value, regs)))
    if len(setup) != 19:
        raise SystemExit(f"rpmh_init now produces {len(setup)} writes, ShengGmu.c expects 19")
    for window, offset, value in setup:
        if window not in (1, 2):
            raise SystemExit("RSC/PDC write targets an unexpected window")
        if offset % 4 or not 0 <= value <= 0xFFFFFFFF:
            raise SystemExit(f"generated setup outside a 32-bit register: {window} {offset:#x} {value:#x}")
    return setup


def gpu_chip_id(dtb, windows):
    text = subprocess.run([sys.executable, str(Path(__file__).with_name("gen-muyu-gpu-platform.py")),
                           "--dtb", str(dtb)], capture_output=True, text=True)
    if text.returncode:
        raise SystemExit(text.stderr)
    platform = json.loads((ROOT / "UniXNU-muyu/build/gpu/muyu-gpu-platform.json").read_text())
    if [w["name"] for w in platform["windows"]] != WINDOW_ORDER:
        raise SystemExit("unexpected window order in the platform JSON")
    # qcom,chipid lives on the kgsl node; the generated JSON records the model only, so read it directly.
    import struct
    data = dtb.read_bytes()
    magic, _total, off_struct, off_strings = struct.unpack_from(">4sIII", data, 0)
    if magic != b"\xd0\x0d\xfe\xed":
        raise SystemExit("not a dtb")
    pos, stack, chip = off_struct, [], None

    def name_at(off):
        e = data.index(b"\0", off_strings + off)
        return data[off_strings + off:e].decode()

    while pos + 4 <= len(data):
        tok, = struct.unpack_from(">I", data, pos)
        pos += 4
        if tok == 1:
            e = data.index(b"\0", pos)
            stack.append(data[pos:e].decode())
            pos = (e + 4) & ~3
        elif tok == 2:
            stack.pop()
        elif tok == 3:
            length, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            blob = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
            if name_at(nameoff) == "qcom,chipid" and blob:
                chip = struct.unpack(">I", blob[:4])[0]
        elif tok == 9:
            break
    if chip is None:
        raise SystemExit("DT has no qcom,chipid")
    return chip, platform


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "UniXNU-muyu/build/gpu"))
    ap.add_argument("--dtb", default=str(ROOT / "fw/stock/dtb_3.dtb"))
    ap.add_argument("--regs", default=str(ROOT / "fw/android-harvest/msm-regs"))
    ap.add_argument("--driver", default=str(ROOT / "fw/android-harvest/msm-drv/a6xx_gmu.c"))
    ap.add_argument("--firmware", default=str(ROOT / "fw/gpu-firmware/gen71100_gmu.bin"))
    args = ap.parse_args(argv[1:])

    regs_dir = Path(args.regs)
    regs, ao_mask, cm3_mask = parse_xmls([regs_dir / "a6xx.xml", regs_dir / "a6xx_gmu.xml"])
    setup = parse_setup(args.driver, regs)
    chip, platform = gpu_chip_id(Path(args.dtb), None)
    windows = [(w["base"], w["size"]) for w in platform["windows"]]
    delta = windows[0][0] - windows[3][0]
    firmware = Path(args.firmware).read_bytes()
    # GMU packs major<<16 | minor<<12 | patch<<8; Linux derives it from the GPU chip id
    # when the catalog has no .gmu_chipid (a6xx_gmu.c, the GENERAL_10 write).
    fw_chip = chip & 0xFFFF0000
    fw_chip |= (chip << 4) & 0xF000
    fw_chip |= (chip << 8) & 0x0F00

    for window, offset, _value in setup:
        if offset + 4 > windows[window][1]:
            raise SystemExit(f"setup write 0x{offset:x}+4 exceeds window {window} "
                             f"(0x{windows[window][1]:x})")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    defines = "\n".join(f"#define {k} UINT32_C({v:#x})" for k, v in regs.items())
    text = "/* Generated by UniXNU-muyu/scripts/gen-muyu-gmu-source.py. Do not commit. */\n"
    text += "#include <stdint.h>\n#include <stddef.h>\n" + defines + "\n"
    text += f"#define SHENG_GMU_CHIP_ID UINT32_C({chip:#x})\n"
    text += f"#define SHENG_GMU_FW_CHIP_ID UINT32_C({fw_chip:#x})\n"
    text += f"#define SHENG_GMU_CORE_GPU_DELTA {delta:#x}u\n"
    text += f"#define SHENG_GMU_FIRMWARE_BYTES {len(firmware)}u\n"
    text += f"#define SHENG_GMU_AO_FATAL_MASK {ao_mask:#x}u\n#define SHENG_GMU_CM3_FATAL_MASK {cm3_mask:#x}u\n"
    text += ("static const uint8_t sheng_gmu_firmware_sha256[32]={"
             + ",".join(f"0x{b:02x}" for b in hashlib.sha256(firmware).digest()) + "};\n")
    text += ("static const uint64_t sheng_gmu_window_physical[5]={"
             + ",".join(f"{b:#x}" for b, _ in windows) + "};\n")
    text += ("static const size_t sheng_gmu_window_bytes[5]={"
             + ",".join(f"{s:#x}" for _, s in windows) + "};\n")
    text += "typedef struct {unsigned window;size_t byte_offset;uint32_t value;} ShengGmuSetupWrite;\n"
    text += f"#define SHENG_GMU_RSC_SETUP_COUNT {len(setup)}u\n"
    text += f"static const ShengGmuSetupWrite sheng_gmu_rsc_setup[{len(setup)}]={{\n"
    text += "\n".join(f"{{{w},{o:#x},{v:#x}}}," for w, o, v in setup) + "\n};\n"
    header = out / "ShengGmuSource.h"
    header.write_text(text)
    # The firmware bytes as a C object, so the in-kernel probe can stage the same image the generator
    # hashed. Generated, gitignored, and only built when the probe is in the archive.
    fw_c = out / "MuyuGmuFirmware.c"
    lines = ["/* Generated by gen-muyu-gmu-source.py. Do not commit. */",
             "#include <stddef.h>",
             f"const unsigned char muyu_gmu_firmware[{len(firmware)}] = {{"]
    for i in range(0, len(firmware), 16):
        lines.append("  " + ",".join(f"0x{b:02x}" for b in firmware[i:i + 16]) + ",")
    lines += ["};", f"const size_t muyu_gmu_firmware_size = {len(firmware)};", ""]
    fw_c.write_text("\n".join(lines))
    report = dict(gpu_chip_id=f"{chip:#x}", gmu_fw_chip_id=f"{fw_chip:#x}",
                  firmware=dict(bytes=len(firmware), sha256=hashlib.sha256(firmware).hexdigest()),
                  core_gpu_delta=f"{delta:#x}", windows=[{"enum": e, "name": n, "base": f"{b:#x}", "bytes": f"{s:#x}"}
                                                         for e, n, (b, s) in zip(WINDOW_ENUM, WINDOW_ORDER, windows)],
                  rsc_setup=[{"window": w, "byte_offset": f"{o:#x}", "value": f"{v:#x}"} for w, o, v in setup],
                  registers={k: f"{v:#x}" for k, v in regs.items()},
                  fatal_masks=dict(ao=f"{ao_mask:#x}", cm3=f"{cm3_mask:#x}"),
                  inputs={str(p): hashlib.sha256(Path(p).read_bytes()).hexdigest()
                          for p in (args.dtb, args.driver, args.firmware,
                                    regs_dir / "a6xx.xml", regs_dir / "a6xx_gmu.xml")},
                  generated_header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                  generated_firmware_object=dict(path=str(fw_c), bytes=fw_c.stat().st_size,
                                                 sha256=hashlib.sha256(fw_c.read_bytes()).hexdigest()),
                  source_analysis_only=True, hardware_registers_observed=False)
    (out / "muyu-gmu-source-report.json").write_text(json.dumps(report, indent=2) + "\n")

    for name, value in regs.items():
        if name in ("REG_GMU_CM3_ITCM_START", "REG_GMU_CM3_DTCM_START", "REG_GMU_CM3_SYSRESET",
                    "REG_GMU_CM3_FW_INIT_RESULT", "REG_GMU_HFI_CTRL_STATUS", "REG_GMU_GENERAL_10"):
            print(f"  {name:34s} {value:#x}")
    print(f"chip id {chip:#x}, gmu fw chip id {fw_chip:#x}, delta {delta:#x}")
    print(f"RSC/PDC setup writes: {len(setup)} (first {{win,off,val}} = {setup[0]})")
    print(f"wrote {header} and {out / 'muyu-gmu-source-report.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
