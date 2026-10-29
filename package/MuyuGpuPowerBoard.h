// SPDX-License-Identifier: BSD-2-Clause-Patent
// muyu (SM8635 / cliffs) Adreno power and clock observation: board facts plus a gated read order.
//
// This is the cliffs counterpart of the sheng GpuPowerBoard.h. The constants are NOT the sheng
// ones: cliffs is not register compatible with SM8550 or SM8650. Every value here is asserted by
// UniXNU-muyu/scripts/check-muyu-gpu-mmio.py against the vendor driver source (MiCode
// muyu-v-oss gpucc-cliffs.c / gcc-cliffs.c) and against the device's own devicetree reg values.
// See UniXNU-muyu/docs/GPU-CLIFFS-MMIO.md.
//
// Why gated: the GPUCC, the GPU SMMU and the GPU register window sit behind clocks and power
// domains Darwin has no driver for. Reading a block whose clock or rail is off can hang the bus
// (the board just resets) instead of failing, so each stage runs only if the previous one showed
// its prerequisite on. Two hard rules learned on the device (2026-10-29):
//   1. never walk a whole clock controller; only read the exact registers below;
//   2. a regcache-backed read is not hardware state, so this probe talks to MMIO directly.
#ifndef MUYU_GPU_POWER_BOARD_H
#define MUYU_GPU_POWER_BOARD_H
#include <stdint.h>

// Windows (device devicetree: soc/clock-controller@100000, @3d90000; soc/qcom,kgsl-smmu@3da0000).
#define MGB_GCC_BASE 0x00100000ULL
#define MGB_GCC_GPU_PAGE 0x00171000ULL   // GCC + 0x71000: the GPU branches, one page
#define MGB_GCC_VOTE_PAGE 0x00152000ULL  // GCC + 0x52000: the GPLL0 GPU votes
#define MGB_GPUCC_BASE 0x03d90000ULL
#define MGB_GPUCC_SIZE 0xa000ULL
#define MGB_SMMU_BASE 0x03da0000ULL
#define MGB_SMMU_GR0_SIZE 0x1000ULL      // global space 0 only
#define MGB_GPU_BASE 0x03d00000ULL
#define MGB_GPU_WINDOW 0x1000ULL

// GCC (gcc-cliffs.c). gcc_gpu_cfg_ahb_clk has no clk_branch: gcc_cliffs_probe keeps it always-on
// with regmap_update_bits(regmap, 0x71004, BIT(0), BIT(0)); its halt check gates on the enable bit.
#define MGB_GCC_GPU_CFG_AHB 0x004
#define MGB_GCC_GPU_MEMNOC_GFX 0x010
#define MGB_GCC_GPU_SNOC_DVM_GFX 0x018
#define MGB_GCC_DDRSS_GPU_AXI 0x15c        // 0x7115c; sheng has 0x71154
#define MGB_GCC_PLL_VOTE 0x000             // 0x52000, bits 15 and 16
#define MGB_GPLL0_VOTES ((1u << 15) | (1u << 16))

#define MGB_CBCR_CLK_OFF (1u << 31)
#define MGB_CBCR_ENABLE (1u << 0)
#define MGB_RCG_ROOT_OFF (1u << 31)
#define MGB_RCG_CFG_SRC_SEL 0x700u         // bits 10:8
#define MGB_RCG_CFG_DIV 0x1fu              // bits 4:0

