#!/usr/bin/env python3
"""Verify muyu's RPMh RSC (drv@2) facts against the sources they came from; never touches the device.

The MuyuRpmhRsc package pins these numbers, all re-derived from first-party inputs:

  1. muyu's own devicetree (fs-decoded, fw/stock/dtb_3.dtb): the RSC node at
     /soc/rsc@17a00000 with drv@2 (qcom,drv-id=2, qcom,tcs-offset=0xd00) and the
     channel@0 qcom,tcs-config pairs (2,3),(0,2),(1,2),(3,0),(4,1). rpmh
     assigns TCS ids in pair order, so ACTIVE (type 2) gets ids {0,1,2}, then
     SLEEP {3,4}, WAKE {5,6}, CONTROL none, FAST_PATH {7}.
  2. The two ARC regulators that gate GX: gfx.lvl (pmxr2230-s4-level, init
     0x10) and mxc.lvl (pm_v6c_s6_gfx_voter_level, init 0x30), both
     qcom,set=3 (set-active) on slave 3.
  3. The AOP command-db reservation (0x81c60000 + 0x20000, version 1).

Anything here that drifts from the sources is an error: the code and the doc
are supposed to fail loudly instead of drifting.

usage: check-muyu-rpmh-host.py [--dtb PATH] [--json PATH] [--no-compile]
"""
import argparse
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # .../pad7pro-xnu
PKG = ROOT / "UniXNU-muyu/package"

RSC = {
    "rsc_phandle": "soc/rsc@17a00000",
    "drv2": "soc/rsc@17a00000/drv@2",
    "drv2_base": 0x17A20000,      # drv@2's reg is the second 0x10000 share
    "drv2_bytes": 0x10000,
    "drv_id": 2,
    "tcs_offset": 0xD00,
    "major_min": 3,
}
# channel@0 qcom,tcs-config pairs (type,count) in DT order. rpmh_probe_channel_tcs_config
# assigns ids {0,1,2} to the first (ACTIVE), {3,4} SLEEP, {5,6} WAKE, {} CONTROL, {7} FAST_PATH.
TCS_CONFIG = ((2, 3), (0, 2), (1, 2), (3, 0), (4, 1))
ARC_SLAVE = 3
ARC_REGS = {
    # (node suffix, resource name in cmd-db, DT regulator-name, init level)
    "gfx": ("pmxr2230-s4-level", "gfx.lvl", "pmxr2230_s4_level", 0x10),
    "mxc": ("pm_v6c-s6-gfx-voter-level", "mxc.lvl", "pm_v6c_s6_gfx_voter_level", 0x30),
}
CMD_DB = dict(node="/reserved-memory/aop_cmd_db_region@81c60000", base=0x81C60000,
              bytes=0x20000, version=1)


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


