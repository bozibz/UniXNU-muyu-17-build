// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the muyu gated GPU power observation sequence (software register peer, not hardware).
#include "MuyuGpuPowerBoard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct model {
    uint32_t cfg_ahb, memnoc, dvm, ddrss, vote;
    uint32_t cx, gx, hwctrl, ccahb, smmu, cxo, gmu;
    uint32_t pll1_mode, pll1_opmode, pll1_l, pll1_user, gmu_cfg, hub_cfg;
    uint32_t scr0, id0, id1, id2, gfsr, rbbm;
    unsigned cc_reads, smmu_reads, gpu_reads, writes;
    unsigned last_write_offset;
    uint32_t last_write_value;
    unsigned violations;     // reads of a window whose prerequisite was not met
    int fail_cc_read;
};

static int rd(void *c, unsigned w, uint32_t o, uint32_t *out) {
    struct model *m = c;
    if (w > MGB_WIN_GPU) return -1;
    switch (w) {
    case MGB_WIN_GCC_GPU:
        *out = o == MGB_GCC_GPU_CFG_AHB ? m->cfg_ahb : o == MGB_GCC_GPU_MEMNOC_GFX ? m->memnoc
             : o == MGB_GCC_GPU_SNOC_DVM_GFX ? m->dvm : o == MGB_GCC_DDRSS_GPU_AXI ? m->ddrss : 0xdead0001u;
        return 0;
    case MGB_WIN_GCC_VOTE:
        *out = m->vote;
        return 0;
    case MGB_WIN_GPUCC:
        if (!(m->cfg_ahb & MGB_CBCR_ENABLE)) ++m->violations;
        if (m->fail_cc_read) return -1;
        ++m->cc_reads;
        *out = o == MGB_CC_CX_GDSCR ? m->cx : o == MGB_CC_GX_GDSCR ? m->gx
             : o == MGB_CC_CX_GDS_HW_CTRL ? m->hwctrl : o == MGB_CC_AHB_CLK ? m->ccahb
             : o == MGB_CC_SMMU_VOTE_CLK ? m->smmu : o == MGB_CC_CXO_CLK ? m->cxo
             : o == MGB_CC_CX_GMU_CLK ? m->gmu : o == MGB_CC_PLL1 + MGB_CC_PLL_MODE ? m->pll1_mode
             : o == MGB_CC_PLL1 + MGB_CC_PLL_OPMODE ? m->pll1_opmode
             : o == MGB_CC_PLL1 + MGB_CC_PLL_L_VAL ? m->pll1_l
             : o == MGB_CC_PLL1 + MGB_CC_PLL_USER_CTL ? m->pll1_user
             : o == MGB_CC_GMU_CLK_SRC + 4 ? m->gmu_cfg : o == MGB_CC_HUB_CLK_SRC + 4 ? m->hub_cfg : 0xdead0002u;
        return 0;
    case MGB_WIN_SMMU:
        if (!(m->cx & MGB_GDSC_PWR_ON) || (m->smmu & MGB_CBCR_CLK_OFF)) ++m->violations;
        ++m->smmu_reads;
        *out = o == MGB_SMMU_SCR0 ? m->scr0 : o == MGB_SMMU_ID0 ? m->id0 : o == MGB_SMMU_ID1 ? m->id1
             : o == MGB_SMMU_ID2 ? m->id2 : o == MGB_SMMU_GFSR ? m->gfsr : 0xdead0003u;
        return 0;
    case MGB_WIN_GPU:
        if (!(m->cx & MGB_GDSC_PWR_ON) || !(m->gx & MGB_GDSC_PWR_ON)) ++m->violations;
        ++m->gpu_reads;
        *out = m->rbbm;
        return 0;
    }
    return -1;
}

static int wr(void *c, unsigned w, uint32_t o, uint32_t v) {
    struct model *m = c;
    ++m->writes; m->last_write_offset = o; m->last_write_value = v;
    if (w != MGB_WIN_GCC_GPU || o != MGB_GCC_GPU_CFG_AHB) ++m->violations;   // only one sanctioned write
    m->cfg_ahb = v;
    return 0;
}

static struct model populated(void) {
    struct model m;
    memset(&m, 0, sizeof m);
    m.cfg_ahb = 0x80000001u;                 // enable bit set, hw-gated
    m.memnoc = 0x00000001u; m.dvm = 0x00000001u; m.ddrss = 0x00000001u; m.vote = MGB_GPLL0_VOTES;
    m.cx = 0x80228801u;                      // PWR_ON | retention bits | SW_COLLAPSE
    m.gx = 0x80222801u;
    m.hwctrl = 0xa800010au;
    m.ccahb = 0x80000001u; m.smmu = 0x00000001u; m.cxo = 0x80000001u; m.gmu = 0x00000001u;
    m.pll1_mode = 0x8000001cu; m.pll1_opmode = 1u; m.pll1_l = 0x16u; m.pll1_user = 5u;
    m.gmu_cfg = 0x00000303u; m.hub_cfg = 0x00000505u;
    m.scr0 = 0x00000000u; m.id0 = 0x00000018u; m.id1 = 0x00000016u; m.id2 = 0x00000002u; m.gfsr = 0;
    m.rbbm = 0x00000001u;
    return m;
}

