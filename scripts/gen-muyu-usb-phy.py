#!/usr/bin/env python3
"""Generate package/MuyuUsbPhy.h from the board's device tree.

The USB3 SS PHY on muyu is `qcom,usb-ssphy-qmp-dp-combo` at 0x88e8000, and its DT node
carries the whole vendor init sequence (`qcom,qmp-phy-init-seq`) plus the per-section
register offsets (`qcom,qmp-phy-reg-offset`). Both are DT facts, so they belong in a
generated header with an assertion, not typed in by hand.

The one trap worth recording: `qcom,qmp-phy-init-seq` packs {offset, value} pairs where
*each cell is a big-endian u32*. Read as little-endian the offsets come out as 0x00100000,
0x04100000, ... which looks like a segmented encoding and tempts an implementer to invent a
decode rule. They are plain byte-swapped 0x1000, 0x1004, ... inside the 0x3000 window.

usage: gen-muyu-usb-phy.py [--dtb FILE] [--check]
"""
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import importlib.util

_spec = importlib.util.spec_from_file_location(
    "mmio", Path(__file__).resolve().parent / "check-muyu-gpu-mmio.py")
mmio = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mmio)

NODE = "/soc/ssphy@88e8000"
COMPATIBLE = b"qcom,usb-ssphy-qmp-dp-combo"


def be_u32(blob, count):
    return list(struct.unpack(">" + "I" * count, blob))


def parse_pairs(blob):
    """`qcom,qmp-phy-init-seq`: packed (offset, value) pairs, big-endian u32 cells."""
    pairs = []
    for at in range(0, len(blob) - 7, 8):
        off, val = struct.unpack_from(">II", blob, at)
        if off == 0 and val == 0:
            break
        pairs.append((off, val))
    return pairs


