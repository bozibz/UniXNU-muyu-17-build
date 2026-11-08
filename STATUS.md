# Status and how to continue

**Last updated:** 2026-11-08 · **Device:** Xiaomi Pad 7 Pro (`muyu`, SM8635 "cliffs", Adreno 735)
**Lines in flight:** iPadOS 27.0.1 (24A446) — main line · iPadOS 17.7.11 (21H461) — fallback that still reaches the desktop
**Ledger:** `porting.toml` — 13 areas, every status backed by a citation

This file is the hand-off. Read it first, then:

| Document | Why |
| --- | --- |
| `docs/HANDOFF-2026-11-08.md` | the last session: eight rebuilt rounds reconstructed from artefacts, and the restore to stock |
| `docs/SCREEN-ONLY.md` | where diagnostics are going — the panel is now the only channel |
| `docs/BUILD-NAMING.md` | how builds are named and tagged from now on |
| `porting.toml` | per-area state, findings, limits and the next action for each |

## Where the device is right now

Stock `boot` / `vendor_boot` / `recovery` were flashed back at **2026-11-08 16:18** (local) and
nothing has been flashed since. Why is not recorded — `docs/HANDOFF-2026-11-08.md` §7 marks it
`unknown`, and it is the one open question in that file.

Nothing has been verified on the tablet after that restore. `userdata` still holds the iPadOS
root, so stock Android has no valid data partition to mount: treat the device as **parked**, not
recovered. Continuing means flashing a matched `boot` + `vendor_boot` pair for the same build to
slot a (see *Build and flash*).

## The two lines

| Line | Reaches | Blocker |
| --- | --- | --- |
| **27.0.1 (24A446)** — main | launchd runs boot tasks; `apfs` mounts `SystemCryptex`; the owner cache enumerates `dyld_shared_cache_arm64e` | dyld never requests a slide, so threaded pointers inside cache pages are never rebased |
| **17.7.11 (21H461)** — fallback | the desktop, CPU-composed at ~1.4 fps; touch works | GPU GX rail; then thermal, which has never been implemented |

The 17 line exists because it is a place to stand: it boots a working userland on the same
kernel and the same panel, so anything that breaks the 27 line can be bisected against it.

## 27 line: what works

The shared-cache chain took roughly thirty rounds and four real fixes; they are the expensive
part of this port, so they are listed rather than summarised:

1. **`map_with_linking_np` rejected 16 KiB device pages** — the kernel compared
   `mwli_page_size != PAGE_SIZE`; the uke port compares `!= 4 * PAGE_SIZE`. (u91)
2. **dyld pager fixups at 16 KiB** — `vm_dyld_pager.c` carried over from uke: `seg->page_size`,
   `page_shift`, whole-page assembly, key 0 signed as well. (u92)
3. **`T0SZ_USER` at 47 bits** — the turn: the pager went 63 → 822 pages, `mwl` calls 1 → 24,
   and launchd started running boot tasks. (u98)
4. **Chain traversal verified correct**: `rb` from 21.7 k to 370 k rebases, `slid_error=0`. (u95/u101)

The 14:36 panel readout on u155 (transcribed in `docs/HANDOFF-2026-11-08.md` §5) shows the
further of the two: `apfs` mounting `SystemCryptex` on pid 1's request, owner cache walking the
dyld shared-cache file list. The kernel main line has been in userland for a while — the
diagnostic USB never was a prerequisite for that.

## 27 line: the current blocker

**dyld does not ask for a slide.** Cache pages therefore keep unresolved threaded pointers and
the first job that touches one dies. The last build (`kernel/kernel.json`, 16:13) also reports
`loadable=false` with `IOHIDFamily` missing — and u159 was flashed anyway. That decision is
recorded as a finding in `porting.toml` under `configuration`, because nobody wrote down why.

Two facts that bound the search: `-disable_aslr` is still in the live config (the uke line does
not use it, and it has not been re-tested without), and `DoNotUseSEP` from the `is-qian` fork is
still unwired (`[payload.adt] filesystems = false`) — without it an unencrypted Data-role volume
is refused once `/private/var` has to be mounted for real.

