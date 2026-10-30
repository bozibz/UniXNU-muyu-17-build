# UniXNU-muyu

Device repository for running [UniXNU](https://github.com/UniXNU/UniXNU) on the
**Xiaomi Pad 7 Pro** (codename `muyu`, Qualcomm SM8635 "cliffs", Adreno 735),
with an iPadOS 17.7.11 (21H461) root on `userdata`.

Derived from [UniXNU-braveark](https://github.com/tattwamashi/UniXNU-braveark)
(same SM8635 SoC). Identifiers that came from that shared SM8635 work keep their
`braveark` names (`drivers/BravearkPreflight`, `package/BravearkLastLog.c`, the
`-bravearktextconsole` boot argument, the `BRAVEARK:` log prefix and two XNU
patches); names specific to this device use `muyu` (`kits/muyu`, the `muyu-*`
boot arguments). `board.toml` is the authority for the device identity.

## Status

| Stage | State |
| --- | --- |
| UEFI (fastboot boot) | works |
| UniXNU menu | works |
| Payload (`vendor_boot` ramdisk) | works |
| DarwinBoot / XNU | works: reaches BSD init, boots iPadOS to the desktop |
| Display | works, CPU-composed framebuffer (~1.4 fps) |
| Input | physical buttons and the Novatek touch controller as native IOServices |
| GPU | **Adreno 735 GMU cold-starts and completes the full HFI startup** (2026-10-30); no CP/Metal yet |

## GPU

The GPU work is the current focus. All of it is hardware-verified except the last
step, and the evidence is in the repository:

- `docs/GPU-CLIFFS-MMIO.md` — the cliffs register map (this SoC is **not** register
  compatible with SM8550/SM8650; the devicetree node names are leftovers).
- `docs/GPU-GMU-MAP.md` — the GMU platform description derived from the device DTB.
- `docs/GPU-PROBE-RUNBOOK.md` — probe modes 1–16, expected values and judgement trees.
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
board configuration (`board.toml`) and the device code under `package/`, `drivers/`,
`kits/muyu` and `patches/`. In short:

```bash
# kernel, payload, vendor_boot (see AGENTS.md for the full sequence)
cd UniXNU && python3.12 unixnu build kernel -D ../UniXNU-muyu \
    --kernelcache out/ipsw/21H461/kernelcache.decompressed
python3.12 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
    --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
    --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr
python3.12 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
    --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_muyu.img
```

Requirements: unlocked bootloader, a wall charger for the experiments, and a full
stock ROM for recovery. The iPadOS root image is **not** part of this repository.

## Hardware data

- SM8635 "cliffs", 12 GB RAM (device dependent), UFS, 4 KiB pages.
- Display 1840×2880; Novatek `NVT-ts-spi` on GENI SPI `0xa90000`.
- GPU: Adreno 735, `qcom,adreno-gpu-gen7-11-0`, GMU `gen71100` firmware.
- `userdata` is the iPadOS root; the Android user data is gone.

## Safety

This is an experimental port on a device with an unlocked bootloader. It has no
thermal management and no DVFS; long GPU experiments raise the temperature. Read
`AGENTS.md` in the upstream workspace before flashing anything: it lists the
operations that have previously bricked or wedged this tablet.

## License

Same license as UniXNU (BSD-2-Clause-Patent), see `LICENSE`.
