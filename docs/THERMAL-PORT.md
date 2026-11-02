# Thermal sensor port (muyu / SM8635 "cliffs")

**State:** read-only survey **runs on the device** (2026-11-02, first try). Sampling and the
throttle are the open work.

| item | value |
| --- | --- |
| boot argument | `muyu-thermal=<seconds>` (default off): 0 = one-shot, N > 0 = sample N s then panic |
| kernel | `UniXNU/out/muyu/kernel/kernel.macho` = `4c3a54222ef6a5f0e9ba688af59947f87629d60676fc54eb2923f3f244b4d3b7` |
| image (one-shot) | `fw/vendor_boot_thermal1.img` = `a8ee8a5feb8b76b86cab99dd51b0c3fbc811d41e1e8b3ce6dfdecba1f8dea195` (61.81 MiB) |
| host checks | `scripts/check-muyu-thermal-host.py` PASS (DT map, decode, row, trend, freestanding) |
| first device run | `vendor_boot_thermal1.img`, photo `logs/2026-11-02-18-24-56-235.jpg` |

### First result (2026-11-02 18:24, `muyu-thermal=0`-equivalent one-shot)

```
MUYU THERM L=29 B=27 G=25 D=26 hot=29 en=111 inv=0
@MuyuThermalProbe.c:127
```

What it settles:

| question | answer |
| --- | --- |
| are the registers readable at the DT bases? | **yes** |
| does the firmware leave TSENS enabled? | **yes**, `en=111` for all three controllers |
| does every sensor we report have VALID? | **yes**, `inv=0` |
| is `Sn_STATUS` deciCelsius here? | **yes** - 29/27/25/26 °C is a cold idle boot; an ADC code would decode to a nonsense negative with the default slope |
| is the sensor map right? | consistent: the little cluster (boot CPU) is the warmest, GPU the coolest |

Caveat: this is the **boot-time** snapshot, taken before userland runs, so it says "the sensors
work", not "the tablet stays cool". That is what the sampling mode is for.

No throttling, no DVFS, no battery/charging driver. This file records what the hardware
offers, how the survey reads it, and what a policy has to be built on top of it.

Read `AGENTS.md` §5.6 first: this port has **no thermal management**, no DVFS and `wdt=-1`, so the
tablet has no way to protect itself from sustained load. That is the reason this track exists.

---

## 1. What the SoC exposes

Three `qcom,tsens-v2` controllers (device DT, `fw/muyu_dt.txt`, `soc/tsensN@…`):

| controller | TM window | SROT window | `#qcom,sensors` | DT zones it feeds |
| --- | --- | --- | --- | --- |
| `tsens0` | `0x0c228000` | `0x0c222000` | 14 | `cpuss-0/1` (1, 2), `cpu-1-*-*` (3–10), `cpu-2-*-*` (11–13) |
| `tsens1` | `0x0c229000` | `0x0c223000` | 16 | `cpu-0-*-*` (1–3), `nsphvx/nsphmx` (4–7), **`gpuss-0..3` (8–11)**, `video` (12), **`ddr` (13)**, `camera-0/1` (14, 15) |
| `tsens2` | `0x0c22a000` | `0x0c224000` | 5 | `aoss-2` (0), `mdmss-0..3` (1–4) — modem, not ours |

So in this port's terms:

- **little cluster** = `tsens1[1..3]`
- **mid + big clusters** = `tsens0[1..13]`
- **GPU** = `tsens1[8..11]`
- **DDR** = `tsens1[13]`

Those are the four numbers a throttle needs, and they are what the panel row reports.

The DT's own trips (they are the *emergency* ladder, not a throttle policy — Android throttles in
userspace `thermal-engine`, which does not exist here):