## 17 line: what works, and where it stopped

| Area | State |
| --- | --- |
| UEFI (`fastboot boot`), UniXNU menu, payload from `vendor_boot` | works |
| XNU → BSD init → iPadOS desktop | works |
| Display | works, CPU-composed (~1.4 fps) |
| Physical buttons, Novatek touch (`muyu-touch-drv=1`) | works (native IOService) |
| GPU: CX power, GPUCC clock tree, GPU SMMU | works (hardware-verified) |
| GPU: GMU cold start + full HFI startup | **works** (2026-10-30, `cold=0 ph=7 proto=0`) |
| GPU: GX rail, CP, command submission, Metal | **not implemented** — paused 2026-11-02 by the maintainer's decision |
| Thermal sensors (TSENS) | survey written and built (`muyu-thermal=<n>`); **never run on the device** |
| Thermal policy, DVFS, watchdog | **not implemented** |

The GX rail is the reason the GPU stopped: `gpu_cc_gx_gdsc`'s parent supply is the RPMh ARC
voter `pm_v6c_s6_gfx_voter_level`, so `CFG_GDSCR.POWER_UP_COMPLETE` (bit 16) never sets. A
CPU-side RSC sender exists (`package/MuyuRpmhRsc.{h,c}`, probe mode 17, host-tested by
`scripts/check-muyu-rpmh-host.py`) and was flashed, but **the on-device readout was never
collected**. Nine register tables and probe modes 1–17 are still in `docs/GPU-PROBE-RUNBOOK.md`;
nothing needs rediscovering, it needs one flash.

## Diagnostics: the panel is the only channel

Decided 2026-11-08 (`docs/SCREEN-ONLY.md`):

- **USB CDC ACM — abandoned.** About 29 rounds (u128–u159). `DEPCFG` is accepted and never
  starts executing; the UTMI/eUSB2 PHY appears to sit in suspend. Ordered SUSPHY clears after
  endpoint commands came too late.
- **Linux init_boot + pstore — built, never flashed, now shelved.** v1's hand-rolled boot header
  was rejected by ABL at 14:59; v3/v4 use the stock `init_boot` header and read ramoops at
  `0xa7000000`. v1–v4 appear in no ledger line at all.
- **Panel — device-verified, and the only channel that has ever produced conclusions.**

**Both dead channels were then removed, not just declared dead** (2026-11-08, `docs/ABANDONED-CHANNELS.md`):
the CDC ACM driver and its protocol header, the 17-line `MuyuUsbDebug.c` copy, the whole
`package/linux-log/` reader, both packer scripts, the `zzzzzzzzzzz-muyu-debug.patch` that registered
`muyu_usb.c`/`muyu_pstore.c`, the `libMuyuUsbDebug.a` force-load hook and the `-DXNU_MUYU_USB=1`
define in the legacy build, the three `muyu_usb*` boot arguments in both board files, and the last
copy of `muyu_usb_debug.c` inside the wip screen patch (its `files.arm64` registration went with it).
What deliberately survives: `muyu_debug.c`/`muyu_log_ring.h` (the console ring the panel uses), the
guarded `weak` hooks in `muyu_vc_status_paint()`, and the dated handoff narratives. Note that
`"sheng-log=1"` is **not** part of this removal — it is implemented by `package/BootLog.c` +
`package/BravearkLastLog.c`, not by the deleted `muyu_pstore.c`.

Everything the screen needs already exists on the 17 line: a single-source build tag, automatic
page rotation, previous-panic replay, crash-report painting, a watchdog. It was lost on the 27
line at the link and config layer — its `kernel.macho` keeps only `_boot_progress_poll`/`_tag`,
no `STACK64`, and 25 bootargs against the 17 line's 68. `docs/SCREEN-ONLY.md` §4 is a five-step
re-plant, not a rewrite.

## Build and flash

**Two board files, and this trips people up.** `unixnu` reads `<device>/board.toml` and nothing
else:

| File | Schema | Line |
| --- | --- | --- |
| `board.toml` | 1 | 21H461 (the historical template) |
| `board.spike27-live.toml` | 2, 703 lines | 24A446 — byte-identical to the `/tmp/spike27` copy that built today's images |

