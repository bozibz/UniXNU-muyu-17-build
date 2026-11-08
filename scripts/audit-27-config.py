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

# Boot arguments known to be inert on this line. Empty on purpose: the three that
# were here (native-source-storage, -bravearkdeferredconsole, muyu-ignition-trace)
# became comments on 2026-11-08, so they no longer reach this check. If one comes
# back, the check must FAIL — do not re-add an entry just to make it pass.
KNOWN_INERT: dict[str, str] = {}

results: list[tuple[str, str, str]] = []  # (status, item, detail)


def ok(item: str, detail: str = "") -> None:
    results.append(("OK", item, detail))


def external(item: str, detail: str = "") -> None:
    results.append(("EXT", item, detail))


def fail(item: str, detail: str = "") -> None:
    results.append(("FAIL", item, detail))


def source_texts() -> dict[Path, str]:
    """Source that actually reaches the kernel.

    Deliberately excludes patches/xnu/wip/ (an archive of unfinished tree state, never
    applied) and patches/xnu/ (the 17 line's directory: listed in this line's series?
    no). A parser found only in one of those would be a false positive — the option is
    still inert. This is the difference between "a patch in the tree parses it" and
    "the kernel parses it".
    """
    out: dict[Path, str] = {}
    for pat in ("package/*", "drivers/**/*", "patches/**/*"):
        for p in ROOT.glob(pat):
            if not p.is_file() or p.suffix not in {".c", ".h", ".cpp", ".mm", ".patch", ".py"}:
                continue
            parts = p.relative_to(ROOT).parts
            # patches/xnu/ belongs to the 17 line and patches/xnu/wip/ is an archive; neither
            # is applied here, so neither may satisfy this line's argument check.
            if parts[:2] == ("patches", "xnu"):
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


def _series_entries(path: Path) -> list[str]:
    return [
        ln.strip()
        for ln in path.read_text(encoding="utf-8").splitlines()
        if ln.strip() and not ln.lstrip().startswith("#")
    ]


def _resolve_layer(entry: str, board: dict) -> tuple[Path | None, str]:
    """Resolve one "layer:relative" series reference to a file.

    Deliberately re-implemented from the framework's rules rather than imported: this
    must keep working from a device checkout that has no framework on sys.path.
    """
    layer, sep, rel = entry.partition(":")
    if not sep:
        return None, "no layer prefix"
    if layer == "device":
        return ROOT / rel, ""
    if layer == "arch":
        arch = "arm64e"
        for fw in framework_roots():
            prof = fw / "profiles/os" / board["board"]["os_profile"] / "profile.toml"
            if prof.is_file():
                arch = tomllib.loads(prof.read_text(encoding="utf-8")).get("os", {}).get("arch", arch)
                break
        hits = [fw / "profiles/arch" / arch / rel for fw in framework_roots()
                if (fw / "profiles/arch" / arch / rel).is_file()]
        # More than one hit is normal: both framework checkouts carry the layer, and
        # check_framework_divergence() reports where they disagree.
        return (hits[0], "") if hits else (None, f"no {arch} arch series in the framework checkouts")
    if layer == "os":
        hits = [fw / "profiles/os" / board["board"]["os_profile"] / rel for fw in framework_roots()
                if (fw / "profiles/os" / board["board"]["os_profile"] / rel).is_file()]
        return (hits[0], "") if hits else (None, "no os series in the framework checkouts")
    if layer == "soc":
        hits = [p / rel for p in sorted(ROOT.parent.glob("UniXNU-*-common")) if (p / rel).is_file()]
        return (hits[0], "") if len(hits) == 1 else (None, f"soc layer not found in {len(hits)} repos")
    return None, f"unknown layer {layer!r}"