def check_dt(dtb, fails):
    nodes, phandles = dt_nodes(dtb)

    rsc = "/" + RSC["rsc_phandle"]
    if rsc not in nodes:
        fails.append("DT: no RSC node at /soc/rsc@17a00000")
    else:
        pairs = u32s(nodes[rsc]["reg"])
        if len(pairs) != 6:
            fails.append(f"DT RSC: reg has {len(pairs)} cells, expected 6 (3 base/size pairs)")
        elif pairs[0] != 0x17A00000 or pairs[2] != 0x17A10000 or pairs[5] != RSC["drv2_bytes"]:
            fails.append(f"DT RSC: reg {[f'{v:#x}' for v in pairs]} does not match the expected shares")
        elif pairs[4] != RSC["drv2_base"]:
            fails.append(f"DT RSC: drv@2 share {pairs[4]:#x} != {RSC['drv2_base']:#x}")

    drv = next((p for p in nodes if p.endswith("/drv@2")), None)
    if not drv:
        fails.append("DT: no drv@2 under the RSC")
    else:
        if u32s(nodes[drv].get("qcom,drv-id", b""))[0] != RSC["drv_id"]:
            fails.append("DT drv@2: qcom,drv-id != 2")
        if u32s(nodes[drv].get("qcom,tcs-offset", b""))[0] != RSC["tcs_offset"]:
            fails.append("DT drv@2: qcom,tcs-offset != 0xd00")

        ch = next((p for p in nodes if "/drv@2/" in p and "channel" in p), None)
        if not ch:
            fails.append("DT drv@2: no channel child")
        else:
            pairs = u32s(nodes[ch].get("qcom,tcs-config", b""))
            got = [(pairs[i], pairs[i + 1]) for i in range(0, len(pairs), 2)]
            if got != list(TCS_CONFIG):
                fails.append(f"DT drv@2/channel: qcom,tcs-config {got} != {list(TCS_CONFIG)}")

    # The two ARC regulators, by regulator-name, with qcom,set=3 (active set)
    # and the pinned init levels.
    for key, (suffix, res_id, reg_name, init) in ARC_REGS.items():
        hit = next((p for p in nodes
                    if nodes[p].get("regulator-name", b"").rstrip(b"\0") == reg_name.encode()), None)
        if not hit:
            fails.append(f"DT: no regulator-name {reg_name}")
            continue
        props = nodes[hit]
        parent = next((p for p in nodes if p.rstrip("/") == hit.rsplit("/", 1)[0]), None)
        pprops = nodes[parent] if parent in nodes else {}
        if pprops.get("qcom,resource-name", b"").rstrip(b"\0") != res_id.encode():
            fails.append(f"DT {reg_name}: parent resource-name != {res_id}")
        if pprops.get("compatible", b"").rstrip(b"\0") != b"qcom,rpmh-arc-regulator":
            fails.append(f"DT {reg_name}: parent compatible != qcom,rpmh-arc-regulator")
        init_got = u32s(props.get("qcom,init-voltage-level", b"")[:4])[0] if "qcom,init-voltage-level" in props else None
        if init_got != init:
            fails.append(f"DT {reg_name}: init-voltage-level 0x{init_got:x} != 0x{init:x}")
        set_got = u32s(props.get("qcom,set", b""))[0] if "qcom,set" in props else None
        if set_got != 3:
            fails.append(f"DT {reg_name}: qcom,set {set_got} != 3 (active)")

    aop = next((p for p in nodes if CMD_DB["node"] in p), None)
    if not aop:
        fails.append("DT: no AOP cmd-db reservation")
    else:
        words = u32s(nodes[aop]["reg"])
        if words[1] != CMD_DB["base"] or words[3] != CMD_DB["bytes"]:
            fails.append(f"DT AOP cmd-db: {words[1]:#x}/{words[3]:#x} != {CMD_DB['base']:#x}/{CMD_DB['bytes']:#x}")


def compile_and_run(fails):
    """Freestanding compile the package + host tests, run the binary (ASan/UBSan)."""
    cc = "clang"
    cflags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-fsanitize=address,undefined",
              "-fno-omit-frame-pointer", "-I", str(PKG)]
    with tempfile.TemporaryDirectory(prefix="muyu-rpmh-host-") as tmp:
        tmp = Path(tmp)
        obj = tmp / "test.bin"
        cmd = [cc] + cflags + [str(PKG / "MuyuRpmhRsc.c"), str(PKG / "MuyuRpmhRscHostTests.c"),
                               "-o", str(obj)]
        try:
            subprocess.run(cmd, check=True, capture_output=True, text=True)
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            fails.append(f"host compile failed: {e.stderr if hasattr(e, 'stderr') else e}")
            return
        try:
            subprocess.run([str(obj)], check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as e:
            fails.append(f"host test failed (exit {e.returncode}):\n{e.stdout}\n{e.stderr}")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--dtb", default=str(ROOT / "fw/stock/dtb_3.dtb"))
    ap.add_argument("--json", default=None)
    ap.add_argument("--no-compile", action="store_true")
    args = ap.parse_args(argv[1:])

    fails = []
    check_dt(args.dtb, fails)
    if not args.no_compile:
        compile_and_run(fails)

    ok = not fails
    print("RPMh RSC facts:", "OK" if ok else "FAIL")
    for f in fails:
        print("  -", f)

    if args.json:
        Path(args.json).write_text(json.dumps(
            {"ok": ok, "fails": fails, "cmd_db": CMD_DB, "rsc": RSC,
             "tcs_config": [list(p) for p in TCS_CONFIG],
             "arcs": {k: v[1] for k, v in ARC_REGS.items()}}, indent=2) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))