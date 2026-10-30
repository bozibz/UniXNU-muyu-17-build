#!/usr/bin/env python3
"""Generate muyu's pinned GMU OPP/BCM policy (ShengGmuVotesSource.h) from its own devicetree.

ShengGmu.c/ShengGmuVotes.c are board-agnostic but include generated headers. sheng
produces ShengGmuVotesSource.h from a pinned mainline `a6xx_catalog.c` entry (chip id
0x43050a01) plus its DT's mainline `opp-table` nodes. muyu is different on both counts:

  * its board DT (`fw/stock/dtb_3.dtb`, the very DTB the vendor_boot ships) does not use
    mainline `opp-table` at all. It carries the vendor Adreno layout:
      /soc/qcom,kgsl-3d0@3d00000/qcom,gpu-pwrlevels/qcom,gpu-pwrlevel@N
          qcom,gpu-freq   (Hz)
          qcom,level      (RPMh ARC level)
          qcom,acd-level  (32-bit ACD encoding, present on every level here)
          qcom,bus-freq   (index into the parent's qcom,bus-table-ddr)
      /soc/qcom,gmu@3d69000/qcom,gmu-freq-table   (freq, level pairs)
    This is the same data the running Android kgsl driver uses (its `num_pwrlevels=11`
    and `freq_table_mhz = 1100 1000 950 900 835 736 684 633 500 353 255` match), so it is
    the board's own policy, not a guess from another SKU. The upstream sm8635
    devicetrees in fact differ (cliffs7: 9 levels, 950 MHz top) -- this SKU is binned
    higher, which is exactly why the device DTB has to be the source.

  * Adreno 735 (chip id 0x43030b00) has no entry in any public catalog. The BCM policy
    (SH0/MC0 buswidth, ACV fixed perfmode) is therefore taken from the *common A7xx
    policy*: every ADRENO_7XX_* entry in the pinned mainline `a7xx_catalog.c` must agree
    before it is used, and the ACV perfmode threshold must equal the board's own top
    DDR bandwidth. If upstream ever diverges or adds 0x43030b00, this script stops or
    switches to the exact entry.

The AOP command-db reservation (base/bytes) also comes from the board DT; its version
word is board policy (mode 10 observed `w0=00000001`, HANDOFF-2026-10-29 §18) and is
pinned here so the parser can reject a different database instead of echoing whatever
it reads.

Nothing here touches hardware, and no value is typed that the inputs do not contain.

usage: gen-muyu-gmu-votes.py [--out DIR] [--dtb PATH] [--catalog PATH]
"""
import argparse
import json
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MUYU_GPU_NODE = "/soc/qcom,kgsl-3d0@3d00000"
MUYU_GMU_NODE = "/soc/qcom,gmu@3d69000"
MUYU_CMD_DB_NODE = "/reserved-memory/aop_cmd_db_region@81c60000"

# Board policy, not derivable from any pinned file: the AOP command-db version word.
# Observed once, on this board, by mode 10 (HANDOFF-2026-10-29 §18.1: w0=00000001).
COMMAND_DB_VERSION = 1

BCM_NAMES = ["SH0", "MC0", "ACV"]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def properties(dtb, path):
    return subprocess.check_output(["fdtget", "-p", str(dtb), path], text=True).split()


def fdtget(dtb, path, prop=None, kind="x"):
    cmd = ["fdtget"]
    if prop is None:
        cmd += ["-l", str(dtb), path]
    else:
        cmd += ["-t", kind, str(dtb), path, prop]
    out = subprocess.check_output(cmd, text=True).split()
    if prop is None:
        return out
    return [int(v, 16) for v in out] if kind in ("x", "i") else out


def one(dtb, path, prop):
    values = fdtget(dtb, path, prop)
    if len(values) != 1:
        raise SystemExit(f"{path}/{prop}: expected one cell, got {values}")
    return values[0]