static void test_mode1_reads_when_enabled(void) {
    struct model m = populated();
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GCC_VOTE) | (1u << MGB_WIN_GPUCC) |
                    (1u << MGB_WIN_SMMU) | (1u << MGB_WIN_GPU), &r);
    CHECK(m.violations == 0);
    CHECK(r.gcc == MGB_DONE && r.cc == MGB_DONE && r.smmu == MGB_DONE && r.gpu == MGB_DONE);
    CHECK(m.writes == 0);
    CHECK(r.cfg_ahb == 0x80000001u && r.cfg_ahb_after == r.cfg_ahb);
    CHECK(r.cx_gdscr == m.cx && r.gx_gdscr == m.gx && r.cx_hw_ctrl == m.hwctrl);
    CHECK(r.cc_smmu == m.smmu && r.cc_gmu == m.gmu && r.gmu_cfg == m.gmu_cfg);
    CHECK(mgb_rcg_src(r.gmu_cfg) == 3 && mgb_rcg_div(r.gmu_cfg) == 4);
    CHECK(mgb_smmu_num_smr(r.id0) == 0x18 && mgb_smmu_num_cb(r.id1) == 0x16);
    CHECK(r.rbbm_status == m.rbbm);
    CHECK(mgb_gdsc_on(r.cx_gdscr) && mgb_gdsc_on(r.gx_gdscr));
}

static void test_mode1_never_writes(void) {
    struct model m = populated();
    m.cfg_ahb = 0x80000000u;                 // enable bit clear
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC), &r);
    CHECK(m.writes == 0);
    CHECK(r.cc == MGB_SKIPPED_PREREQ);       // GPUCC must not be touched
    CHECK(m.violations == 0);
}

static void test_mode2_enables_ahb_once(void) {
    struct model m = populated();
    m.cfg_ahb = 0x80000000u;
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 2, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC), &r);
    CHECK(m.writes == 1);
    CHECK(m.last_write_offset == MGB_GCC_GPU_CFG_AHB && m.last_write_value == 0x80000001u);
    CHECK(r.enabled_ahb == 1 && r.cfg_ahb_after == 0x80000001u);
    CHECK(r.cc == MGB_DONE);
    CHECK(m.violations == 0);
}

static void test_mode2_is_idempotent(void) {
    struct model m = populated();            // enable bit already set
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 2, (1u << MGB_WIN_GCC_GPU), &r);
    CHECK(m.writes == 0 && r.enabled_ahb == 0);
}

static void test_smmu_and_gpu_gated(void) {
    struct model m = populated();
    m.cx = 0x00228801u;                      // PWR_ON clear
    m.smmu = 0x80000001u;                    // vote clock not running
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_SMMU) | (1u << MGB_WIN_GPU), &r);
    CHECK(r.smmu == MGB_SKIPPED_PREREQ && r.gpu == MGB_SKIPPED_PREREQ);
    CHECK(m.smmu_reads == 0 && m.gpu_reads == 0);
    CHECK(m.violations == 0);

    struct model n = populated();
    n.cx = 0x80228801u; n.smmu = 0x80000001u; n.gx = 0x00222801u;   // CX on, vote running, GX off
    struct mgb_io io2 = { &n, rd, wr, NULL };
    struct mgb_result r2;
    mgb_run(&io2, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_SMMU) | (1u << MGB_WIN_GPU), &r2);
    CHECK(r2.smmu == MGB_SKIPPED_PREREQ);    // vote clock still gated
    CHECK(r2.gpu == MGB_SKIPPED_PREREQ);     // GX off
}

static void test_missing_windows(void) {
    struct model m = populated();
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 1, 0, &r);
    CHECK(r.gcc == MGB_NOMAP);
    struct mgb_result r2;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU), &r2);
    CHECK(r2.cc == MGB_NOMAP && r2.smmu == MGB_NOMAP && r2.gpu == MGB_NOMAP);
    struct mgb_result r3;
    mgb_run(&io, 0, 0xff, &r3);
    CHECK(r3.gcc == MGB_SKIPPED_MODE);
}

static void test_io_error_is_recorded(void) {
    struct model m = populated();
    m.fail_cc_read = 1;
    struct mgb_io io = { &m, rd, wr, NULL };
    struct mgb_result r;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_SMMU) | (1u << MGB_WIN_GPU), &r);
    CHECK(r.io_error == 1);
    CHECK(r.cc != MGB_DONE);
    CHECK(r.smmu == MGB_SKIPPED_PREREQ && r.gpu == MGB_SKIPPED_PREREQ);
}

int main(void) {
    test_mode1_reads_when_enabled();
    test_mode1_never_writes();
    test_mode2_enables_ahb_once();
    test_mode2_is_idempotent();
    test_smmu_and_gpu_gated();
    test_missing_windows();
    test_io_error_is_recorded();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("PASS: %d cases, cliffs offsets, gating, one sanctioned write, no out-of-prerequisite reads\n", 7);
    return 0;
}
