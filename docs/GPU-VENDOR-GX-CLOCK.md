# Vendor truth for the GX clock and SPTPRAC (from the muyu GKI tree)

Source: `MiCode/Xiaomi_Kernel_OpenSource`, branch **`muyu-v-oss`** (SM8635/Adreno735),
files `drivers/gpu/drm/msm/adreno/a6xx_gmu.c` and `a6xx_gmu.xml.h`.
This is upstream `drm/msm`, not the downstream `msm_kgsl` that ships on the device,
but the GMU/SPTPRAC programming is the same hardware contract.

## 1. `SPTPRAC_PWR_CLK_STATUS` bit layout (AUTHORITATIVE)

```c
#define REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS              0x000050d0
#define ..._SPTPRAC_GDSC_POWERING_OFF   0x00000001   /* bit0 */
#define ..._SPTPRAC_GDSC_POWERING_ON    0x00000002   /* bit1 */
#define ..._SPTPRAC_GDSC_POWER_OFF      0x00000004   /* bit2 */
#define ..._SPTPRAC_GDSC_POWER_ON       0x00000008   /* bit3 */
#define ..._SP_CLOCK_OFF                0x00000010   /* bit4 */
#define ..._GMU_UP_POWER_STATE          0x00000020   /* bit5 */
#define ..._GX_HM_GDSC_POWER_OFF        0x00000040   /* bit6 */
#define ..._GX_HM_CLK_OFF               0x00000080   /* bit7 */
```

> **This corrects `HANDOFF-2026-10-31.md:326`**, which states
> `bit0 = GX_HM_GDSC_POWER_OFF, bit1 = GX_HM_CLK_OFF`. They are **bit6 and bit7**.
> Every earlier reading that used bit0/bit1 for GX power must be re-decoded.

Re-decoding our own measurements with the correct layout:

| run | `st` | bits set | meaning |
| --- | --- | --- | --- |
| m26 (no `0x1e`) | `0x2` | bit1 `SPTPRAC_GDSC_POWERING_ON` | GDSC **stuck powering on** |
| m27/m28 (`0x1e`) | `0x0` | none | idle/unreported |

Note `GX_HM_GDSC_POWER_OFF` (bit6) and `GX_HM_CLK_OFF` (bit7) were **never set** in any
run, so `st` never actually said "GX clock off" — the earlier claim that
"`st` went 2→0 so the GX clock turned on" was reading the wrong bits entirely.

## 2. `a6xx_sptprac_enable` — legacy-only

```c
static int a6xx_sptprac_enable(struct a6xx_gmu *gmu)
{
        if (!gmu->legacy)
                return 0;                       /* ★ not for non-legacy GMUs */

        gmu_write(gmu, REG_A6XX_GMU_GX_SPTPRAC_POWER_CONTROL, 0x778000);
        ret = gmu_poll_timeout(gmu, REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS, val,
                (val & 0x38) == 0x28, 1, 100);
        ...
}
```

Caller (inside `a6xx_gmu_start`-time setup):

```c
if (gmu->idle_level < GMU_IDLE_STATE_SPTP) {
        ret = a6xx_sptprac_enable(gmu);
        ...
}
```

Consequences for this port:

- the vendor's own code says **the CPU must not touch SPTPRAC unless the GMU is
  legacy *and* idle_level < IDLE_STATE_SPTP**;
- our GMU runs a full HFI cold start (`proto=0 hfi=1`), i.e. **not** in that mode,
  so writing `GX_SPTPRAC_POWER_CONTROL = 0x778000` from the CPU is **not** vendor-backed
  and is expected to be ignored — which matches `HANDOFF-2026-10-31.md` §10.2.
- the "SPTPRAC on" criterion is `(val & 0x38) == 0x28` (bit3 + bit5 set, bit4 clear).
  Our `st` has never matched it.

## 3. `RBBM_SW_RESET_CMD` is for power-down, not init

`a6xx_gmu.c` asserts it in the force-power-down path:

```c
/* Reset GPU core blocks */
gpu_write(gpu, REG_A6XX_RBBM_SW_RESET_CMD, 1);
udelay(100);
```

So a "write 1 then 0 and poll" init recipe is **not** what the vendor does here either,
and `gen7_start` in `msm_kgsl` never touches the register at all.

## 4. What this means for the port

The GX clock is owned by the GMU firmware on this configuration. The vendor path is
**HFI**, not CPU register pokes: `a6xx_gmu_start` → `a6xx_gmu_hfi_start` → later
`PREP_SLUMBER`/`GPU_SET` traffic. That is consistent with what the trace showed
(`0xa` then `0x1e`) and with `0x1e` being accepted (`proto=0`).

So the next question is not "which register turns the clock on" but **"which HFI
sequence makes the firmware raise the GX clock"** — i.e. the `PREP_SLUMBER`/`GPU_SET`
pair that `a6xx_gmu_notify_slumber`/`a6xx_gpu_boot` issue.

## Reproduce

```bash
git clone --filter=blob:none --no-checkout --depth 1 \
  --branch muyu-v-oss https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git /tmp/muyu-ksrc
cd /tmp/muyu-ksrc
git sparse-checkout set drivers/gpu/drm/msm/adreno && git checkout
grep -n "SPTPRAC_PWR_CLK_STATUS" drivers/gpu/drm/msm/adreno/a6xx_gmu.xml.h
sed -n '355,380p' drivers/gpu/drm/msm/adreno/a6xx_gmu.c
```