def collect(dtb):
    nodes, _ = mmio.dt_nodes(dtb)
    if NODE not in nodes:
        raise SystemExit(f"{dtb}: no {NODE}")
    node = nodes[NODE]
    compat = node.get("compatible", b"")
    if not compat.startswith(COMPATIBLE):
        raise SystemExit(f"{NODE}: compatible is {compat!r}, expected {COMPATIBLE!r}")

    base, size = be_u32(node["reg"], 2)
    pairs = parse_pairs(node["qcom,qmp-phy-init-seq"])
    if not pairs:
        raise SystemExit(f"{NODE}: empty init-seq")
    if any(off >= size for off, _ in pairs):
        raise SystemExit(f"{NODE}: init-seq offset outside the {size:#x} window")

    clocks = be_u32(node["clocks"], len(node["clocks"]) // 4)
    resets = be_u32(node["resets"], len(node["resets"]) // 4)
    return {
        "base": base,
        "size": size,
        "pairs": pairs,
        "reg_offset": be_u32(node["qcom,qmp-phy-reg-offset"],
                             len(node["qcom,qmp-phy-reg-offset"]) // 4),
        "clock_names": [x.decode() for x in node["clock-names"].split(b"\0") if x],
        "reset_names": [x.decode() for x in node["reset-names"].split(b"\0") if x],
        "clocks": clocks,
        "resets": resets,
    }


REG_NAMES = (
    ("TX2_OFF", 0), ("RX2_OFF", 1), ("PCS_OFF", 2), ("TX_OFF", 3),
    ("SERDES_OFF", 4), ("RX_OFF", 5), ("DP_PCS_STATUS_OFF", 6),
    ("PCS_MISC_OFF", 7), ("PCS_SW_RESET_OFF", 8), ("PCS_POWER_DOWN_OFF", 9),
    ("START_CTRL_OFF", 10), ("PHY_STATUS_OFF", 11), ("PCS_STATUS_OFF", 12),
)

HEADER = """/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * muyu (SM8635 / cliffs) USB3 SS PHY (QMP DP combo). GENERATED - do not edit by hand.
 *
 * Source: {dtb_rel}, node {node}
 *   compatible = {compat}
 *   reg        = <{base:#x} {size:#x}>          (qmp_phy_base)
 *   clocks     = {clocks}
 *   resets     = {resets}
 *
 * Regenerate and verify with:
 *   python3.12 UniXNU-muyu/scripts/gen-muyu-usb-phy.py
 *   python3.12 UniXNU-muyu/scripts/gen-muyu-usb-phy.py --check
 *
 * `qcom,qmp-phy-init-seq` packs (offset, value) pairs whose cells are BIG-ENDIAN u32. Read
 * little-endian they appear as 0x00100000, 0x04100000, ... which reads like a segmented
 * encoding; they are plain byte-swapped 0x1000, 0x1004, ... All {count} offsets lie inside
 * the {size:#x} window.
 *
 * Not in this header, because they are protocol rather than DT facts: the PLL/SerDes bring-up
 * order, the level select (G3 vs G1 tables) and the GUSB3PIPECTL programming.
 */
#ifndef _MUYU_USB_PHY_H_
#define _MUYU_USB_PHY_H_

#include <stdint.h>

/* qmp_phy_base. The SS PHY is at 0x88e8000, not beside the eUSB2 HS PHY at 0x88e3000. */
#define MUYU_SSPHY_BASE            {base:#010x}u
#define MUYU_SSPHY_BYTES           {size:#06x}u

/* qcom,qmp-phy-reg-offset, in DT order. 0xffff means unused on this board. */
"""

PAIR_BLOCK = """/* The vendor init sequence: {{offset from qmp_phy_base, value}}. */
struct muyu_ssphy_pair {{
    uint32_t off;
    uint32_t val;
}};

static const struct muyu_ssphy_pair muyu_ssphy_init_seq[{count}] = {{
{table}
}};

#define MUYU_SSPHY_INIT_COUNT \\
    (sizeof(muyu_ssphy_init_seq) / sizeof(muyu_ssphy_init_seq[0]))

/* QPHY_* bit meanings (drivers/phy/qualcomm/phy-qcom-qmp.h, same IP family). */
#define MUYU_QPHY_SW_RESET      (1u << 0)   /* QPHY_SW_RESET */
#define MUYU_QPHY_SW_PWRDN      (1u << 0)   /* QPHY_PCS_POWER_DOWN_CONTROL */
#define MUYU_QPHY_START_SERDES  (1u << 0)   /* QPHY_START_CONTROL */
#define MUYU_QPHY_START_PCS     (1u << 1)
#define MUYU_QPHY_PHYSTATUS     (1u << 6)   /* QPHY_PCS_STATUS: 0 = ready */

#endif /* _MUYU_USB_PHY_H_ */
"""


def render(dtb_rel, facts):
    out = [HEADER.format(
        dtb_rel=dtb_rel, node=NODE, compat=COMPATIBLE.decode(),
        base=facts["base"], size=facts["size"], count=len(facts["pairs"]),
        clocks=", ".join(facts["clock_names"]),
        resets=", ".join(facts["reset_names"]))]
    for name, index in REG_NAMES:
        value = facts["reg_offset"][index]
        out.append(f"#define MUYU_SSPHY_{name}".ljust(40) + f"{value:#06x}u\n")
    out.append("\n")
    rows = []
    pairs = facts["pairs"]
    for at in range(0, len(pairs), 4):
        rows.append("    " + " ".join(f"{{{o:#06x},{v:#04x}}}," for o, v in pairs[at:at + 4]))
    out.append(PAIR_BLOCK.format(count=len(pairs), table="\n".join(rows)))
    return "".join(out)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", default=str(ROOT / "fw/stock/dtb_3.dtb"))
    ap.add_argument("--out", default=str(ROOT / "UniXNU-muyu/package/MuyuUsbPhy.h"))
    ap.add_argument("--check", action="store_true", help="verify the checked-in header matches the DT")
    args = ap.parse_args(argv[1:])

    dtb = Path(args.dtb)
    facts = collect(dtb)
    try:
        dtb_rel = str(dtb.resolve().relative_to(ROOT))
    except ValueError:
        dtb_rel = str(dtb)
    text = render(dtb_rel, facts)

    out = Path(args.out)
    if args.check:
        current = out.read_text() if out.is_file() else ""
        if current != text:
            print(f"{out}: does not match {dtb_rel}; regenerate with gen-muyu-usb-phy.py")
            return 1
        print(f"USB SS PHY facts: OK ({len(facts['pairs'])} init pairs, "
              f"base {facts['base']:#x}, {facts['size']:#x} bytes)")
        return 0
    out.write_text(text)
    print(f"wrote {out} ({len(facts['pairs'])} init pairs)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
