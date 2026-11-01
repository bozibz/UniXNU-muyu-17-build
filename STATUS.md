# Status and how to continue

**Last updated:** 2026-11-01 · **Device:** Xiaomi Pad 7 Pro (`muyu`, SM8635, Adreno 735)
**OS:** iPadOS 17.7.11 (21H461) on `userdata` · **Format:** 1 (`board.toml`)

This file is the hand-off. If you are picking this port up, read this first, then the
documents under `docs/`.

## What works

| Area | State |
| --- | --- |
| UEFI (`fastboot boot`), UniXNU menu, payload from `vendor_boot` | works |
| XNU → BSD init → iPadOS desktop | works |
| Display | works, CPU-composed (~1.4 fps) |
| Physical buttons | work |
| Novatek touch (`muyu-touch-drv=1`) | works (native IOService) |
| USB debug console | experimental: SS PHY facts pinned, opt-in bring-up written, not yet enumerated |
| USB3 SS PHY (`ssphy@88e8000`) | DT facts generated into `package/MuyuUsbPhy.h`; read-only survey in the driver; opt-in power-on follows mainline qmp-usb order. Images: `fw/vendor_boot_usb7.img` (survey + 90 s panic), `fw/vendor_boot_usb8.img` (attempt to bring the PHY up) |
| GPU: CX power, GPUCC clock tree, GPU SMMU | works (hardware-verified) |
| GPU: GMU cold start + full HFI startup | **works** (2026-10-30, `cold=0 ph=7 proto=0`) |
| GPU: GX rail, CP, command submission, Metal | **not implemented** |

## GPU: where it stands

The GMU runs and answers HFI. The seven startup commands are acknowledged by the firmware;
this repeated on every probe run (`cold=0 ph=7 proto=0 fw=0x00000100 hfi=0x00000001`).

Evidence, in the order it was obtained:

1. CX power + GPUCC clocks — `docs/GPU-CLIFFS-MMIO.md` (the register map; this SoC is *not*
   SM8650-compatible, and the devicetree GDSC node names are leftovers).
2. GPU SMMU identity (24 SMR / 22 CB) and the GMU's stage-1 context (bank 2, SID 5).
3. Five GMU memory blocks + the A735 firmware staged with its SHA verified.
4. GMU cold start + HFI — probe modes 7–11, `docs/GPU-PROBE-RUNBOOK.md` §11.

Two facts that took hardware iterations and should not be rediscovered:

- **GMU firmware chip id is `0x070b0000`.** The catalog-less formula gives `0x4303b000`,
  which the firmware rejects (`FW_INIT_RESULT=0x901`). The CM3 image contains its own check
  (`0x10004104` + literal); the generator derives the value from that.
- The board OPP/BCM policy comes from the device's own downstream devicetree
  (`qcom,gpu-pwrlevels` / `qcom,gmu-freq-table`), 11 GPU levels 1100→255 MHz.

## The current blocker (GX rail)

`ShengGmuColdStart()` succeeds, but the GPU's GX power domain does not come up:

- `gpu_cc_gx_gdsc` is a `support-cfg-gdscr` GDSC: enable = clear `SW_COLLAPSE`, and the status
  is `CFG_GDSCR (0x9038) bit16`. That bit never sets (`cfg=0x050e0000`).
- Its `parent-supply` is the RPMh ARC voter `pm_v6c_s6_gfx_voter_level` (`mxc.lvl`).
  Linux enables it through the regulator framework; the official component catalog has no
  RPMh/RSC component, so a CPU-side sender was written here:
  `package/MuyuRpmhRsc.{h,c}` + `MuyuRpmhRscHostTests.c`, probe mode 17, host-tested by
  `scripts/check-muyu-rpmh-host.py`. It sends one ACTIVE TCS transfer with both ARC votes
  (`gfx.lvl` selector from init level 0x10, `mxc.lvl` from 0x30). **Flashing done; the
  on-device panic readout has not been collected yet.**
- The GX clock-controller rail (`gx_clkctl_gx_gdsc`, `0x3d68024`) reads back 0 while the
  parent is off; its devicetree node is `status = "disabled"`, so it is not the missing
  piece.
- The OOB `GPU_SET` handshake (non-legacy bits 30/31 and legacy 16/24) gets no ACK, which
  is consistent with GX being unavailable.

See `HANDOFF-2026-10-29.md` §20 (workspace) and `docs/GPU-PROBE-RUNBOOK.md` §12 for all
readings.

## Next steps, in order

1. **GX power.** The minimal RSC voter is implemented (mode 17). Flash
   `fw/vendor_boot_gpupower17.img` and read the `gmu rpmh` panic line: `vote=Success` plus
   `gx` ACK means GX is up; `Busy`/`Timeout` means the TCS parameters need work.
2. **SQE/ZAP + A735 hardware init.** The SQE/ZAP firmware is in `vendor`/`odm`; the pwrup
   register list can be extracted from the device's `msm_kgsl.ko` (BTF + rodata).
3. **CP, ring, fences.** Bounded submission is the first real "GPU works" proof.
4. **Metal.** `AdrenoMetal` + turnip, with an A735 backend on top of `Adreno740`.

## Build and flash (short form)

```bash
cd UniXNU && python3.12 unixnu build kernel -D ../UniXNU-muyu \
    --kernelcache out/ipsw/21H461/kernelcache.decompressed
python3.12 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
    --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
    --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr
python3.12 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
    --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_muyu.img
```

The full procedure (boot arguments, data seed, flash script, ledgers) is in the workspace's
`AGENTS.md` and in `docs/GPU-PROBE-RUNBOOK.md`.

## Hard limits to know

- No thermal management and no DVFS on this port; long GPU experiments heat the tablet.
- `ShengGmu.c`'s poll caps a single wait at 10 ms; the A735 firmware finishes well inside it.
- ACD and IFPC are off (they need AOSS/ACD and power-collapse prerequisites).
- GPLL0's GPU vote (GCC `0x52000` bit 15/16) does not stick; the GMU uses PLL1, so it only
  matters for hub/axi votes.

## If you want to take over

Open an issue on this repository. Everything needed to reproduce the state is here: the
register tables, the probe modes, the generator scripts and their checks, and the reasoning
behind each decision.