def check_series(board: dict) -> None:
    """The series is the only source of truth for what gets applied.

    Structure being enforced (identical to the 17 line, and to UniXNU-uke): each layer owns
    a directory, the device layer's series is nothing but the listing of patches/xnu-27/, and
    the order is the filename order. patches.py does not glob and does not fall back — a
    declared series that is missing stops the build — so existence and resolution matter
    more than any opinion about ordering.
    """
    series_refs = board.get("kernel", {}).get("patch_series", [])
    if not series_refs:
        fail("kernel.patch_series", "empty")
        return

    total: list[Path] = []
    for ref in series_refs:
        path, why = _resolve_layer(ref, board)
        if path is None or not path.is_file():
            fail(f"patch_series {ref}", why or f"declared but missing: {ref}")
            continue
        entries = _series_entries(path)
        resolved: list[Path] = []
        for entry in entries:
            sub, sub_why = _resolve_layer(entry, board) if ":" in entry else (path.parent / entry, "")
            if sub is None or not sub.is_file():
                fail(f"series entry {entry}", sub_why or f"missing under {path.parent}")
                continue
            resolved.append(sub.resolve())
        if path.parent == ROOT / "patches/xnu-27":
            ok(f"patch_series {ref}", f"{len(resolved)} device patches, all present")
        else:
            external(f"patch_series {ref}", f"{len(resolved)} patches from {path.parent.relative_to(ROOT.parent)}")
        total += resolved

    dupes = sorted({p.name for p in total if total.count(p) > 1})
    if dupes:
        fail("duplicate series entries", ", ".join(dupes) + " (patches.py raises on this)")
    else:
        ok("applied patch set", f"{len(total)} patches, no duplicates")

    # The device layer: series == directory, in filename order. Anything sitting next to the
    # series but unlisted is the silent failure this check exists to stop — present, looks
    # live, never applied.
    dev = ROOT / "patches/xnu-27"
    if not dev.is_dir():
        fail("patches/xnu-27/", "missing: the 27 line's device patch directory")
        return
    listed = [ln for ln in _series_entries(dev / "series") if ":" not in ln] if (dev / "series").is_file() else []
    on_disk = sorted(p.name for p in dev.glob("*.patch"))
    if not (dev / "series").is_file():
        fail("patches/xnu-27/series", "missing")
    elif listed != on_disk:
        extra = [x for x in on_disk if x not in listed]
        gone = [x for x in listed if x not in on_disk]
        detail = []
        if extra:
            detail.append("on disk but not listed: " + ", ".join(extra))
        if gone:
            detail.append("listed but absent: " + ", ".join(gone))
        if not detail:
            detail.append("same set, different order (the 17 line applies in filename order)")
        fail("patches/xnu-27/series vs directory", "; ".join(detail))
    else:
        ok("patches/xnu-27/series vs directory", f"{len(on_disk)} patches, name order")


def check_line_separation() -> None:
    """Two lines, two directories — and the 17-line directory is applied by a glob.

    A schema_version 1 board (the 17 line's board.toml) does not name a series: the build
    globs patches/xnu/*.patch and applies every one of them. So a 27-line patch parked in
    there is applied to a 17-line kernel, and moving a 17-line patch out of it silently
    drops it from that kernel. Neither shows up as a build error.
    """
    a = ROOT / "patches/xnu"
    b = ROOT / "patches/xnu-27"
    if not b.is_dir():
        return
    shared = sorted({p.name for p in a.glob("*.patch")} & {p.name for p in b.glob("*.patch")})
    if shared:
        fail("patch directories overlap", ", ".join(shared) + " exists in both lines' directories")
    else:
        ok("line separation", f"{len(list(a.glob('*.patch')))} patches for the 17 line (globbed), "
                               f"{len(list(b.glob('*.patch')))} for the 27 line (listed)")



def check_framework_divergence(board: dict) -> None:
    """Two framework checkouts, one series: if their copies differ, so do the builds."""
    roots = framework_roots()
    if len(roots) < 2:
        external("framework divergence", "only one framework checkout available")
        return
    target = "patches/xnu-12377"
    base = roots[0] / target
    if not base.is_dir():
        external("framework divergence", f"{roots[0].name} has no {target}")
        return
    diffs = []
    for f in sorted(base.glob("*.patch")):
        for other in roots[1:]:
            peer = other / target / f.name
            if not peer.exists():
                diffs.append(f"{f.name} missing in {other.name}")
            elif peer.read_bytes() != f.read_bytes():
                diffs.append(f"{f.name} ({roots[0].name} vs {other.name})")
    if diffs:
        external(
            "framework divergence",
            f"{len(diffs)} differ: " + "; ".join(diffs[:4]) + (" …" if len(diffs) > 4 else ""),
        )
    else:
        ok("framework divergence", f"{roots[0].name} and {roots[1].name} agree on {target}")


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
    ser = ROOT / "patches/xnu-27/series"
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
    check_line_separation()
    check_framework_divergence(board)
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