So a 27-line build starts by putting the live config in place (`cp board.spike27-live.toml
board.toml`), and `git checkout board.toml` puts the 17 line back.

Builds are tagged `<line>-<purpose>-<MMDD><letter>` — `27-scr-1108a`. The tag is generated, not
typed into `printf`s (that is how a u155 image once read u151):

```bash
python3 scripts/build-tag.py next --purpose scr --note "what this round is for"
python3 scripts/build-tag.py show        # the tag this build will carry
```

Then the three steps, with the 24A446 kernelcache:

```bash
cd UniXNU-27 && python3 unixnu build kernel -D ../UniXNU-muyu \
    --kernelcache out/ipsw/24A446/kernelcache.decompressed
python3 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
    --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
    --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr
python3 ../UniXNU-muyu/scripts/build-tag.py check out/muyu/kernel/kernel.macho
```

`vendor_boot` is assembled by `scripts/patch-vendor-boot.py` from `fw/vendor_boot_lite.img`; the
full sequence, including the `set_active a` and the pair-flashing rule, is in the workspace
`AGENTS.md`. The 17-line commands (with `--kernelcache out/ipsw/21H461/...`) are unchanged and
still work.

## Keeping the 27 line clean

This line was carried by many agents in sequence, so the failure mode is never a build error — it
is a silent disconnect: a series file that was declared but never committed, a defconfig that does
not exist, a boot argument whose parser was renamed or never committed. All of it builds fine and
all of it does nothing. `scripts/audit-27-config.py` checks for exactly those:

```bash
python3 scripts/audit-27-config.py          # exit 1 on any disconnect
```

What it enforces, and why each one earns its place:

| Check | The fault it catches |
| --- | --- |
| `patch_series` resolves, and every device `.patch` is either listed or in `backlog/` | a series that does not exist (the build stops) and patches that look live but are never applied |
| `os:` entries resolve through the framework | a relative hop that only works in one checkout |
| `kernel.defconfig` exists in `configs/` | a build silently falling back to another config |
| every `kernel_args` entry has a parser (this repo, the framework, or a justified XNU-native list) | the inert-option class of bug |
| `device:` / `scripts/` / `kits/` references resolve, here or in a framework checkout | paths to files deleted three hand-overs ago |
| no live mention of the 2026-11-08 removals | dangling references to the removed USB/Linux channels |
| wip patch copies == `drivers/MuyuDebug/` | two copies of one driver drifting apart |
| the two framework checkouts agree on `patches/xnu-12377` | the same series producing two different kernels |

State at 2026-11-08 21:40: **12 ok, 6 external, 0 fail, 33 kernel_args.**

The first fault was the big one: **this repo could not build its own 27 line.** `patches.py` does
not glob and does not fall back — a declared series that is missing raises `patch series missing`
and stops — and `patches/xnu/series-spike27` had never been committed. Every u-round was built
from `/tmp/spike27`. That copy still exists, so the series was rebuilt from it and cross-checked
against the framework's own record of what it applied (`xnu-patches.list`, 16:12): **72 entries =
67 from the framework's `xnu-12377` layer + 5 device patches**, in u159's order. The 67 use the
same `os:../../../patches/xnu-12377/` relative hop the OS profile series uses, so they resolve in
either framework checkout.

The second was quieter and matters more for the next round: the repo carried **19** device patches
while the build applied **5**. The other 16 are the 17 line's patches (video console, last-panic
replay, watchdog, the launchd traces) — copied in, never listed, never applied. That is the same
fact as §3's `STACK64`-missing finding, seen from the file side. They are now in
`patches/xnu/backlog/` with a README, ready for the screen re-plant.

The rest: the `configs/muyu-27_defconfig` reference now points at the `muyu-userland_defconfig`
the board actually names, and four inert arguments (`native-source-storage`,
`-bravearkdeferredconsole`, `muyu-ignition-trace`, `braveark-hold-reboot`) became comments with the
reason and the real name attached.

