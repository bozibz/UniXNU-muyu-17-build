#!/usr/bin/env python3
"""Audit the 27 line's configuration for hand-over damage.

Why this exists
---------------
The 27 line (iPadOS 27.0.1 / 24A446) was carried by many agents in sequence, with
different conventions. The failure mode is never a syntax error - toml parses fine
and the build runs - it is a *silent* disconnect:

  * a series file that is declared but was never committed, so the applied patch
    set is whatever the loader happens to glob;
  * a defconfig that is named but does not exist, so the build quietly falls back;
  * a boot argument whose parser was renamed, removed, or never committed, so the
    option is inert and the panel shows nothing - indistinguishable from "the code
    ran and did nothing";
  * a path that pointed at a file somebody deleted three hand-overs ago.

None of those fail the build. All of them cost a flash to discover. This script
turns each into a line of output.

Usage
-----
    python3 scripts/audit-27-config.py            # human report, exit 1 on FAIL
    python3 scripts/audit-27-config.py --quiet    # only FAIL lines + summary

Exit status: 0 = clean, 1 = at least one FAIL.
"""
from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "board.spike27-live.toml"

# Boot arguments that XNU itself parses. Verified by reading the upstream names,
# not by guessing: these live in osfmk/pexpert and bsd/, which this repo does not
# carry. Keep this list short and justified - anything device-specific belongs in
# the source scan, not here.
XNU_NATIVE = {
    "debug",                     # pe_parse_boot_argn("debug")
    "disable_aslr",              # osfmk/kern/... (-disable_aslr)
    "use_contiguous_hint",       # arm_vm_init
    "kt_fixed_zones",            # osfmk/kern/zalloc
    "apfs_shared_datavolume",    # bsd/vfs apfs mount option
    "wdt",                       # pexpert watchdog timeout (-1 = off)
    "no_slto_panic",             # osfmk/vm pmap (SLTO)
    "ml-timeout-lock-panic-scale",  # osfmk/ml lock timeout
}

# Parsed by the sibling framework repo (UniXNU-27 drivers), not by this one.
# Each entry names the file that reads it, so the claim stays checkable.
FRAMEWORK_PARSED = {
    "sheng-smp": "UniXNU-27/drivers/ArmCpu/ArmCpu.cpp",
    "armcpu-spi-ipi": "UniXNU-27/drivers/ArmCpu/ArmCpu.cpp",
}

# Files removed on 2026-11-08 (see docs/ABANDONED-CHANNELS.md). Any live mention
# of these in the 27 line's own config or series is a dangling reference.
REMOVED_2026_11_08 = [
    "muyu_usb_debug.c",
    "muyu_usb_protocol.h",
    "MuyuUsbDebug.c",
    "package/linux-log/",
    "collect-linux-readonly.py",
    "build-muyu-usb-debug.py",
    "build-muyu-log-reader.py",
    "zzzzzzzzzzz-muyu-debug.patch",
    "USB-DEBUG-PORT.md",
]

# Boot arguments known to be inert on this line, kept as comments on purpose.
# If one of these becomes live again, delete the entry - the audit must not
# whitelist a name that somebody re-wired.
KNOWN_INERT = {
    "muyu-ignition-trace": "u44 sysctl hook was never committed (IPADOS27-MIGRATION §6.58.2)",
    "bravearkdeferredconsole": "uke names it -ukedeferredconsole (uke-deferred-video-kprintf.patch)",
    "native-source-storage": "uke-side name; this repo gates on muyu-source-storage",
}

results: list[tuple[str, str, str]] = []  # (status, item, detail)


def ok(item: str, detail: str = "") -> None:
    results.append(("OK", item, detail))


def external(item: str, detail: str = "") -> None:
    results.append(("EXT", item, detail))


def fail(item: str, detail: str = "") -> None:
    results.append(("FAIL", item, detail))


def source_texts() -> dict[Path, str]:
    out: dict[Path, str] = {}
    for pat in ("package/*", "drivers/**/*", "patches/**/*"):
        for p in ROOT.glob(pat):
            if not p.is_file() or p.suffix not in {".c", ".h", ".cpp", ".mm", ".patch", ".py"}:
                continue
            try:
                out[p] = p.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                pass
    return out


def parsed_arg_names(texts: dict[Path, str]) -> dict[str, str]:
    """name -> first file that parses it."""
    found: dict[str, str] = {}
    for path, text in texts.items():
        for m in re.finditer(r'PE_parse_boot_argn\s*\(\s*"([^"]+)"', text):
            found.setdefault(m.group(1), str(path.relative_to(ROOT)))
        for m in re.finditer(r'TUNABLE\s*\([^,]+,[^,]+,\s*"([^"]+)"', text):
            found.setdefault(m.group(1), str(path.relative_to(ROOT)))
    return found