def parse_opps(dtb):
    """GPU and GMU OPPs in the vendor (kgsl) DT layout."""
    table = MUYU_GPU_NODE + "/qcom,gpu-pwrlevels"
    bus = fdtget(dtb, MUYU_GPU_NODE, "qcom,bus-table-ddr")
    if len(bus) < 2 or any(bus[i] >= bus[i + 1] for i in range(len(bus) - 1)):
        raise SystemExit("DDR bus table is not a strictly ascending list")
    children = sorted(fdtget(dtb, table), key=lambda n: int(n.rsplit("@", 1)[1]))
    if not children:
        raise SystemExit("no qcom,gpu-pwrlevels entries")
    gpu = []
    for index, child in enumerate(children):
        node = f"{table}/{child}"
        reg = one(dtb, node, "reg")
        if reg != index:
            raise SystemExit(f"{child}: reg {reg} does not match its @N suffix")
        hz = one(dtb, node, "qcom,gpu-freq")
        level = one(dtb, node, "qcom,level")
        bus_index = one(dtb, node, "qcom,bus-freq")
        if not hz or hz % 1000 or not 0 < level <= 0xFFFF:
            raise SystemExit(f"{child}: unrepresentable OPP frequency/level")
        if not 0 < bus_index < len(bus):
            raise SystemExit(f"{child}: qcom,bus-freq {bus_index} outside the bus table")
        acd_present = "qcom,acd-level" in properties(dtb, node)
        acd = one(dtb, node, "qcom,acd-level") if acd_present else 0
        if not bus[bus_index]:
            raise SystemExit(f"{child}: zero bandwidth at bus index {bus_index}")
        gpu.append(dict(hz=hz, level=level, kbps=bus[bus_index], acd_present=acd_present, acd=acd))
    gpu.sort(key=lambda e: e["hz"])
    if len({e["hz"] for e in gpu}) != len(gpu):
        raise SystemExit("duplicate GPU OPP frequencies")
    if any(gpu[i]["level"] <= gpu[i - 1]["level"] for i in range(1, len(gpu))):
        raise SystemExit("GPU OPP levels are not strictly ascending in frequency order")

    pairs = fdtget(dtb, MUYU_GMU_NODE, "qcom,gmu-freq-table")
    if not pairs or len(pairs) % 2:
        raise SystemExit("qcom,gmu-freq-table is not a list of (freq, level) pairs")
    gmu = [dict(hz=pairs[i], level=pairs[i + 1], kbps=0, acd_present=False, acd=0)
           for i in range(0, len(pairs), 2)]
    for entry in gmu:
        if not entry["hz"] or not 0 < entry["level"] <= 0xFFFF:
            raise SystemExit("unrepresentable GMU OPP frequency/level")
    gmu.sort(key=lambda e: e["hz"])
    if any(gmu[i]["level"] <= gmu[i - 1]["level"] for i in range(1, len(gmu))):
        raise SystemExit("GMU OPP levels are not strictly ascending in frequency order")

    # ShengGmuBuildStartup expects the zero (off) entry first; sheng's generator does the same.
    dummy = dict(hz=0, level=0, kbps=0, acd_present=False, acd=0)
    if len(gpu) + 1 > 16 or len(gmu) + 1 > 16:
        raise SystemExit("more OPPs than AdrenoHfiArcVotes accepts (16)")
    # AdrenoHfiBuildBandwidth requires the off (0 KB/s) entry first, then strictly
    # ascending active bandwidths -- same shape as sheng's generated table.
    bandwidths = sorted({0} | {e["kbps"] for e in gpu})
    if len(bandwidths) > 16:
        raise SystemExit("more bandwidth levels than AdrenoHfiBuildBandwidth accepts (16)")
    return dict(gpu=[dummy] + gpu, gmu=[dummy] + gmu, bandwidths=bandwidths, bus_table=bus)


