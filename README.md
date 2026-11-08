# UniXNU-muyu

Device repository for running [UniXNU](https://github.com/UniXNU/UniXNU) on the
**Xiaomi Pad 7 Pro** (codename `muyu`, Qualcomm SM8635 "cliffs", Adreno 735).

Two OS lines live here, both with an iPadOS root on `userdata`:

| Line | Board file | State |
| --- | --- | --- |
| iPadOS **27.0.1** (24A446) | `board.spike27-live.toml` | main line — userland runs from the shared cache; blocked on dyld's slide |
| iPadOS **17.7.11** (21H461) | `board.toml` | fallback — boots to the desktop, ~1.4 fps; GPU paused |

**Start with [`STATUS.md`](STATUS.md)** — it is the hand-off, and it says what is currently
on the tablet, which is not what either line's progress suggests.

Derived from [UniXNU-braveark](https://github.com/tattwamashi/UniXNU-braveark)
(same SM8635 SoC). Identifiers that came from that shared SM8635 work keep their
`braveark` names (`drivers/BravearkPreflight`, `package/BravearkLastLog.c`, the
`-bravearktextconsole` boot argument, the `BRAVEARK:` log prefix and two XNU
patches); names specific to this device use `muyu` (`kits/muyu`, the `muyu-*`
boot arguments). The board file in use is the authority for the device identity — per line, as
above.

## Status

| Stage | State |
| --- | --- |
| UEFI (fastboot boot) | works |
| UniXNU menu | works |
| Payload (`vendor_boot` ramdisk) | works |
| DarwinBoot / XNU | works on both lines: 17 reaches the desktop, 27 reaches launchd running boot tasks |
| Display | works, CPU-composed framebuffer (~1.4 fps on the 17 line) |
| Input | physical buttons and the Novatek touch controller as native IOServices |
| GPU | **Adreno 735 GMU cold-starts and completes the full HFI startup** (2026-10-30); no CP/Metal yet — paused 2026-11-02 |
| Diagnostics | **panel only** since 2026-11-08: the USB debug console and the Linux/pstore reader are both out — and their code was deleted the same day (`docs/ABANDONED-CHANNELS.md`) |
| Thermal / DVFS | not implemented on either line; one read-only TSENS survey is written and has never been run |

Two things are tracked outside this table because they change every session: the flashed state of
the device and the current blocker. Both are in [`STATUS.md`](STATUS.md), and the per-area
evidence is in [`porting.toml`](porting.toml) — 13 areas, each with its status, findings and next
action.

### 27 line: the current blocker

dyld never requests a slide, so threaded pointers inside shared-cache pages are never rebased and
the first job that touches one dies. The shared-cache chain itself is done (16 KiB pager, 47-bit
`T0SZ_USER`, `map_with_linking_np` page-size fix); the evidence is in
`docs/HANDOFF-2026-11-07.md` §12 and the panel readouts transcribed in
`docs/HANDOFF-2026-11-08.md`.


## GPU

GPU work is **paused** (2026-11-02, maintainer's decision) in favour of getting the 27 line's
userland up. Everything below was hardware-verified before the pause, and the whole chain is kept
so it does not have to be re-derived:

- `docs/GPU-CLIFFS-MMIO.md` — the cliffs register map (this SoC is **not** register
  compatible with SM8550/SM8650; the devicetree node names are leftovers).
- `docs/GPU-GMU-MAP.md` — the GMU platform description derived from the device DTB.
- `docs/GPU-PROBE-RUNBOOK.md` — probe modes 1–17, expected values and judgement trees.
- `docs/GPU-PORT.md` — the phased plan and what is still missing.

Milestones reached on hardware:

1. CX power domain and the GPUCC clock tree (modes 4–6).
2. GPU SMMU identity and the GMU's stage-1 SMMU context (mode 9).
3. The five GMU memory blocks and the A735 firmware staged with its SHA verified (modes 7–8).
4. **GMU cold start + full HFI startup**: `cold=0 ph=7 proto=0 fw=0x00000100 hfi=0x00000001`
   (mode 11, 2026-10-30; the HFI startup's seven commands are acknowledged by the firmware).

Two facts worth keeping:

- The GMU firmware chip id is **`0x070b0000`**, recovered from the CM3 image itself
  (it compares the `GMU_GENERAL_10` mirror against that literal). The catalog-less
  formula gives `0x4303b000`, which the firmware rejects (`FW_INIT_RESULT=0x901`).
- The board OPP/BCM policy is generated from the device's own downstream devicetree
  (`qcom,gpu-pwrup-levels`-style nodes), not from another SKU.

Known blocker: GX rail power-up. `gpu_cc_gx_gdsc`'s parent supply is the RPMh ARC
voter `pm_v6c_s6_gfx_voter_level`; this port does not drive the RSC yet, so
`CFG_GDSCR.POWER_UP_COMPLETE` never sets.

## Build and boot

The build and flashing procedures are the UniXNU ones; this repository only adds the
board configuration and the device code under `package/`, `drivers/`, `kits/muyu` and
`patches/`. Two things to know before the first build:

**`unixnu` reads `board.toml` and nothing else.** The 27 line's configuration is archived as
`board.spike27-live.toml` (schema 2, the file that built the current images); the tracked
`board.toml` is the schema-1 21H461 template. Pick a line by putting its file in place:

```bash
cp board.spike27-live.toml board.toml   # 27 line
git checkout board.toml                 # back to the 17 line
```

**Every build carries a generated tag** (`27-scr-1108a`), not a hand-typed `u` number — the tags
are written into 23 separate `printf`s historically, which is why a u155 image once reported
u151. See `docs/BUILD-NAMING.md`:

```bash
python3 scripts/build-tag.py next --purpose scr --note "what this round is for"
```

Then, for the 27 line:

```bash
cd UniXNU-27 && python3 unixnu build kernel -D ../UniXNU-muyu \
    --kernelcache out/ipsw/24A446/kernelcache.decompressed
python3 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
    --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
    --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr
python3 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
    --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_muyu.img
```

For the 17 line the same three steps run with `--kernelcache out/ipsw/21H461/kernelcache.decompressed`.

Requirements: unlocked bootloader, a wall charger for the experiments, and a full
stock ROM for recovery. The iPadOS root image is **not** part of this repository.

## Documentation

| Document | What it covers |
| --- | --- |
| `STATUS.md` | the hand-off: current device state, current blocker, next steps in order |
| `porting.toml` | 13 areas with status, findings, limits, next action and evidence |
| `docs/HANDOFF-2026-11-08.md` | the last session, reconstructed from artefacts when the narrative was missing |
| `docs/HANDOFF-2026-11-07.md` | ~4700 lines: 60 rounds of panel readouts and the reasoning between them |
| `docs/SCREEN-ONLY.md` | why USB and pstore are out, and the five-step re-plant of the 17-line screen stack |
| `docs/ABANDONED-CHANNELS.md` | the two removed log channels: what was measured, what was deleted, how to get it back |
| `docs/BUILD-NAMING.md` | the build tag scheme |
| `docs/GPU-PROBE-RUNBOOK.md` | probe modes 1–17, expected values, judgement trees |
| `docs/GPU-CLIFFS-MMIO.md` | the cliffs register map — this SoC is **not** register-compatible with SM8550/SM8650 |
| `docs/THERMAL-PORT.md`, `docs/TOUCH-PORT.md` | the peripheral ports that are still live |
| `docs/UKE-CODE-AUDIT.md`, `docs/UKE-FULL-WALKTHROUGH.md` | what the sibling SM7675 port (`UniXNU-uke`) has that this one lacks, and vice versa |

## Hardware data

- SM8635 "cliffs", 12 GB RAM (device dependent), UFS, 4 KiB pages.
- Display 1840×2880; Novatek `NVT-ts-spi` on GENI SPI `0xa90000`.
- GPU: Adreno 735, `qcom,adreno-gpu-gen7-11-0`, GMU `gen71100` firmware.
- `userdata` is the iPadOS root; the Android user data is gone.

## Safety

This is an experimental port on a device with an unlocked bootloader. It has no
thermal management and no DVFS; long experiments raise the temperature. Read
`AGENTS.md` in the upstream workspace before flashing anything: it lists the
operations that have previously bricked or wedged this tablet.

## License

Same license as UniXNU (BSD-2-Clause-Patent), see `LICENSE`.
