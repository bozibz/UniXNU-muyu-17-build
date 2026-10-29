#!/usr/bin/env python3
"""Derive muyu's GPU/GMU platform description from its own devicetree.

The sheng recipe (prepare-sheng-gmu.py) takes its register offsets from a pinned copy of the Linux msm
driver plus fdtget on sheng.dtb; neither input exists in this repository. This script needs only the
device's DTB, and emits the same facts the GMU cold start and the SMMU context need, so the values are
reproducible instead of typed by hand.

usage: gen-muyu-gpu-platform.py [--dtb PATH] [--out PATH]
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Order matters: it is enum {ShengGmuCore, ShengGmuRscc, ShengGmuPdc, ShengGmuGpu, ShengGmuCxMisc}.
WINDOWS = ["gmu", "rscc", "gmu_pdc", "kgsl_3d0_reg_memory", "cx_misc"]


def parse_fdt(data):
    """Minimal FDT reader: returns {path: {prop: bytes}} with phandles resolved separately."""
    magic, _total, off_struct, off_strings = struct.unpack_from(">4sIII", data, 0)
    if magic != b"\xd0\x0d\xfe\xed":
        raise SystemExit("not a dtb")
    pos, stack, nodes = off_struct, [], {}

    def name_at(off):
        end = data.index(b"\0", off_strings + off)
        return data[off_strings + off:end].decode("utf-8", "replace")

    while pos + 4 <= len(data):
        tok, = struct.unpack_from(">I", data, pos)
        pos += 4
        if tok == 1:
            end = data.index(b"\0", pos)
            stack.append(data[pos:end].decode("utf-8", "replace"))
            pos = (end + 4) & ~3
        elif tok == 2:
            stack.pop()
        elif tok == 3:
            ln, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            nodes.setdefault("/" + "/".join(c for c in stack if c), {})[name_at(nameoff)] = data[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == 9:
            break
    return nodes


def u32s(blob):
    return list(struct.unpack(">%dI" % (len(blob) // 4), blob))


def reg_pairs(node):
    """(name, base, size) for each entry of reg/reg-names (1 address cell + 1 size cell on this SoC)."""
    words = u32s(node["reg"])
    names = [n.decode() for n in node.get("reg-names", b"").split(b"\0") if n]
    return [(names[i // 2] if i // 2 < len(names) else "?", words[i], words[i + 1])
            for i in range(0, len(words), 2)]


def interrupts(node):
    """SPI numbers from <type number flags> triples (type 0 = SPI)."""
    words = u32s(node["interrupts"])
    return [words[i + 1] for i in range(0, len(words), 3) if words[i] == 0]


def strings(node, key):
    return [s.decode() for s in node.get(key, b"").split(b"\0") if s]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", type=Path, default=ROOT / "fw/stock/dtb_3.dtb")
    ap.add_argument("--out", type=Path, default=ROOT / "UniXNU-muyu/build/gpu/muyu-gpu-platform.json")
    ap.add_argument("--firmware-dir", type=Path, default=ROOT / "fw/gpu-firmware")
    a = ap.parse_args()

    nodes = parse_fdt(a.dtb.read_bytes())
    gpu = nodes["/soc/qcom,kgsl-3d0@3d00000"]
    gmu = nodes["/soc/qcom,gmu@3d69000"]
    phandles = {struct.unpack(">I", n["phandle"])[0]: p for p, n in nodes.items() if len(n.get("phandle", b"")) == 4}

    ranges = {}
    for want in ("kgsl_3d0_reg_memory", "rscc", "cx_misc"):
        for name, base, size in reg_pairs(gpu):
            if name == want:
                ranges[name] = {"base": base, "size": size}
    for name, base, size in reg_pairs(gmu):
        ranges[name] = {"base": base, "size": size}

    missing = [w for w in WINDOWS if w not in ranges]
    if missing:
        raise SystemExit("devicetree is missing windows: " + ", ".join(missing))

    gmu_iommus = u32s(gmu["iommus"])
    smmu_path = phandles.get(gmu_iommus[0], "?")
    smmu = nodes.get(smmu_path, {})
    smmu_regs = {name: {"base": base, "size": size} for name, base, size in reg_pairs(smmu)} if smmu else {}

    platform = {
        "device": "muyu",
        "soc": "SM8635",
        "gpu": {
            "model": strings(gpu, "qcom,gpu-model")[0] if strings(gpu, "qcom,gpu-model") else None,
            "compatible": strings(gpu, "compatible"),
            "irq_spi": interrupts(gpu)[0] if interrupts(gpu) else None,
            "clocks": strings(gpu, "clock-names"),
        },
        "gmu": {
            "compatible": strings(gmu, "compatible"),
            "irq_hfi_spi": interrupts(gmu)[0] if interrupts(gmu) else None,
            "irq_gmu_spi": interrupts(gmu)[1] if len(interrupts(gmu)) > 1 else None,
            "clocks": strings(gmu, "clock-names"),
            "regulators": strings(gmu, "regulator-names"),
            "mboxes": strings(gmu, "mbox-names"),
        },
        "windows": [{"index": i, "name": w, **ranges[w]} for i, w in enumerate(WINDOWS)],
        "smmu": {
            "path": smmu_path,
            "compatible": strings(smmu, "compatible") if smmu else [],
            "regions": smmu_regs,
            "global_interrupts": u32s(smmu["#global-interrupts"])[0] if smmu.get("#global-interrupts") else None,
            "irq_spis": interrupts(smmu) if smmu.get("interrupts") else [],
            "stream": gmu_iommus[1] if len(gmu_iommus) > 1 else None,
            "iommu_cells": u32s(smmu["#iommu-cells"])[0] if smmu.get("#iommu-cells") else None,
        },
        "firmware": {
            name: {"file": name, "bytes": (a.firmware_dir / name).stat().st_size if (a.firmware_dir / name).exists() else 0,
                   "sha256": sha256(a.firmware_dir / name)}
            for name in ("gen71100_gmu.bin", "gen71100_sqe.fw", "gen71100_zap.mbn")
        },
        "source_dtb": {"path": str(a.dtb), "sha256": sha256(a.dtb)},
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(platform, indent=2) + "\n")
    print(f"wrote {a.out}")
    for w in platform["windows"]:
        print(f"  window {w['index']} {w['name']:20s} 0x{w['base']:08x} +0x{w['size']:x}")
    print(f"  irq gpu={platform['gpu']['irq_spi']} hfi={platform['gmu']['irq_hfi_spi']} gmu={platform['gmu']['irq_gmu_spi']}")
    print(f"  smmu {platform['smmu']['path']} stream={platform['smmu']['stream']} spis={len(platform['smmu']['irq_spis'])}")


if __name__ == "__main__":
    main()