def check_board(board: dict) -> None:
    sv = board.get("schema_version")
    if sv == 2:
        ok("schema_version = 2")
    else:
        fail("schema_version", f"expected 2, got {sv!r}")

    prof = board.get("board", {}).get("os_profile", "")
    if "27" in prof:
        ok("os_profile", prof)
    else:
        fail("os_profile", f"expected the 27 profile, got {prof!r}")


def check_defconfig(cfg: str) -> None:
    # kernel.defconfig is a name the framework resolves against this repo's configs/.
    p = ROOT / "configs" / f"{cfg}.toml"
    p2 = ROOT / "configs" / cfg
    if p.exists() or p2.exists():
        ok("kernel.defconfig", cfg)
    else:
        have = sorted(x.name for x in (ROOT / "configs").glob("*defconfig"))
        fail("kernel.defconfig", f"{cfg!r} not found in configs/ (have: {', '.join(have)})")


def check_series(board: dict) -> None:
    series_refs = board.get("kernel", {}).get("patch_series", [])
    if not series_refs:
        fail("kernel.patch_series", "empty")
        return
    for ref in series_refs:
        layer, _, rel = ref.partition(":")
        if not rel:  # no "layer:" prefix -> plain repo path
            layer, rel = "device", ref
        path = ROOT / rel
        if layer != "device":
            external(f"patch_series {ref}", "arch/soc/os layer lives in the framework")
            continue
        if not path.exists():
            fail(f"patch_series {ref}", "declared but the file does not exist")
            continue
        ok(f"patch_series {ref}")
        listed = [
            ln.strip()
            for ln in path.read_text(encoding="utf-8").splitlines()
            if ln.strip() and not ln.lstrip().startswith("#")
        ]
        base = path.parent
        missing = [x for x in listed if not (base / x).exists()]
        if missing:
            fail(f"series entries in {rel}", "missing files: " + ", ".join(missing))
        else:
            ok(f"series entries in {rel}", f"{len(listed)} patches, all present")

        # Every patch on disk must be listed, or a patch added later silently does
        # not get applied while still looking like part of the tree.
        on_disk = sorted(p.name for p in base.glob("*.patch"))
        unlisted = [x for x in on_disk if x not in listed]
        if unlisted:
            fail(f"unlisted patches in {rel}", ", ".join(unlisted))
        elif on_disk == listed:
            ok(f"series order in {rel}", "identical to the alphabetical glob (no behaviour change)")
        else:
            ok(f"series order in {rel}", "explicit order differs from the glob (intentional?)")


def check_kernel_args(args: list[str], texts: dict[Path, str]) -> None:
    parsed = parsed_arg_names(texts)
    for raw in args:
        # XNU keeps the leading dash in the parse string for `-foo` style flags
        # (PE_parse_boot_argn("-bravearktextconsole", ...)), while a plain `foo=1`
        # is parsed without it. Try both spellings before declaring it unparsed.
        bare = raw.split("=")[0]
        tok = bare.lstrip("-")
        if bare in parsed or tok in parsed:
            continue
        if tok in XNU_NATIVE:
            continue
        if tok in FRAMEWORK_PARSED:
            external(f"kernel_arg {raw}", FRAMEWORK_PARSED[tok])
            continue
        if tok in KNOWN_INERT:
            fail(f"kernel_arg {raw}", f"inert but active: {KNOWN_INERT[tok]}")
            continue
        fail(f"kernel_arg {raw}", "no parser found in this repo, the framework, or the XNU whitelist")


def framework_roots() -> list[Path]:
    """Sibling framework checkouts that a device repo is allowed to reference."""
    return [p for p in (ROOT.parent / "UniXNU-27", ROOT.parent / "UniXNU") if p.is_dir()]


def check_tool_paths(board: dict) -> None:
    k = board.get("kernel", {})
    p = board.get("payload", {})
    refs = [
        ("kernel.prelink.tool", k.get("prelink", {}).get("tool")),
        ("payload.skip_setup_tool", p.get("skip_setup_tool")),
        ("payload.adt.tool", p.get("adt", {}).get("tool")),
        ("device_tree.source", board.get("device_tree", {}).get("source")),
    ]
    for label, ref in refs:
        if not ref:
            continue
        rel = ref.split(":", 1)[1] if ref.startswith("device:") else ref
        if (ROOT / rel).exists():
            ok(label, rel)
            continue
        # kits/ and most scripts/ are provided by the framework, not the device repo:
        # prelink tooling especially (UniXNU-27/kits/XnuPrelink/emit_prelink.py).
        hit = next((f for f in framework_roots() if (f / rel).exists()), None)
        if hit is not None:
            external(label, f"{rel} (provided by {hit.name})")
            continue
        fail(label, f"{rel} exists neither here nor in {', '.join(f.name for f in framework_roots()) or 'a framework checkout'}")


