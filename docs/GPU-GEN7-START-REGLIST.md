# gen7 GPU hard-init register list (extracted from `msm_kgsl.ko`)

Source: `fw/android-harvest/msm_kgsl.ko` (`sha256 252a6f8d…`, identical to the
device's `/vendor/lib/modules/msm_kgsl.ko`). Function **`gen7_start`** at offset
`0x73ba8`, size `0xa68`. This is the A735/A740 hard-init sequence the vendor runs
after `gen7_gpu_boot` has done OOB + HFI + `kgsl_mmu_start`.

## How it was extracted

`gen7_start` is a long run of `kgsl_regmap_write(regmap, value, offset)` calls with
immediate operands. Disassembling it and tracking `w1`/`w2` across `mov`/`movk`
recovers the pairs. **The argument order is `w1 = value`, `w2 = offset`** —
confirmed by matching the offsets against `drivers/Adreno740/NativeAdrenoA740HwInit.h`:

- 21 of 42 extracted offsets are named `A740_*` registers

- e.g. `0x8e08`=`RB_NC_MODE_CNTL`, `0xb604`=`TPL1_NC_MODE_CNTL`,
  `0xae02`=`SP_NC_MODE_CNTL`, `0x8602`=`GRAS_NC_MODE_CNTL`, `0xe3a`=`UCHE_GBIF_GX_CONFIG`

## The list

| # | offset | value | register |
| --- | --- | --- | --- |
| 1 | `0x00e3a` | `0x010240e0` | A740_UCHE_GBIF_GX_CONFIG |
| 2 | `0x0050b` | `0xffffffff` | A740_RBBM_PERFCTR_GPU_BUSY_MASKED |
| 3 | `0x00e09` | `0xfffff000` | A740_UCHE_TRAP_BASE |
| 4 | `0x00e0a` | `0x0001ffff` |  |
| 5 | `0x00e07` | `0xfffff000` | A740_UCHE_WRITE_THRU_BASE |
| 6 | `0x00e08` | `0x0001ffff` |  |
| 7 | `0x00e0b` | `0x0001ffff` | A740_UCHE_GMEM_RANGE_MIN |
| 8 | `0x00e17` | `0x00800000` | A740_UCHE_CACHE_WAYS |
| 9 | `0x00e3c` | `0x0006690e` | A740_UCHE_CMDQ_CONFIG |
| 10 | `0x00500` | `0x00000001` | A740_RBBM_PERFCTR_CNTL |
| 11 | `0x1f842` | `0x00000004` |  |
| 12 | `0x08e08` | `0x0001f842` | A740_RB_NC_MODE_CNTL |
| 13 | `0x0b604` | `0x0001f842` | A740_TPL1_NC_MODE_CNTL |
| 14 | `0x0b609` | `0x3fe05ff4` |  |
| 15 | `0x0b60a` | `0x3fa0ebee` |  |
| 16 | `0x0b60b` | `0x3f5193ed` |  |
| 17 | `0x0b60c` | `0x3f0243f0` |  |
| 18 | `0x0ae02` | `0x3f0243f0` | A740_SP_NC_MODE_CNTL |
| 19 | `0x08602` | `0x3f0243f0` | A740_GRAS_NC_MODE_CNTL |
| 20 | `0x00e01` | `0x3f0243f0` | A740_UCHE_MODE_CNTL |
| 21 | `0x0001f` | `0x40000000` | A740_RBBM_INTERFACE_HANG_INT_CNTL |
| 22 | `0x00e19` | `0x00000080` | A740_UCHE_CLIENT_PF |
| 23 | `0x08e50` | `0x00000001` |  |
| 24 | `0x0b602` | `0x000c0700` |  |
| 25 | `0x23b0e` | `0xff000000` |  |
| 26 | `0x000ff` | `0x00000001` |  |
| 27 | `0x0084f` | `0x0000000b` | A740_CP_PROTECT_CNTL |
| 28 | `0x00b09` | `0x0000000b` |  |
| 29 | `0x00b09` | `0x0000000b` |  |
| 30 | `0x03c02` | `0x0000000b` |  |
| 31 | `0x03c01` | `0x00000100` |  |
| 32 | `0x00844` | `0x0000006f` | A740_CP_APRIV_CNTL |
| 33 | `0x00ad0` | `0x0000000f` | A740_CP_BV_APRIV_CNTL |
| 34 | `0x00b31` | `0x0000000f` | A740_CP_LPAC_APRIV_CNTL |
| 35 | `0x00b78` | `0x00000001` |  |
| 36 | `0x00841` | `0x00000001` |  |
| 37 | `0x00ada` | `0x00000001` |  |
| 38 | `0x00b30` | `0x00000001` |  |
| 39 | `0x0f800` | `0xf0000000` | A740_RBBM_SECVID_TSB_TRUSTED_BASE |
| 40 | `0x0f802` | `0x000000ae` | A740_RBBM_SECVID_TSB_TRUSTED_SIZE |
| 41 | `0x000ad` | `0x00000001` |  |
| 42 | `0x0011f` | `0x00000001` |  |

## Gaps (do not treat the list as complete)

- **9 of 51 `kgsl_regmap_write` calls could not be paired** because the
  value or offset came from a register rather than an immediate. Their positions are
  visible in the disassembly between the listed ones.
- The **first call is `kgsl_regmap_multi_write`** with a table taken from the device
  struct (`[dev->X+0xa8]`, count `[dev->X+0xb0]`), i.e. the DT/driver reglist.
  It is **not** a static table, so it cannot be extracted offline; it must come from
  the board's own device tree or be replicated from the vendor reglist.
- `gen7_start` also calls `llcc_get_slice_id`/`llcc_slice_activate`, `kgsl_regmap_rmw`
  (6x) and `kgsl_regmap_read` (7x, polling), which are not captured here.

## Key negative finding

**`gen7_start` never writes `RBBM_SW_RESET_CMD` (`0x43`).** A search for that offset in
the whole function returns zero hits. So the vendor does **not** take the GPU out of
reset with an RBBM software reset on this path — it programs the block directly.
Any plan built around "write RBBM_SW_RESET_CMD then poll" is therefore not
vendor-backed and was dropped.

## Reproduce

```bash
export PATH=/opt/homebrew/opt/llvm/bin:$PATH
llvm-objdump -dr --start-address=0x73ba8 --stop-address=0x745f0 \
  fw/android-harvest/msm_kgsl.ko
# that range has 75 call relocations in total; 51 of them are kgsl_regmap_write:
llvm-objdump -dr --start-address=0x73ba8 --stop-address=0x745f0 \
  fw/android-harvest/msm_kgsl.ko | grep -c 'CALL26.*kgsl_regmap_write'   # 51
```

## The `multi_write` reglist: NOT resolved (and the obvious guess is wrong)

`gen7_start` starts with `kgsl_regmap_multi_write(regmap, ptr, count)` where
`ptr = [x21 + 0xa8]`, `count = [x21 + 0xb0]`, `x21 = [dev + 0x1920]`.

Things that were checked and did **not** settle it:

- The static initialisers of every `*_gpudev` struct have **no relocation at `+0xa8`**,
  so that field is not a statically-initialised table pointer.
- The device tree node `soc/qcom,kgsl-3d0@3d00000` has **no init reglist property**
  (only `qcom,initial-pwrlevel`, `qcom,bus-table-*`, `qcom,min-access-length`, ...).
  The one `of_property_read_variable_u32_array` call that was decoded reads
  `qcom,min-access-length`, not a reglist.
- There **are** static symbols named like tables:

  ```
  gen7_pwrup_reglist          0x1f6a0  size 0x44  (17 u32 words)
  gen7_0_0_pwrup_reglist      0x1f65c  size 0x44
  gen7_ifpc_pwrup_reglist     0x1f574  size 0xd8
  gen7_0_0_ifpc_pwrup_reglist 0x1f4a0  size 0xd4
  ```

  But **their entry format is not `(offset, value)`**: all 17 words of
  `gen7_pwrup_reglist` look like register offsets, one word (at `0x1f6c0`) carries a
  `PREL32` relocation, and pairing them yields "values" such as `0xe0a`/`0xe08`
  that contradict the real values `gen7_start` writes to the same registers
  (`0xe08 <- 0x0001ffff`, `0xe0a <- 0x0001ffff`). They are most likely
  save/restore or mask tables, not the init sequence.

**Conclusion: the `multi_write` table is still unknown.** Do not synthesise one.

Two honest ways forward:

1. **From the vendor kernel sources** (the MiCode muyu tree) — cheapest and exact.
2. **Read the pointer on Android** while kgsl is loaded: `[dev+0x1920]+0xa8` and
   `+0xb0` are live values there, and a read-only kprobe can print them.

   Note this is **not** something the XNU probe can do: `[dev+0x1920]` is a kgsl
   kernel structure that does not exist in the XNU address space, so the panic
   screen cannot show it. Do not plan a mode for it.