| trip | value | where |
| --- | --- | --- |
| `tj_cfg` (GPU) | **95.0 °C** | `thermal-zones/gpuss-0` |
| `cpu7-emerg2-cfg` | **110.0 °C** | `thermal-zones/cpu-2-0-2` |
| `reset-mon-cfg` | **115.0 °C** | every CPU/GPU/DDR zone |
| `thermal-engine-config` / `thermal-hal-config` | **125.0 °C** | every zone (the DT's "max" placeholder) |

115 °C is a reset monitor: crossing it is not a throttle, it is the SoC deciding to stop.
A usable policy has to act well below that (75–80 °C is the usual first step on this class of part).

---

## 2. Register model

From mainline `drivers/thermal/qcom/tsens-v2.c` (`tsens_v2_regfields`). These three controllers
use the plain `"qcom,tsens-v2"` compatible, so the driver's `feat->adc` is 0: **each `Sn_STATUS`
word already carries deciCelsius**, not an ADC code, and there is no per-sensor calibration in the
reading path. This is the one assumption the decode hangs on; see `MuyuThermal.h` for the note on
what changes if a future DT names an ADC variant.

| offset | register | use |
| --- | --- | --- |
| SROT `0x00` | `SROT_HW_VER` | `31:28` major, `27:16` minor, `15:0` step |
| SROT `0x04` | `SROT_CTRL` | bit0 `TSENS_EN`, bit1 `TSENS_SW_RST` |
| TM `0xe4` | `TM_TRDY` | mainline reads bit0 as "fresh result"; **on this board it reads `0x8`** (see below) |
| TM `0xa0 + 4·id` | `Sn_STATUS` | `11:0` deciCelsius (two's complement), bit21 `VALID` |

`temp_mC = sign_extend12(Sn_STATUS[11:0]) × 100`.

**TRDY is not a gate here.** The first sampling run (2026-11-02) logged `trdy=00000008` for all
three controllers - bit0 clear - while every one of the 35 reported sensors had VALID set and
plausible temperatures (`inv=0`). So this IP's readiness flag is not where mainline's v2 layout
puts it. The survey records TRDY for the log and gates nothing on it; `VALID` is the only
readiness signal it trusts.

**Read-only by construction:** the driver only writes `TSENS_EN` on v1/VER_0 IP; on v2 the mainline
`init_common()` refuses to run unless the firmware already enabled TSENS and left it ready. The
bootloader does. So the survey never writes a register — and if a block reports `en=0` or `trdy=0`
it says so instead of "fixing" it.

---

## 3. The survey (`muyu-thermal=<seconds>`)

`package/MuyuThermalProbe.c`, called from `native_legacy_display_late_probe()` after the GPU probe.
Default off.

| value | effect |
| --- | --- |
| absent | off |
| `muyu-thermal=0` | one-shot read-only survey at boot, publishes the summary row, no panic (the boot continues to iOS) |
| `muyu-thermal=N` (N > 0) | survey now, then read once a second in a kernel thread for N seconds and `panic()` with the summary and the trend |

One argument is the whole interface on purpose: the kernel command line has a 1000-byte budget and
this board's is already at 956 bytes without it (adding three arguments overflowed it by 14 bytes).

A sampling run always ends in the panic - that is what freezes the rows for a photograph. The
thread starts before userland, so its samples include the boot and compositor load; a one-shot
value only ever shows a cold device.

Output, on the boot-progress page (row `THERM`) and in the kernel log:

```
THERM L=30 B=61 G=55 D=49 hot=70 en=111 inv=0
```

| token | meaning |
| --- | --- |
| `L` | little cluster (`tsens1[1..3]`), °C |
| `B` | mid+big clusters (`tsens0[1..13]`), °C |
| `G` | GPU (`tsens1[8..11]`), °C |
| `D` | DDR (`tsens1[13]`), °C |
| `hot` | hottest valid sensor of all three controllers, °C |
| `en` | per-controller `TSENS_EN` as three digits, `111` = all enabled |
| `inv` | sensors whose `VALID` bit was clear (should be 0; a nonzero count means a sensor id is not populated on this SKU) |

and a second row for a sampling run:

```
THRM2 h0=29 h15=33 h30=35 h60=37 h120=38 pk=38
```

| token | meaning |
| --- | --- |
| `hN=` | hottest sensor of all controllers at N seconds into the sampling run |
| `pk=` | hottest sample over the whole run |

`?` in a field means "no valid sensor in that range", not 0 °C.

Log lines are shaped for the persistent log as well:

```
MUYU THERM t0 ver=2.4.4 ctrl=... trdy=... n=14
MUYU THERM t0 deg: 45 46 ...
MUYU THERM summary L=... valid=44
```

---

## 4. What is verified on the host, and what is not

`scripts/check-muyu-thermal-host.py` (no device):

1. re-derives the three controller bases, their sensor counts and the `cpu-*`/`gpuss-*`/`ddr`
   zone→(controller, id) map from `fw/muyu_dt.txt`, and compares them with the constants in
   `MuyuThermal.h` — so the header cannot silently drift from the device tree;
2. compiles and runs `package/MuyuThermalHostTests.c` (sign extension, rounding, region roll-up,
   exact row text);
3. checks the header still compiles freestanding for `aarch64-none-elf`.

**Not verified:** that the registers read at all on this board, that the bootloader leaves TSENS
enabled, and every absolute temperature. Those need one boot; §5 is the runbook.

---

## 5. Runbook (not yet executed)

```bash
W=~/pad7pro-xnu
export PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH
cd $W/UniXNU
python3.12 unixnu build kernel -D ../UniXNU-muyu \
    --kernelcache out/ipsw/21H461/kernelcache.decompressed
python3.12 unixnu build payload -D ../UniXNU-muyu \
    --kernel out/muyu/kernel/kernel.macho --unique-chip-id 0000000000000001 \
    --owner-trust-cache /tmp/muyu-static-system.bundle \
    --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr \
    --kernel-arg muyu-thermal=120
python3.12 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
    --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_thermal1.img
cd $W && ./tools/muyu-flash-mode.sh preflight
./tools/muyu-flash-mode.sh xnu fw/vendor_boot_thermal1.img
# photograph the panic screen, then return:
./tools/muyu-flash-mode.sh xnu fw/vendor_boot_btn2.img
```

### Judgment tree

| reading | meaning | next |
| --- | --- | --- |
| `en=111`, `inv=0`, plausible °C (20–45 at boot) | registers are readable, decode is right | implement the throttle |
| `en` not `111`, or `trdy=0` | the firmware did not leave that controller running; a CPU-side enable would be needed (and is a write we have not justified) | investigate that block before writing anything |
| `L=? B=? …` all `?` | no `VALID` bit: either the mapping is wrong or v2 here is not plain v2 | check `ctrl`/`trdy`/`ver` in the same log line before touching the decode |
| plausible values but `hot` far above ambient at boot | the boot itself is already heating the part; measure idle after login instead | sample later (see §6) |

---

## 6. Next steps (in order)

1. **Run §5 once.** Everything below depends on the registers being readable at all.
2. **Sampling.** The survey is one-shot at boot. A policy needs a periodic read (1 s) on the
   existing boot-progress monitor thread, plus the peak over the last minute, otherwise a
   photograph only ever shows the instant it was taken.
3. **Throttle.** Two candidate actuators, in this order:
   - **CPU frequency cap** — `package/CpuFreqProbe.cpp` already knows the EPSS layout
     (`reg_perf_state = 0x320`, domains at `0x17d91000`/`…2000`/`…3000` from the DT's
     `qcom,cpufreq-hw` node) but nothing has validated the write on this board yet. Cap by
     writing a lower LUT index; verify with `domain_state` readback and the existing `CPUFREQ` row.
   - **Idle injection** — if the EPSS write does not stick, the fallback is to park a core or
     insert a duty-cycle sleep in the compositor path, which is the only sustained CPU load here.
4. **Emergency action** at the DT's real emergency trips (95 °C GPU / 110 °C CPU): stop accepting
   new work and, past ~115 °C, power off rather than wait for the reset monitor.
5. **Watchdog.** `wdt=-1` is still set. A thermal policy that hangs takes the whole safety story
   with it, so the two belong in the same chapter.

---

## 7. Provenance

| fact | source |
| --- | --- |
| controller bases, sensor counts, zone → sensor ids, trip points | `fw/muyu_dt.txt` (vendor DT dump), re-derived by `scripts/check-muyu-thermal-host.py` |
| register offsets, `VALID`/`LAST_TEMP` bit positions, v2 "already deciCelsius" | mainline `drivers/thermal/qcom/tsens-v2.c` + `tsens.c` (v6.12) |
| no CPU-side enable for v2 | mainline `tsens.c` `init_common()`: `-ENODEV` on "device not enabled" for anything above VER_0 |
| Android loads `qcom_tsens.ko` and the driver runs | `logs/android-harvest-20261101-135328/dmesg.txt` |
| TSENS interrupts are live (30/32/34, low counts) | `logs/android-harvest-20261101-135328/interrupts.txt` |
| why this matters at all | `AGENTS.md` §5.6, §6.4 |
