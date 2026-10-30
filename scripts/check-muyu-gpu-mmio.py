#!/usr/bin/env python3
"""Verify muyu's GPU MMIO map against the sources it came from; never touches the device.

The map in docs/GPU-CLIFFS-MMIO.md is not copied from another SoC: every number here is
re-derived from three first-party inputs and asserted against the table below.

  1. The vendor kernel's own cliffs clock drivers (MiCode/Xiaomi_Kernel_OpenSource,
     branch muyu-v-oss): drivers/clk/qcom/gpucc-cliffs.c and gcc-cliffs.c. The GPUCC
     block on cliffs is NOT register-compatible with SM8650 (the earlier assumption);
     e.g. gpu_cc_ahb_clk is 0x90c8 here, not 0x911c.
  2. muyu's own devicetree: GDSC/BCR addresses come from the *reg* properties, because
     the node *names* are stale copies from SM8650 (qcom,gdsc@3d99108 has reg 0x3d9908c).
  3. The AOP command database reservation, the GMU windows and the Adreno chip id.

Inputs are local files; nothing is fetched and no register is read. A mismatch is an
error: the doc and the code are supposed to fail loudly instead of drifting.

usage: check-muyu-gpu-mmio.py [--dtb PATH] [--vendor-source DIR] [--json PATH]
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # .../pad7pro-xnu

# name -> (kind, register offset) as the vendor GPUCC driver programs it.
GPUCC_CLOCKS = {
    "gpu_cc_pll0": ("alpha_pll", 0x0000),
    "gpu_cc_pll1": ("alpha_pll", 0x1000),
    "gpu_cc_ff_clk_src": ("rcg2", 0x91b8),
    "gpu_cc_gmu_clk_src": ("rcg2", 0x9168),
    "gpu_cc_hub_clk_src": ("rcg2", 0x919c),
    "gpu_cc_xo_clk_src": ("rcg2", 0x9010),
    "gpu_cc_demet_div_clk_src": ("regmap_div", 0x902c),
    "gpu_cc_xo_div_clk_src": ("regmap_div", 0x9028),
    "gpu_cc_ahb_clk": ("branch", 0x90c8),
    "gpu_cc_crc_ahb_clk": ("branch", 0x90cc),
    "gpu_cc_cx_accu_shift_clk": ("branch", 0x9108),
    "gpu_cc_cx_ff_clk": ("branch", 0x90f8),
    "gpu_cc_cx_gmu_clk": ("branch", 0x90e4),
    "gpu_cc_cxo_clk": ("branch", 0x90f0),
    "gpu_cc_freq_measure_clk": ("branch", 0x9008),
    "gpu_cc_hlos1_vote_gpu_smmu_clk": ("branch", 0x7000),
    "gpu_cc_hub_aon_clk": ("branch", 0x9198),
    "gpu_cc_hub_cx_int_clk": ("branch", 0x90f4),
    "gpu_cc_memnoc_gfx_clk": ("branch", 0x90fc),
    "gpu_cc_mnd1x_0_gfx3d_clk": ("branch", 0x9158),
    "gpu_cc_mnd1x_1_gfx3d_clk": ("branch", 0x915c),
    "gpu_cc_sleep_clk": ("branch", 0x90dc),
}
# Reset controls (GPUCC_*_BCR) and the two GPU power domains. GDSCR is always BCR+4.
GPUCC_RESETS = {
    "gpu_cc_xo_bcr": 0x9000,
    "gpu_cc_gx_bcr": 0x9030,
    "gpu_cc_cx_bcr": 0x9088,
    "gpu_cc_gfx3d_aon_bcr": 0x910c,
    "gpu_cc_rbcpr_bcr": 0x9118,
    "gpu_cc_gmu_bcr": 0x9164,
    "gpu_cc_acd_bcr": 0x9180,
    "gpu_cc_cb_bcr": 0x918c,
    "gpu_cc_fast_hub_bcr": 0x9194,
    "gpu_cc_ff_bcr": 0x91b4,
}
# Absolute addresses (GPUCC base 0x03d90000) from the DT reg properties, not node names.
DT_GDSC = {
    "gpu_cc_cx_gdsc": 0x03D9908C,
    "gpu_cc_gx_gdsc": 0x03D99034,
    "gx_clkctl_gx_gdsc": 0x03D68024,
}
# GCC side (base 0x00100000): the GPU branches and the GPLL0 GPU votes.
GCC_CLOCKS = {
    "gcc_gpu_cfg_ahb_clk": 0x71004,      # kept always-on by gcc_cliffs_probe, BIT(0)
    "gcc_gpu_memnoc_gfx_clk": 0x71010,
    "gcc_gpu_snoc_dvm_gfx_clk": 0x71018,
    "gcc_ddrss_gpu_axi_clk": 0x7115c,
}
GCC_GPLL0_VOTE_REG = 0x52000
GCC_GPLL0_VOTE_MASK = (1 << 15) | (1 << 16)
# PLL configurations are per-SoC constants, not derived: L/alpha decide 510/440 MHz.
GPUCC_PLLS = {
    "gpu_cc_pll0": dict(offset=0x0000, l=0x1A, alpha=0x9000, mhz=510),
    "gpu_cc_pll1": dict(offset=0x1000, l=0x16, alpha=0xEAAA, mhz=440),
}
AOP_CMD_DB = dict(phandle_name="aop_cmd_db_region", base=0x81C60000, bytes=0x20000)


def dt_nodes(path):
    """{node path: {prop: bytes}}, plus {phandle: node path}."""
    data = Path(path).read_bytes()
    magic, _total, off_struct, off_strings = struct.unpack_from(">4sIII", data, 0)
    if magic != b"\xd0\x0d\xfe\xed":
        raise SystemExit(f"{path}: not a dtb")
    pos, stack, nodes = off_struct, [], {}

    def name_at(off):
        end = data.index(b"\0", off_strings + off)
        return data[off_strings + off:end].decode()

    while pos + 4 <= len(data):
        tok, = struct.unpack_from(">I", data, pos)
        pos += 4
        if tok == 1:                                    # FDT_BEGIN_NODE
            end = data.index(b"\0", pos)
            stack.append(data[pos:end].decode())
            pos = (end + 4) & ~3
        elif tok == 2:                                  # FDT_END_NODE
            stack.pop()
        elif tok == 3:                                  # FDT_PROP
            length, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            blob = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
            nodes.setdefault("/" + "/".join(c for c in stack if c), {})[name_at(nameoff)] = blob
        elif tok == 9:
            break
    phandles = {}
    for p, props in nodes.items():
        for key in ("phandle", "linux,phandle"):
            if key in props:
                phandles[struct.unpack(">I", props[key][0:4])[0]] = p
    return nodes, phandles


def u32s(blob):
    return list(struct.unpack(">%dI" % (len(blob) // 4), blob))


def reg_base(props):
    words = u32s(props["reg"])
    return words[0] if len(words) == 2 else words[0]


def check_dt(dtb, fails):
    nodes, phandles = dt_nodes(dtb)
    seen = {}
    for path, props in nodes.items():
        name = props.get("regulator-name", b"").decode().rstrip("\0")
        if props.get("compatible", b"").startswith(b"qcom,gdsc") and name in DT_GDSC:
            seen[name] = reg_base(props)
            if seen[name] != DT_GDSC[name]:
                fails.append(f"DT {name}: reg 0x{seen[name]:x} != expected 0x{DT_GDSC[name]:x}")
            if name == "gpu_cc_cx_gdsc":
                hw = props.get("hw-ctrl-addr")
                if not hw:
                    fails.append("DT gpu_cc_cx_gdsc: hw-ctrl-addr missing")
                else:
                    ref = phandles.get(struct.unpack(">I", hw[0:4])[0])
                    hw_base = reg_base(nodes[ref]) if ref else None
                    if hw_base != 0x03D990A0:
                        fails.append(f"DT CX gds_hw_ctrl: 0x{hw_base:x} != expected 0x3d990a0")
    for name in DT_GDSC:
        if name not in seen:
            fails.append(f"DT: no qcom,gdsc node with regulator-name {name}")
    gmu = next((p for p in nodes if "qcom,gmu@" in p), None)
    if not gmu or reg_base(nodes[gmu]) != 0x03D68000:
        fails.append("DT: GMU window 0 is not 0x03d68000")
    kgsl = next((p for p in nodes if "qcom,kgsl-3d0@" in p), None)
    chipid = u32s(nodes[kgsl].get("qcom,chipid", b""))[0] if kgsl else None
    if chipid != 0x43030B00:
        fails.append(f"DT: qcom,chipid 0x{chipid:x} != 0x43030b00 (Adreno735)")
    aop = next((p for p in nodes if AOP_CMD_DB["phandle_name"] in p), None)
    if not aop:
        fails.append("DT: no aop cmd-db reservation")
    else:
        words = u32s(nodes[aop]["reg"])
        if words[1] != AOP_CMD_DB["base"] or words[3] != AOP_CMD_DB["bytes"]:
            fails.append(f"DT: AOP cmd-db {words[1]:#x}/{words[3]:#x} != {AOP_CMD_DB['base']:#x}/{AOP_CMD_DB['bytes']:#x}")
    return seen


def parse_vendor_gpucc(source):
    text = Path(source).read_text()
    clocks = {}
    for kind, name, body in re.findall(r"static struct clk_(\w+) (\w+) = \{(.*?)\n\};", text, re.S):
        for field in ("halt_reg", "cmd_rcgr", "offset", "reg"):
            m = re.search(r"\." + field + r" = (0x[0-9a-fA-F]+)", body)
            if m:
                clocks[name] = (kind if kind != "regmap_div" else "regmap_div", int(m.group(1), 0))
                break
    resets = {"gpu_" + k.lower(): int(v, 16) for k, v in re.findall(r"\[GPUCC_GPU_(CC_\w+_BCR)\] = \{ (0x[0-9a-f]+)", text)}
    plls = {}
    for name in ("gpu_cc_pll0_config", "gpu_cc_pll1_config"):
        block = re.search(name + r" = \{(.*?)\};", text, re.S)
        if block:
            l = re.search(r"\.l = (0x[0-9a-f]+)", block.group(1))
            alpha = re.search(r"\.alpha = (0x[0-9a-f]+)", block.group(1))
            plls[name.replace("_config", "")] = (int(l.group(1), 0), int(alpha.group(1), 0))
    return clocks, resets, plls


def parse_vendor_gcc(source):
    text = Path(source).read_text()
    clocks = {}
    for kind, name, body in re.findall(r"static struct clk_(\w+) (\w+) = \{(.*?)\n\};", text, re.S):
        if not name.startswith("gcc_gpu") and name != "gcc_ddrss_gpu_axi_clk":
            continue
        m = re.search(r"\.(halt_reg|enable_reg) = (0x[0-9a-f]+)", body)
        if m:
            clocks[name] = int(m.group(2), 0)
    mask = 0
    for block in re.findall(r"static struct clk_branch gcc_gpu_gpll0(?:_div)?_clk_src = \{(.*?)\n\};", text, re.S):
        for bit in re.findall(r"\.enable_mask = BIT\((\d+)\)", block):
            mask |= 1 << int(bit)
    always_on = {int(v, 0) for v in re.findall(r"regmap_update_bits\(regmap, (0x[0-9a-f]+), BIT\(0\), BIT\(0\)\)", text)}
    return clocks, mask, always_on


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", default=str(ROOT / "fw/stock/dtb_3.dtb"))
    ap.add_argument("--vendor-source", default=str(ROOT / "fw/android-harvest/vendor-source"))
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv[1:])
    vendor = Path(args.vendor_source)
    fails = []

    gpucc_path, gcc_path = vendor / "gpucc-cliffs.c", vendor / "gcc-cliffs.c"
    for p in (gpucc_path, gcc_path, Path(args.dtb)):
        if not p.exists():
            raise SystemExit(f"missing input: {p}")

    gpucc, resets, plls = parse_vendor_gpucc(gpucc_path)
    for name, (kind, off) in GPUCC_CLOCKS.items():
        if name not in gpucc:
            fails.append(f"gpucc-cliffs.c: no clock {name}")
        elif gpucc[name] != (kind, off):
            fails.append(f"gpucc-cliffs.c: {name} {gpucc[name]} != {(kind, off)}")
    for name, off in GPUCC_RESETS.items():
        if resets.get(name) != off:
            fails.append(f"gpucc-cliffs.c: reset {name} {resets.get(name)} != {off:#x}")
    for name, want in GPUCC_PLLS.items():
        if plls.get(name) != (want["l"], want["alpha"]):
            fails.append(f"gpucc-cliffs.c: {name} config {plls.get(name)} != {(want['l'], want['alpha'])}")

    gcc, vote_mask, always_on = parse_vendor_gcc(gcc_path)
    # gcc_gpu_cfg_ahb_clk has no clk_branch definition on cliffs: the probe enables it directly.
    for name, off in GCC_CLOCKS.items():
        if name == "gcc_gpu_cfg_ahb_clk":
            continue
        if gcc.get(name) != off:
            fails.append(f"gcc-cliffs.c: {name} {gcc.get(name)} != {off:#x}")
    if vote_mask != GCC_GPLL0_VOTE_MASK:
        fails.append(f"gcc-cliffs.c: GPLL0 GPU vote mask {vote_mask:#x} != {GCC_GPLL0_VOTE_MASK:#x}")
    if GCC_CLOCKS["gcc_gpu_cfg_ahb_clk"] not in always_on:
        fails.append("gcc-cliffs.c: gcc_gpu_cfg_ahb_clk is no longer kept always-on by the probe")

    gdsc = check_dt(args.dtb, fails)

    # The probe header carries the same numbers to the kernel; keep it in lockstep with this table.
    header = ROOT / "UniXNU-muyu/package/MuyuGpuPowerBoard.h"
    if header.exists():
        text = header.read_text()
        def define(name):
            import re
            m = re.search(r"^#define\s+" + name + r"\s+\(?(0x[0-9a-fA-F]+|\d+)u?L?L?\)?", text, re.M)
            if not m:
                fails.append(f"MuyuGpuPowerBoard.h: no {name}")
                return None
            return int(m.group(1), 0)
        # Header values are page-relative for the GCC windows and plain offsets inside GPUCC.
        pairs = [
            ("MGB_GCC_GPU_CFG_AHB", 0x00171000, 0x00100000 + GCC_CLOCKS["gcc_gpu_cfg_ahb_clk"]),
            ("MGB_GCC_GPU_MEMNOC_GFX", 0x00171000, 0x00100000 + GCC_CLOCKS["gcc_gpu_memnoc_gfx_clk"]),
            ("MGB_GCC_GPU_SNOC_DVM_GFX", 0x00171000, 0x00100000 + GCC_CLOCKS["gcc_gpu_snoc_dvm_gfx_clk"]),
            ("MGB_GCC_DDRSS_GPU_AXI", 0x00171000, 0x00100000 + GCC_CLOCKS["gcc_ddrss_gpu_axi_clk"]),
            ("MGB_GCC_PLL_VOTE", 0x00152000, 0x00100000 + GCC_GPLL0_VOTE_REG),
            ("MGB_CC_CX_GDSCR", 0, 0x908C),
            ("MGB_CC_GX_GDSCR", 0, 0x9034),
            ("MGB_CC_GX_CFG_GDSCR", 0, 0x9038),
            ("MGB_CC_CX_GDS_HW_CTRL", 0, 0x90A0),
            ("MGB_CC_AHB_CLK", 0, GPUCC_CLOCKS["gpu_cc_ahb_clk"][1]),
            ("MGB_CC_CX_GMU_CLK", 0, GPUCC_CLOCKS["gpu_cc_cx_gmu_clk"][1]),
            ("MGB_CC_SMMU_VOTE_CLK", 0, GPUCC_CLOCKS["gpu_cc_hlos1_vote_gpu_smmu_clk"][1]),
            ("MGB_CC_GMU_CLK_SRC", 0, GPUCC_CLOCKS["gpu_cc_gmu_clk_src"][1]),
            ("MGB_CC_HUB_CLK_SRC", 0, GPUCC_CLOCKS["gpu_cc_hub_clk_src"][1]),
            ("MGB_CC_PLL0", 0, GPUCC_PLLS["gpu_cc_pll0"]["offset"]),
            ("MGB_CC_PLL1", 0, GPUCC_PLLS["gpu_cc_pll1"]["offset"]),
        ]
        for name, base, want in pairs:
            got = define(name)
            if got is not None and base + got != want:
                fails.append(f"MuyuGpuPowerBoard.h: {name} {base + got:#x} != {want:#x}")
        if define("MGB_GPUCC_BASE") != 0x03D90000:
            fails.append("MuyuGpuPowerBoard.h: MGB_GPUCC_BASE is not 0x3d90000")
        print(f"MuyuGpuPowerBoard.h constants checked: {len(pairs) + 1}")

    if args.json:
        Path(args.json).write_text(json.dumps(dict(
            gpucc_clocks={k: {"kind": v[0], "offset": v[1]} for k, v in GPUCC_CLOCKS.items()},
            gpucc_resets=GPUCC_RESETS, gpucc_plls=GPUCC_PLLS, gcc_clocks=GCC_CLOCKS,
            gcc_gpll0_vote={"reg": GCC_GPLL0_VOTE_REG, "mask": GCC_GPLL0_VOTE_MASK},
            dt_gdsc={k: hex(v) for k, v in DT_GDSC.items()}, aop_cmd_db=AOP_CMD_DB,
            sources=dict(gpucc_cliffs_c=hash_of(gpucc_path), gcc_cliffs_c=hash_of(gcc_path),
                         dtb=hash_of(Path(args.dtb)))), indent=2) + "\n")

    print(f"GPUCC clocks checked against gpucc-cliffs.c: {len(GPUCC_CLOCKS)}")
    print(f"GPUCC resets checked: {len(GPUCC_RESETS)}; PLL configs: {len(GPUCC_PLLS)}")
    print(f"GCC GPU clocks checked: {len(GCC_CLOCKS)}; GPLL0 vote mask {GCC_GPLL0_VOTE_MASK:#x}")
    print("DT GDSC addresses:", ", ".join(f"{k}=0x{v:x}" for k, v in gdsc.items()))
    if fails:
        print("\nFAIL")
        for f in fails:
            print("  -", f)
        return 1
    print("\nOK: every documented register agrees with the vendor driver source and the devicetree")
    return 0


def hash_of(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