def check_dangling(board: dict, texts: dict[Path, str]) -> None:
    haystacks = {"board.spike27-live.toml": BOARD.read_text(encoding="utf-8")}
    ser = ROOT / "patches/xnu/series-spike27"
    if ser.exists():
        haystacks[str(ser.relative_to(ROOT))] = ser.read_text(encoding="utf-8")
    for name, text in haystacks.items():
        for needle in REMOVED_2026_11_08:
            if needle in text:
                # Mentions inside a comment that says "removed" are the archive trail.
                bad = [
                    ln
                    for ln in text.splitlines()
                    if needle in ln and "removed" not in ln.lower() and "删除" not in ln
                ]
                if bad:
                    fail(f"dangling ref in {name}", f"{needle}: {bad[0].strip()[:80]}")
    ok("dangling references", "no live mention of the 2026-11-08 removals")


def check_unused_parsers(texts: dict[Path, str], args: list[str]) -> None:
    """A parser nobody passes an argument for is dead code, not a bug - report only."""
    parsed = parsed_arg_names(texts)
    live = {a.lstrip("-").split("=")[0] for a in args}
    unused = sorted(n for n in parsed if n not in live and not n.startswith("-"))
    if unused:
        external("parsers with no kernel_arg", f"{len(unused)} (informational): " + ", ".join(unused[:6]))


def check_embedded_copies() -> None:
    """The wip patches carry whole copies of drivers/MuyuDebug/*.c.

    Two copies of one file is the second classic hand-over fault: somebody edits
    one side, the other silently keeps the old behaviour, and which one the build
    sees depends on whether the patch was applied. They must stay byte-identical.
    """
    wip = sorted((ROOT / "patches/xnu/wip").glob("*.patch"))
    if not wip:
        external("embedded driver copies", "no wip patches")
        return
    checked = 0
    for pf in wip:
        lines = pf.read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            m = re.match(r"diff --git a/bsd/dev/arm/(\S+) b/bsd/dev/arm/\1", line)
            if not m:
                continue
            name = m.group(1)
            disk = ROOT / "drivers/MuyuDebug" / name
            if not disk.exists():
                continue  # not a mirrored file
            body: list[str] = []
            for nxt in lines[i + 1 :]:
                if nxt.startswith("diff --git"):
                    break
                if nxt.startswith("+") and not nxt.startswith("+++"):
                    body.append(nxt[1:])
            embedded = "\n".join(body) + "\n"
            checked += 1
            if embedded == disk.read_text(encoding="utf-8"):
                ok(f"copy in {pf.name}", f"bsd/dev/arm/{name} == drivers/MuyuDebug/{name}")
            else:
                a, b = embedded.splitlines(), disk.read_text(encoding="utf-8").splitlines()
                diff = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
                fail(
                    f"copy in {pf.name}",
                    f"bsd/dev/arm/{name} drifted from drivers/MuyuDebug/{name} ({diff} lines differ)",
                )
    if not checked:
        external("embedded driver copies", "no mirrored files found")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true", help="only FAIL lines and the summary")
    ns = ap.parse_args()

    board = tomllib.loads(BOARD.read_text(encoding="utf-8"))
    texts = source_texts()

    check_board(board)
    check_defconfig(board.get("kernel", {}).get("defconfig", ""))
    check_series(board)
    args = board.get("payload", {}).get("kernel_args", [])
    check_kernel_args(args, texts)
    check_tool_paths(board)
    check_dangling(board, texts)
    check_embedded_copies()
    check_unused_parsers(texts, args)

    width = max(len(i) for _, i, _ in results)
    for status, item, detail in results:
        if ns.quiet and status != "FAIL":
            continue
        line = f"{status:4s}  {item:<{width}}  {detail}"
        print(line.rstrip())

    n_fail = sum(1 for s, _, _ in results if s == "FAIL")
    n_ok = sum(1 for s, _, _ in results if s == "OK")
    n_ext = sum(1 for s, _, _ in results if s == "EXT")
    print(f"\n{n_ok} ok, {n_ext} external, {n_fail} fail  (kernel_args: {len(args)})")
    if n_fail:
        print("FAIL means a silent disconnect: the build will succeed and the option will do nothing.")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