**`braveark-hold-reboot` is the one to notice.** It was live on the 17 line (u88 parked on its
panic screen), and on the 27 line it does nothing: its two patches are in `backlog/`. So a 27-line
panic reboots and the panel loses the text — the single thing a panel-only channel cannot afford.
Wiring those two patches back in (they are a pair) is the first item of the screen re-plant.

One deviation is recorded, not fixed, because it is framework work: the two framework checkouts
disagree on three of the 67 (`offline-owner-boot-manifest`, `offline-owner-systemos-mount`,
`zzz-boot-progress`, core-repo copies newer), so the same series builds differently under
`UniXNU` and `UniXNU-27`. The audit reports it.

## Next steps, in order

1. **Answer `docs/HANDOFF-2026-11-08.md` §7.** One line: why stock images went back at 16:18.
2. **Re-plant the screen stack** (`docs/SCREEN-ONLY.md` §4). Turn on `CONFIG_XNU_BOOT_PROGRESS`
   and `CONFIG_XNU_NATIVE_LEGACY_DISPLAY`, wire the four units back into the fileset, set the
   build tag, and make the `check-boot-*.py`
   set a gate before flashing. (The `muyu_usb_debug.c` registration in `bsd/conf/files.arm64` is
   already gone — removed with the driver on 2026-11-08.) Until the panel can state its own version
   and replay the previous panic, every round costs a photograph and a guess.
3. **One 27-line build under the new instrumentation**, `27-scr-…`, flashed as a pair, and read
   the panel. The target is not progress — it is confirming the dyld-slide blocker still
   reproduces when the rig can actually report.
4. **Then the blocker itself**: why dyld never requests a slide, and which job first touches an
   unrebased threaded pointer.
5. **Thermal is still unaddressed on both lines.** No zone, no DVFS, `wdt=-1`: the tablet cannot
   protect itself from sustained load. One read-only survey flash (`muyu-thermal=2`) settles
   whether the sensors are readable at all.

GPU stays paused unless the maintainer asks; the tables, the probe modes and the GX result are
all still here.

## Hard limits to know

- No thermal management and no DVFS; long experiments heat the tablet.
- `memory` is `unknown` in `porting.toml` on purpose — there is no citable evidence yet, so it is
  not promoted to `implemented`.
- `ShengGmu.c`'s poll caps a single wait at 10 ms; the A735 firmware finishes well inside it.
- ACD and IFPC are off (they need AOSS/ACD and power-collapse prerequisites).
- GPLL0's GPU vote (GCC `0x52000` bit 15/16) does not stick; the GMU uses PLL1, so it only
  matters for hub/axi votes.

## Where things live

| Path | What |
| --- | --- |
| `fw/` | 353 images, including `fw/stock/` (the restore set) and the `vendor_boot_27-u1xx.img` series |
| `logs/flash-ledger.tsv` | 2651 lines: every flash, its sha256, and what was on screen |
| `logs/*.jpg` | panel photographs — the primary evidence of the whole project |
| `docs/` | 21 documents; `HANDOFF-2026-11-0{7,8}.md` are the two long ones |
| `drivers/MuyuDebug/` | the screen/USB sources, rescued out of `.cache/xnu-work` on 2026-11-08 |
| `patches/xnu/wip/` | `muyu-screen-wip-20261108.patch` — the full diff those sources came from, not yet split |
| `/tmp/spike27` and `/tmp/spike27-out` | the live build inputs and tree for the 27 line; **volatile**, and the reason `board.spike27-live.toml` was archived |

## Syncing this repository

Two remotes: `muyu` → `yehua123-oos/UniXNU-muyu` (tracked by `main`), `origin` →
`tattwamashi/UniXNU-braveark` (the upstream this port derives from). Push goes to `muyu`:

```bash
git -c credential.helper=osxkeychain push muyu main
```

The credential lives in the macOS login keychain (`github.com`, account `yehua123-oos`). Note
that the 2026-11-07 handoff (§12.7) records a token being echoed in a session and recommends
revoking it — if that was done, store a fresh one before pushing.

## If you want to take over

Open an issue on this repository. Everything needed to reproduce the state is here: the register
tables, the probe modes, the generator scripts and their checks, the flashed-image hashes, the
photographs, and the reasoning behind each decision — including the ones that turned out wrong.