def parse_catalog(chip_id, bandwidths):
    """The board's BCM policy: exact chip entry if one exists, else the common A7xx list."""
    text = Path(CATALOG).read_text()
    starts = [(m.start(), m.group(1)) for m in
              re.finditer(r"\.chip_ids = ADRENO_CHIP_IDS\(\s*([^)]*)\)", text)]
    entries = []
    for i, (at, ids) in enumerate(starts):
        end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
        body = text[at:end]
        chip_ids = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+", ids)]
        family = re.search(r"\.family = (ADRENO_[A-Z0-9_]+)", body)
        # The array ends with a sentinel entry; stopping at the first "}," would only
        # capture SH0 because every BCM row is one line.
        bcms = re.search(r"\.bcms = \(const struct a6xx_bcm\[\]\) \{(.*?\{ /\* sentinel \*/ \}.*?\},)",
                         body, re.S)
        entries.append(dict(chip_ids=chip_ids, family=family.group(1) if family else None,
                            bcms=parse_bcms(bcms.group(1)) if bcms else None,
                            body=body))

    def proportional(entries, label):
        """SH0/MC0 are SoC interconnect widths: every A7xx entry must agree on them."""
        distinct = {json.dumps([b for b in e["bcms"] if not b["fixed"]], sort_keys=True)
                    for e in entries}
        if len(distinct) != 1:
            raise SystemExit(f"{label}: proportional (SH0/MC0) policies differ, refusing to pick one")
        return [b for b in entries[0]["bcms"] if not b["fixed"]]

    exact = [e for e in entries if chip_id in e["chip_ids"]]
    if exact:
        if not exact[0]["bcms"]:
            raise SystemExit("exact catalog entry has no BCM policy")
        policy, count = exact[0]["bcms"], 1
        gmu_chipid = re.search(r"\.gmu_chipid = (0x[0-9a-fA-F]+)", exact[0]["body"])
        source = dict(kind="exact", family=exact[0]["family"],
                      gmu_chipid=int(gmu_chipid.group(1), 0) if gmu_chipid else None)
    else:
        candidates = [e for e in entries if e["family"] and e["family"].startswith("ADRENO_7XX_")
                      and e["bcms"]]
        if not candidates:
            raise SystemExit("catalog has no ADRENO_7XX_* BCM policy")
        proportional_bcms = proportional(candidates, "common A7xx policy")
        # ACV is the perfmode switch; its threshold is a property of the memory clock
        # plan, so the board's own top DDR bandwidth selects the matching variant.
        top = max(bandwidths)
        acv = [e for e in candidates if [b for b in e["bcms"] if b["fixed"]]
               and [b for b in e["bcms"] if b["fixed"]][0]["perfmode_kbps"] == top]
        if not acv:
            raise SystemExit(f"no A7xx ACV policy with perfmode_bw == top DDR bandwidth {top}")
        variants = {json.dumps([b for b in e["bcms"] if b["fixed"]], sort_keys=True) for e in acv}
        if len(variants) != 1:
            raise SystemExit("A7xx ACV policies at this bandwidth differ, refusing to pick one")
        policy = proportional_bcms + [b for b in acv[0]["bcms"] if b["fixed"]]
        count = len(candidates)
        source = dict(kind="common-a7xx", family=None, gmu_chipid=None, entries=count,
                      acv_variants=[json.loads(v)[0]["perfmode_kbps"] for v in sorted(variants)])

    names = [b["name"] for b in policy]
    if names != BCM_NAMES:
        raise SystemExit(f"BCM order {names} is not {BCM_NAMES}")
    for b in policy:
        if not b["fixed"]:
            if not b["buswidth"] or b["perfmode"] or b["perfmode_kbps"]:
                raise SystemExit(f"{b['name']}: proportional BCM has a bad policy")
        else:
            if not b["perfmode"]:
                raise SystemExit(f"{b['name']}: fixed BCM votes no perfmode")
    acv = policy[2]
    if acv["perfmode_kbps"] != max(bandwidths):
        raise SystemExit(f"ACV perfmode threshold {acv['perfmode_kbps']} != top DDR bandwidth "
                         f"{max(bandwidths)}; board policy disagrees with the catalog")
    source["entries"] = count
    return policy, source


