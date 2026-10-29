/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* muyu's GMU stage-1 SMMU bring-up: the board half ShengGpuSmmu.c does not have.
 *
 * ShengGpuSmmuStart* owns one context bank's lifecycle (fault the stream, program the
 * disabled context, attach the stream, observe) but it deliberately does not allocate
 * tables, map memory or do a global SMMU reset. This unit supplies exactly that much:
 *
 *   1. validate the five GMU extents against ShengGmu.c's own sizes and fixed IOVAs;
 *   2. prepare a native4K table pool as an ArmSmmu4KLiveArena (all intermediate tables,
 *      no leaves) and publish it;
 *   3. ShengGpuSmmuStartLive(..., ShengGpuSmmuGmu, bank>=2, slot of stream 5, ...);
 *   4. map the five extents with ArmSmmu4KLiveUpdate, growing the already-mapped set;
 *   5. observe context+streams+the mapped extents again.
 *
 * The hardware facts this depends on (fixed IOVAs, sizes, "the GPU owns banks 0 and 1,
 * so the GMU needs bank >= 2") are the vendor driver's, not guesses:
 *   fw/android-harvest/msm-drv/a6xx_gmu.c  a6xx_gmu_memory_probe(): icache SZ_16M-SZ_16K
 *       @0x04000, dummy SZ_16K @0x60000000, debug SZ_4K*7 @0x60400000, log/hfi SZ_16K
 *       anywhere in [0x60001000, 0x80000000);
 *   fw/android-harvest/vendor-source/arm-smmu-qcom.c  GPU takes context banks 0 and 1.
 *
 * Nothing here touches power, clocks, the GPU SMMU's global registers or the GMU itself:
 * CX power, the GPUCC clock tree and the SMMU vote clock are the caller's prerequisites
 * (MuyuGpuPowerBoard.h modes 5/6), and the global SMMU enable is the probe's
 * mgb_smmu_global_ready() step. Masters must be quiesced: on this path the GMU is still
 * held in reset. Resources are retained on every outcome, including success. */
#ifndef MUYU_GMU_SMMU_H
#define MUYU_GMU_SMMU_H
#include <stddef.h>
#include <stdint.h>
#include "ArmSmmu4K.h"
#include "ArmSmmu4KLive.h"
#include "ShengGpuSmmu.h"
#include "ShengGpuSmmuSource.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MUYU_GMU_ICACHE = 0, MUYU_GMU_DUMMY, MUYU_GMU_DEBUG, MUYU_GMU_LOG, MUYU_GMU_HFI,
    MUYU_GMU_EXTENTS
};

/* Fixed IOVAs. The first three are the ones ShengGmu.c validates; log/hfi are the
 * vendor's dynamic range [0x60001000,0x80000000), placed right after dummy. */
#define MUYU_GMU_ICACHE_IOVA UINT64_C(0x00004000)
#define MUYU_GMU_DUMMY_IOVA  UINT64_C(0x60000000)
#define MUYU_GMU_LOG_IOVA    UINT64_C(0x60004000)
#define MUYU_GMU_HFI_IOVA    UINT64_C(0x60008000)
#define MUYU_GMU_DEBUG_IOVA  UINT64_C(0x60400000)

/* Sizes ShengGmu.c requires byte for byte (0xffc000/0x4000/0x7000/0x4000/0x4000). */
#define MUYU_GMU_ICACHE_BYTES 0x00ffc000u
#define MUYU_GMU_DUMMY_BYTES  0x00004000u
#define MUYU_GMU_DEBUG_BYTES  0x00007000u
#define MUYU_GMU_LOG_BYTES    0x00004000u
#define MUYU_GMU_HFI_BYTES    0x00004000u

/* 32-bit input addresses (the GMU's view) and 40-bit output addresses (this board's
 * RAM), matching the vendor's gmu_vm size of 0x80000000. */
#define MUYU_GMU_INPUT_BITS  32u
#define MUYU_GMU_OUTPUT_BITS 40u

typedef struct {
    void *cpu;              /* writable CPU mapping of the whole pool, page aligned */
    uint64_t physical;      /* contiguous physical base, page aligned */
    size_t capacity_pages;  /* pages owned by the caller */
} MuyuGmuSmmuPool;