// GPUCC (gpucc-cliffs.c). All offsets are byte offsets from MGB_GPUCC_BASE.
#define MGB_CC_PLL0 0x0000                 // 510 MHz config, l=0x1a alpha=0x9000
#define MGB_CC_PLL1 0x1000                 // 440 MHz config, l=0x16 alpha=0xeaaa
#define MGB_CC_PLL_MODE 0x0
#define MGB_CC_PLL_OPMODE 0x4
#define MGB_CC_PLL_L_VAL 0x10
#define MGB_CC_PLL_USER_CTL 0x18
#define MGB_CC_CX_BCR 0x9088
#define MGB_CC_CX_GDSCR 0x908c             // PWR_ON bit31, SW_COLLAPSE bit0, POWER_UP_COMPLETE bit16
#define MGB_CC_CX_GDS_HW_CTRL 0x90a0       // gdsc.c polls PWR_ON here when gds_hw_ctrl is set
#define MGB_CC_GX_BCR 0x9030
#define MGB_CC_GX_GDSCR 0x9034
#define MGB_CC_AHB_CLK 0x90c8              // sheng: 0x911c
#define MGB_CC_CRC_AHB_CLK 0x90cc
#define MGB_CC_SLEEP_CLK 0x90dc
#define MGB_CC_CX_GMU_CLK 0x90e4           // sheng: 0x913c
#define MGB_CC_CXO_CLK 0x90f0              // sheng: 0x9144
#define MGB_CC_HUB_CX_INT_CLK 0x90f4       // sheng: 0x9148
#define MGB_CC_CX_FF_CLK 0x90f8
#define MGB_CC_MEMNOC_GFX_CLK 0x90fc
#define MGB_CC_CX_ACCU_SHIFT_CLK 0x9108    // NOT a gdscr on this SoC
#define MGB_CC_GMU_CLK_SRC 0x9168          // sheng: 0x9318
#define MGB_CC_HUB_AON_CLK 0x9198
#define MGB_CC_HUB_CLK_SRC 0x919c          // sheng: 0x93ec
#define MGB_CC_FF_CLK_SRC 0x91b8
#define MGB_CC_SMMU_VOTE_CLK 0x7000        // same as sheng
#define MGB_CC_MX_CBCR 0x900c              // demet clock lives here on sheng; present but unused here

#define MGB_GDSC_PWR_ON (1u << 31)
#define MGB_GDSC_SW_COLLAPSE (1u << 0)
#define MGB_GDSC_POWER_UP_COMPLETE (1u << 16)

// GPU register window: a6xx.xml RBBM_STATUS is dword 0x0210, and the accessors here scale by 4.
#define MGB_RBBM_STATUS_BYTES (0x0210u * 4u)

// ARM SMMUv2 global space 0 (IHI 0062D.c).
#define MGB_SMMU_SCR0 0x0
#define MGB_SMMU_ID0 0x20
#define MGB_SMMU_ID1 0x24
#define MGB_SMMU_ID2 0x28
#define MGB_SMMU_GFSR 0x48

enum mgb_state { MGB_NOT_RUN = 0, MGB_DONE = 1, MGB_SKIPPED_PREREQ = 2, MGB_SKIPPED_MODE = 3, MGB_NOMAP = 4 };
enum { MGB_WIN_GCC_GPU = 0, MGB_WIN_GCC_VOTE = 1, MGB_WIN_GPUCC = 2, MGB_WIN_SMMU = 3, MGB_WIN_GPU = 4 };

struct mgb_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);   // 0 on success
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);                                     // may be NULL
};

struct mgb_result {
    unsigned mode;
    enum mgb_state gcc, cc, smmu, gpu;
    uint32_t cfg_ahb, cfg_ahb_after, memnoc, dvm, ddrss_axi, pll_vote;
    uint32_t cx_gdscr, gx_gdscr, cx_hw_ctrl, cc_ahb, cc_crc_ahb, cc_smmu, cc_gmu, cc_cxo, cc_hub_cx_int;
    uint32_t cc_accu_shift, cc_sleep, cc_memnoc, cc_cx_ff, cc_hub_aon;
    uint32_t pll1_mode, pll1_opmode, pll1_l, pll1_user;
    uint32_t gmu_rcgr, gmu_cfg, hub_rcgr, hub_cfg;
    uint32_t scr0, id0, id1, id2, gfsr;
    uint32_t rbbm_status;
    unsigned enabled_ahb;
    int io_error;
};