def parse_bcms(block):
    out = []
    for m in re.finditer(r"\{([^{}]*)\}", block):
        body = m.group(1)
        if ".name" not in body:
            continue
        entry = dict(name=re.search(r'\.name = "([A-Z0-9]+)"', body).group(1),
                     buswidth=int(re.search(r"\.buswidth = (\d+)", body).group(1))
                     if ".buswidth" in body else 0,
                     fixed=1 if ".fixed = true" in body else 0,
                     perfmode=0, perfmode_kbps=0)
        if ".perfmode = BIT(" in body:
            entry["perfmode"] = 1 << int(re.search(r"\.perfmode = BIT\((\d+)\)", body).group(1))
        if ".perfmode_bw = " in body:
            entry["perfmode_kbps"] = int(re.search(r"\.perfmode_bw = (\d+)", body).group(1))
        if entry["perfmode"] > 0x3FFF:
            raise SystemExit(f"{entry['name']}: perfmode does not fit the HFI field")
        out.append(entry)
    return out


def parse_command_db(dtb):
    if fdtget(dtb, MUYU_CMD_DB_NODE, "compatible", "s") != ["qcom,cmd-db"]:
        raise SystemExit("command-db compatible mismatch")
    cells = fdtget(dtb, MUYU_CMD_DB_NODE, "reg")
    if len(cells) != 4:
        raise SystemExit("command-db reg is not <base_hi base_lo size_hi size_lo>")
    base = (cells[0] << 32) | cells[1]
    size = (cells[2] << 32) | cells[3]
    if not base or size < 144 or size > 1 << 20 or size & (size - 1):
        raise SystemExit("unsupported command-db reservation")
    return base, size


def write_header(out, chip_id, model, opps, policy, command_db):
    def opp_block(kind, entries):
        rows = "\n".join("{%d,%d,%d,%d,%d}," % (e["hz"], e["level"], e["kbps"],
                                                e["acd_present"], e["acd"]) for e in entries)
        return (f"#define SHENG_{kind.upper()}_OPPS {len(entries)}u\n"
                f"static const ShengGmuOpp sheng_{kind}_opps[]={{\n{rows}\n}};\n")

    text = "/* Generated by UniXNU-muyu/scripts/gen-muyu-gmu-votes.py. Do not commit. */\n"
    text += "/* Board OPP/BCM/ACD policy from muyu's own devicetree and the pinned A7xx catalog. */\n"
    text += "#include <stdint.h>\n"
    text += f"#define SHENG_GMU_VOTES_CHIP_ID {chip_id:#x}u /* DT qcom,chipid, {model} */\n"
    text += "typedef struct {uint64_t hz;uint32_t level,kbps,acd_present,acd;} ShengGmuOpp;\n"
    text += opp_block("gpu", opps["gpu"])
    text += opp_block("gmu", opps["gmu"])
    text += (f"#define SHENG_GPU_BANDWIDTHS {len(opps['bandwidths'])}u\n"
             f"static const uint32_t sheng_gpu_bandwidths[]={{"
             + ",".join(str(b) for b in opps["bandwidths"]) + "};\n")
    text += "static const AdrenoHfiBcm sheng_bcm_policy[3]={\n"
    text += "\n".join("{.buswidth=%d,.fixed=%d,.perfmode=%d,.perfmode_kbps=%d}," %
                      (b["buswidth"], b["fixed"], b["perfmode"], b["perfmode_kbps"]) for b in policy)
    text += "\n};\n"
    header = Path(out) / "ShengGmuVotesSource.h"
    header.write_text(text)

    db = ("/* Pinned DT reservation + the board's observed command-db version. */\n"
          "#include <stdint.h>\n"
          f"#define SHENG_GMU_COMMAND_DB_BASE UINT64_C({command_db[0]:#x})\n"
          f"#define SHENG_GMU_COMMAND_DB_BYTES {command_db[1]}u\n"
          f"#define SHENG_GMU_COMMAND_DB_VERSION {COMMAND_DB_VERSION}u\n")
    db_header = Path(out) / "ShengGmuCommandDbSource.h"
    db_header.write_text(db)
    return header, db_header