typedef struct {
    void *cpu;              /* CPU mapping of the extent, page aligned */
    uint64_t physical;      /* contiguous physical base, page aligned */
    uint64_t iova;
    size_t bytes;
} MuyuGmuSmmuExtent;

typedef struct {
    MuyuGmuSmmuPool pool;
    MuyuGmuSmmuExtent extents[MUYU_GMU_EXTENTS];   /* canonical order, see the enum */
    unsigned bank;          /* >= SHENG_GPU_SMMU_GMU_BANK_MIN */
    unsigned slot;          /* SMR slot leased by the caller for stream 5 */
    unsigned poll_budget;   /* 1..1048576 */
} MuyuGmuSmmuPlan;

/* Cache publication. Must clean the range to the point of coherency (device walks read
 * the tables and the GMU reads the staged firmware) and return 0 only afterwards. A
 * barrier is not a cache clean, so this cannot be folded into ArmSmmu4KIo.barrier. */
typedef struct {
    void *cookie;
    int (*publish)(void *cookie, uint64_t physical, size_t bytes);
} MuyuGmuSmmuCache;

typedef enum {
    MuyuGmuSmmuDone = 0,
    MuyuGmuSmmuArgument,        /* the plan is unusable; nothing was written */
    MuyuGmuSmmuPrepareFailed,
    MuyuGmuSmmuPublishFailed,
    MuyuGmuSmmuStartFailed,
    MuyuGmuSmmuMapFailed,
    MuyuGmuSmmuObserveFailed
} MuyuGmuSmmuOutcome;

typedef struct {
    MuyuGmuSmmuOutcome outcome;
    unsigned phase;             /* 1 prepared, 2 published, 3 started, 4 mapped, 5 observed */
    ArmSmmu4KStatus prepare_status, start_status, map_status, observe_status;
    size_t capacity_pages, table_pages, leaf_pages;
    uint64_t arena_iova, arena_bytes, table_physical;
    uint64_t context_offset;
    uint32_t id0, id1, id2, scr0, global_fault;
    unsigned bank, slot, streams_bank, streams_count;
    unsigned start_phase, start_writes, start_poll_reads;
    unsigned map_published, map_descriptor_writes, mapped;
    unsigned extents;
} MuyuGmuSmmuSummary;

typedef struct {
    MuyuGmuSmmuPlan plan;           /* a copy: callers may free their own */
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state;
    ArmSmmu4KContextConfig context;
    ArmSmmu4KRange ranges[MUYU_GMU_EXTENTS];
    ArmSmmu4KLiveVisibility visibility;
    ShengGpuSmmuSession session;
    ShengGpuSmmuResult started, observed;
    ArmSmmu4KLiveResult mapping;
    uint32_t alias_scratch[MUYU_GMU_EXTENTS];
    int (*publish)(void *, uint64_t, size_t);
    void *publish_cookie;
    MuyuGmuSmmuSummary summary;
} MuyuGmuSmmu;

/* Run the whole lifecycle. On failure every resource stays owned and the context bank
 * is not disabled (ShengGpuSmmu's contract); the caller must treat the SMMU as live. */
MuyuGmuSmmuOutcome MuyuGmuSmmuRun(const ArmSmmu4KIo *io, const MuyuGmuSmmuPlan *plan,
    const MuyuGmuSmmuCache *cache, MuyuGmuSmmu *state);

/* The pool the plan needs for these five extents: one root, the intermediate levels for
 * the whole [icache,debug] aperture and one level-3 table per 2 MiB the extents touch. */
size_t MuyuGmuSmmuTablePages(const MuyuGmuSmmuPlan *plan);

/* Range descriptors exactly as they are programmed, for host tests and the report. */
size_t MuyuGmuSmmuRanges(const MuyuGmuSmmuPlan *plan, ArmSmmu4KRange *out, size_t capacity);

const char *MuyuGmuSmmuOutcomeText(MuyuGmuSmmuOutcome outcome);

#ifdef __cplusplus
}
#endif
#endif
