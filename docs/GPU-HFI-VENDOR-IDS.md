# HFI message ids and payloads — vendor confirmation

Source: `MiCode/Xiaomi_Kernel_OpenSource`, branch `muyu-v-oss`,
`drivers/gpu/drm/msm/adreno/a6xx_hfi.h` / `a6xx_hfi.c`.

## Message ids (AUTHORITATIVE)

| id | vendor name | this port |
| --- | --- | --- |
| `0x05` | `HFI_H2F_MSG_TEST` = 5 | — |
| `0x0a` | `HFI_H2F_MSG_START` = 10 | ✅ `AdrenoHfiStageStart` |
| `0x0e` | `HFI_H2F_MSG_CORE_FW_START` = 14 | ✅ `AdrenoHfiStageCoreStart` |
| `0x0f` | `HFI_H2F_MSG_PERF_TABLE` | ✅ v2 performance table |
| **`0x1e`** | **`HFI_H2F_MSG_GX_BW_PERF_VOTE` = 30** | ✅ `AdrenoHfiStageGmuFreq` |
| `0x21` | `HFI_H2F_MSG_PREPARE_SLUMBER` = 33 | ❌ **present in the trace, never sent** |

> **`0x1e` is a "GX bandwidth + performance vote", not a "GMU DCVS set".**
> Earlier documents called it a frequency command (`HANDOFF-2026-10-31.md` §25.4) or a
> DCVS set. The vendor name makes the effect obvious: the message is a **vote that makes
> the firmware bring GX up at that performance point** — which is exactly what
> `power`/`st` changed when we started sending it.

## `0x1e` payload (AUTHORITATIVE)

```c
struct a6xx_hfi_gx_bw_perf_vote_cmd {
	u32 header;
	u32 ack_type;
	u32 freq;      /* performance index, not Hz */
	u32 bw;
};

static int a6xx_hfi_set_freq(struct a6xx_gmu *gmu, int index)
{
	struct a6xx_hfi_gx_bw_perf_vote_cmd msg = { 0 };
	msg.ack_type = 1;   /* blocking */
	msg.freq = index;
	msg.bw = 0;         /* TODO: bus scaling */
	return a6xx_hfi_send_msg(gmu, HFI_H2F_MSG_GX_BW_PERF_VOTE, &msg, ...);
}
```

### Cross-check against this port's own disassembly

Extracted earlier from `msm_kgsl.ko` (`gen7_gmu_dcvs_set`), the wire payload is
`{header, 1, gmu_count-1-level, 0xffff00ff}`. Mapping onto the vendor struct:

| wire word | vendor field | ours | verdict |
| --- | --- | --- | --- |
| `w[0]` | `header` | rebuilt by the HFI layer | ✅ |
| `w[1]` | `ack_type` | **`1`** = blocking | ✅ **exact match** |
| `w[2]` | `freq` (perf **index**) | `gmu_count-1-level` | ✅ index, not Hz |
| `w[3]` | `bw` | `0xffff00ff` | ⚠️ vendor upstream sends `0`; downstream appears to use a sentinel |

Two things fall out of this:

1. `w[1] = 1` is not a magic constant — it is `ack_type = 1` (blocking, i.e. the firmware
   **must** answer). That is why the message is ACKed 254/254 in the trace and why our
   `AdrenoHfiSend` (which waits) is the right call.
2. `freq` is a **performance index**, so the `gmu_count - 1 - level` inversion is correct
   in principle: index 0 is the highest performance point.

## `0x21` PREPARE_SLUMBER payload

```c
struct a6xx_hfi_prep_slumber_cmd { u32 header; u32 bw; u32 freq; };
a6xx_hfi_send_prep_slumber(): all zero payload.
```

It appears in the device trace immediately after the `0x1e` burst, and this port never
sends it.

## Next experiment this suggests

Our current vote uses `gmu_dcvs_level = 1` → perf index **1** (the 220 MHz point) on the
`{off, 220, 550}` table. If a vote is what raises GX, voting the **highest** point
(`level = 2` → perf index **0**, 550 MHz) is the natural next test — one constant change,
no new register pokes.

## Reproduce

```bash
cd /tmp/muyu-ksrc   # see GPU-VENDOR-GX-CLOCK.md for the clone command
grep -n "HFI_H2F_MSG" drivers/gpu/drm/msm/adreno/a6xx_hfi.h
sed -n '565,585p' drivers/gpu/drm/msm/adreno/a6xx_hfi.c
```