static inline int mgb_branch_enabled(uint32_t cbcr) { return (cbcr & MGB_CBCR_ENABLE) != 0; }
static inline int mgb_branch_running(uint32_t cbcr) { return (cbcr & MGB_CBCR_CLK_OFF) == 0; }
static inline int mgb_gdsc_on(uint32_t gdscr) { return (gdscr & MGB_GDSC_PWR_ON) != 0; }
static inline unsigned mgb_smmu_num_smr(uint32_t id0) { return id0 & 0xff; }
static inline unsigned mgb_smmu_num_cb(uint32_t id1) { return id1 & 0xff; }
static inline uint32_t mgb_rcg_src(uint32_t cfg) { return (cfg & MGB_RCG_CFG_SRC_SEL) >> 8; }
static inline uint32_t mgb_rcg_div(uint32_t cfg) { return (cfg & MGB_RCG_CFG_DIV) + 1; }

static inline int mgb_rd(const struct mgb_io *io, struct mgb_result *r, unsigned window, uint32_t offset, uint32_t *out) {
    uint32_t v = 0;
    if (io->read32(io->cookie, window, offset, &v)) { r->io_error = 1; *out = 0; return -1; }
    *out = v;
    return 0;
}

// Mode 1: read only. Mode 2: additionally enable gcc_gpu_cfg_ahb_clk if its enable bit is clear,
// which is the one write Linux's GCC probe makes. No power domain is ever collapsed/enabled here:
// bringing CX up is a separate, deliberate step.
static inline void mgb_run(const struct mgb_io *io, unsigned mode, unsigned have, struct mgb_result *r) {
    __builtin_memset(r, 0, sizeof *r);
    r->mode = mode;
    if (!mode || !(have & (1u << MGB_WIN_GCC_GPU))) { r->gcc = mode ? MGB_NOMAP : MGB_SKIPPED_MODE; return; }

    // Stage 1: GCC. Safe unconditionally: the cfg_ahb branch is clock-gated but its enable bit reads back.
    if (mgb_rd(io, r, MGB_WIN_GCC_GPU, MGB_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    mgb_rd(io, r, MGB_WIN_GCC_GPU, MGB_GCC_GPU_MEMNOC_GFX, &r->memnoc);
    mgb_rd(io, r, MGB_WIN_GCC_GPU, MGB_GCC_GPU_SNOC_DVM_GFX, &r->dvm);
    mgb_rd(io, r, MGB_WIN_GCC_GPU, MGB_GCC_DDRSS_GPU_AXI, &r->ddrss_axi);
    if (have & (1u << MGB_WIN_GCC_VOTE)) mgb_rd(io, r, MGB_WIN_GCC_VOTE, MGB_GCC_PLL_VOTE, &r->pll_vote);
    r->cfg_ahb_after = r->cfg_ahb;
    if (!mgb_branch_enabled(r->cfg_ahb) && mode >= 2) {
        if (!io->write32(io->cookie, MGB_WIN_GCC_GPU, MGB_GCC_GPU_CFG_AHB, r->cfg_ahb | MGB_CBCR_ENABLE)) {
            r->enabled_ahb = 1;
            mgb_rd(io, r, MGB_WIN_GCC_GPU, MGB_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
        } else r->io_error = 1;
    }
    r->gcc = MGB_DONE;

    // Stage 2: GPUCC needs gcc_gpu_cfg_ahb_clk enabled; otherwise the block may not answer.
    // A blocking/faulting read aborts the stage instead of reading on: the rest of the block is then suspect.
    if (!(have & (1u << MGB_WIN_GPUCC))) r->cc = MGB_NOMAP;
    else if (!mgb_branch_enabled(r->cfg_ahb_after)) r->cc = MGB_SKIPPED_PREREQ;
    else {
        const struct { uint32_t offset; uint32_t *field; } reads[] = {
            {MGB_CC_CX_GDSCR, &r->cx_gdscr},
            {MGB_CC_GX_GDSCR, &r->gx_gdscr},
            {MGB_CC_CX_GDS_HW_CTRL, &r->cx_hw_ctrl},
            {MGB_CC_AHB_CLK, &r->cc_ahb},
            {MGB_CC_CRC_AHB_CLK, &r->cc_crc_ahb},
            {MGB_CC_SMMU_VOTE_CLK, &r->cc_smmu},
            {MGB_CC_CX_GMU_CLK, &r->cc_gmu},
            {MGB_CC_CXO_CLK, &r->cc_cxo},
            {MGB_CC_HUB_CX_INT_CLK, &r->cc_hub_cx_int},
            {MGB_CC_CX_ACCU_SHIFT_CLK, &r->cc_accu_shift},
            {MGB_CC_SLEEP_CLK, &r->cc_sleep},
            {MGB_CC_MEMNOC_GFX_CLK, &r->cc_memnoc},
            {MGB_CC_CX_FF_CLK, &r->cc_cx_ff},
            {MGB_CC_HUB_AON_CLK, &r->cc_hub_aon},
            {MGB_CC_PLL1 + MGB_CC_PLL_MODE, &r->pll1_mode},
            {MGB_CC_PLL1 + MGB_CC_PLL_OPMODE, &r->pll1_opmode},
            {MGB_CC_PLL1 + MGB_CC_PLL_L_VAL, &r->pll1_l},
            {MGB_CC_PLL1 + MGB_CC_PLL_USER_CTL, &r->pll1_user},
            {MGB_CC_GMU_CLK_SRC, &r->gmu_rcgr},
            {MGB_CC_GMU_CLK_SRC + 4, &r->gmu_cfg},
            {MGB_CC_HUB_CLK_SRC, &r->hub_rcgr},
            {MGB_CC_HUB_CLK_SRC + 4, &r->hub_cfg},
        };
        int failed = 0;
        for (unsigned i = 0; i < sizeof reads / sizeof reads[0]; ++i)
            if (mgb_rd(io, r, MGB_WIN_GPUCC, reads[i].offset, reads[i].field)) { failed = 1; break; }
        r->cc = failed ? MGB_NOT_RUN : MGB_DONE;
    }

    // Stage 3: the GPU SMMU needs the CX domain on and its HLOS vote clock running. Global space 0 only.
    if (!(have & (1u << MGB_WIN_SMMU))) r->smmu = MGB_NOMAP;
    else if (r->cc != MGB_DONE || !mgb_gdsc_on(r->cx_gdscr) || !mgb_branch_running(r->cc_smmu)) r->smmu = MGB_SKIPPED_PREREQ;
    else {
        mgb_rd(io, r, MGB_WIN_SMMU, MGB_SMMU_SCR0, &r->scr0);
        mgb_rd(io, r, MGB_WIN_SMMU, MGB_SMMU_ID0, &r->id0);
        mgb_rd(io, r, MGB_WIN_SMMU, MGB_SMMU_ID1, &r->id1);
        mgb_rd(io, r, MGB_WIN_SMMU, MGB_SMMU_ID2, &r->id2);
        mgb_rd(io, r, MGB_WIN_SMMU, MGB_SMMU_GFSR, &r->gfsr);
        r->smmu = MGB_DONE;
    }

    // Stage 4: the GPU window needs both CX and GX on (RBBM_STATUS is in the GX-gated space on gen7).
    if (!(have & (1u << MGB_WIN_GPU))) r->gpu = MGB_NOMAP;
    else if (r->cc != MGB_DONE || !mgb_gdsc_on(r->cx_gdscr) || !mgb_gdsc_on(r->gx_gdscr)) r->gpu = MGB_SKIPPED_PREREQ;
    else {
        mgb_rd(io, r, MGB_WIN_GPU, MGB_RBBM_STATUS_BYTES, &r->rbbm_status);
        r->gpu = MGB_DONE;
    }
}

static inline const char *mgb_state_text(enum mgb_state s) {
    return s == MGB_DONE ? "ok" : s == MGB_SKIPPED_PREREQ ? "skip" : s == MGB_SKIPPED_MODE ? "off"
         : s == MGB_NOMAP ? "nomap" : "-";
}
#endif