def main(argv):
    global CATALOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "UniXNU-muyu/build/gpu"))
    ap.add_argument("--dtb", default=str(ROOT / "fw/stock/dtb_3.dtb"))
    ap.add_argument("--catalog", default=str(ROOT / "fw/android-harvest/msm-drv/a7xx_catalog.c"))
    args = ap.parse_args(argv[1:])
    CATALOG = args.catalog

    dtb = Path(args.dtb)
    if not dtb.exists():
        raise SystemExit(f"missing board DTB {dtb}")
    if not Path(CATALOG).exists():
        raise SystemExit(f"missing pinned catalog {CATALOG} (see fw/android-harvest/README.md)")
    chip = one(dtb, MUYU_GPU_NODE, "qcom,chipid")
    model = fdtget(dtb, MUYU_GPU_NODE, "qcom,gpu-model", "s")
    model = model[0] if model else "?"
    opps = parse_opps(dtb)
    policy, bcm_source = parse_catalog(chip, opps["bandwidths"])
    command_db = parse_command_db(dtb)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    header, db_header = write_header(out, chip, model, opps, policy, command_db)
    report = dict(
        gpu_chip_id=f"{chip:#x}", gpu_model=model,
        gpu_opps=[dict(hz=e["hz"], mhz=e["hz"] // 1000000, level=e["level"], kbps=e["kbps"],
                       acd_present=bool(e["acd_present"]), acd=f"{e['acd']:#x}") for e in opps["gpu"]],
        gmu_opps=[dict(hz=e["hz"], mhz=e["hz"] // 1000000, level=e["level"]) for e in opps["gmu"]],
        bandwidths_kbps=opps["bandwidths"], ddr_bus_table_kbps=opps["bus_table"],
        bcm_policy=policy, bcm_source=bcm_source,
        command_db=dict(base=f"{command_db[0]:#x}", bytes=command_db[1],
                        version=COMMAND_DB_VERSION,
                        evidence="mode 10 w0=00000001 (HANDOFF-2026-10-29 §18.1)"),
        inputs={str(p): sha256(p) for p in (dtb, Path(CATALOG))},
        generated_headers={str(h): sha256(h) for h in (header, db_header)},
        source_analysis_only=True, hardware_registers_observed=False)
    (out / "muyu-gmu-votes-report.json").write_text(json.dumps(report, indent=2) + "\n")

    print(f"chip {chip:#x} ({model}); GPU OPPs {len(opps['gpu'])} (incl. off); "
          f"GMU OPPs {len(opps['gmu'])}; bandwidths {len(opps['bandwidths'])}")
    print(f"  GPU: " + ", ".join(f"{e['hz'] // 1000000} MHz/L{e['level']}/{e['kbps'] // 1000} MB/s"
                                 for e in opps["gpu"][1:]))
    print(f"  GMU: " + ", ".join(f"{e['hz'] // 1000000} MHz/L{e['level']}" for e in opps["gmu"][1:]))
    print(f"  BCM: " + ", ".join(f"{b['name']}bw{b['buswidth']}" for b in policy)
          + f" from {bcm_source['kind']} ({bcm_source['entries']} entries)")
    print(f"  cmd-db {command_db[0]:#x}+{command_db[1]:#x} v{COMMAND_DB_VERSION}")
    print(f"wrote {header} and {db_header}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
