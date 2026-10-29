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


// ---- mode 4: clock bring-up, over a register-bank peer ----------------------------------------------
struct bank {
    uint32_t cc[0x9200 / 4];
    uint32_t gccg[0x1000 / 4];
    uint32_t gccv[0x1000 / 4];
    unsigned writes, violations;
    int lock_never;        // PLL1 never reports LOCK_DET
    int cx_stuck;          // the CX domain never reports PWR_ON
    int root_off_stuck;    // RCG never leaves ROOT_OFF
    int update_stuck;      // CMD_UPDATE never clears
};

static int brd(void *c, unsigned w, uint32_t o, uint32_t *out) {
    struct bank *b = c;
    if (o & 3 || o + 4 > 0x9200) { ++b->violations; return -1; }
    switch (w) {
    case MGB_WIN_GCC_GPU: *out = b->gccg[o / 4]; return 0;
    case MGB_WIN_GCC_VOTE: *out = b->gccv[o / 4]; return 0;
    case MGB_WIN_GPUCC: {
        uint32_t v = b->cc[o / 4];
        if (o == MGB_CC_PLL1 + MGB_CC_PLL_MODE && !b->lock_never) v |= MGB_PLL_LOCK_DET;
        else if (o == MGB_CC_PLL1 + MGB_CC_PLL_MODE) v &= ~MGB_PLL_LOCK_DET;
        if ((o == MGB_CC_HUB_CLK_SRC || o == MGB_CC_GMU_CLK_SRC)) {
            v &= ~MGB_RCG_ROOT_OFF;                          // the clock source reports 0 once ROOT_EN is set
            if (b->root_off_stuck) v |= MGB_RCG_ROOT_OFF;
            if (b->update_stuck) v |= MGB_RCG_UPDATE;
            else { v &= ~MGB_RCG_UPDATE; b->cc[o / 4] &= ~MGB_RCG_UPDATE; }
            b->cc[o / 4] = (b->cc[o / 4] & ~MGB_RCG_ROOT_OFF) | (v & MGB_RCG_ROOT_OFF);
        }
        if (o == MGB_CC_HUB_CLK_SRC + 4 || o == MGB_CC_GMU_CLK_SRC + 4) v = b->cc[o / 4];
        *out = v; return 0;
    }
    default: return -1;
    }
}

static int bwr(void *c, unsigned w, uint32_t o, uint32_t v) {
    struct bank *b = c;
    if (o & 3 || o + 4 > 0x9200) { ++b->violations; return -1; }
    ++b->writes;
    switch (w) {
    case MGB_WIN_GCC_GPU: b->gccg[o / 4] = v; return 0;
    case MGB_WIN_GCC_VOTE: b->gccv[o / 4] = v; return 0;
    case MGB_WIN_GPUCC:
        b->cc[o / 4] = v;
        if (o == MGB_CC_CX_GDSCR) {
            if (!(v & MGB_GDSC_SW_COLLAPSE) && !b->cx_stuck) {
                b->cc[o / 4] |= MGB_GDSC_PWR_ON;
                b->cc[MGB_CC_CX_GDS_HW_CTRL / 4] = MGB_GDSC_PWR_ON;
            } else {
                b->cc[o / 4] &= ~MGB_GDSC_PWR_ON;
                b->cc[MGB_CC_CX_GDS_HW_CTRL / 4] = 0;
            }
        }
        return 0;
    default: return -1;
    }
}

static void bank_ready(struct bank *b) {
    memset(b, 0, sizeof *b);
    b->gccg[MGB_GCC_GPU_CFG_AHB / 4] = 0x80000001u;      // AHB already enabled, hw gated
    b->cc[MGB_CC_AHB_CLK / 4] = 0x00000000u;             // GPUCC AHB running
}

static void test_clock_bringup_succeeds(void) {
    struct bank b; bank_ready(&b);
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; struct mgb_clock_result c;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GCC_VOTE) | (1u << MGB_WIN_GPUCC), &r);
    CHECK(r.cc == MGB_DONE);
    mgb_gmu_clocks_on(&io, &r, (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_GCC_VOTE), &c);
    CHECK(c.failed == 0 && c.step == 7);
    CHECK((b.gccv[MGB_GCC_PLL_VOTE / 4] & MGB_GPLL0_VOTES) == MGB_GPLL0_VOTES);
    CHECK((b.cc[(MGB_CC_PLL1 + MGB_CC_PLL_MODE) / 4] & MGB_PLL_OUTCTRL) != 0);
    CHECK((b.cc[(MGB_CC_PLL1 + MGB_CC_PLL_MODE) / 4] & MGB_PLL_RESET_N) != 0);
    CHECK(mgb_rcg_src(b.cc[(MGB_CC_GMU_CLK_SRC + 4) / 4]) == MGB_GMU_SRC_SEL);
    CHECK(mgb_rcg_div(b.cc[(MGB_CC_GMU_CLK_SRC + 4) / 4]) == MGB_GMU_DIV_FIELD + 1);
    CHECK(mgb_rcg_src(b.cc[(MGB_CC_HUB_CLK_SRC + 4) / 4]) == MGB_HUB_SRC_SEL);
    CHECK(mgb_rcg_div(b.cc[(MGB_CC_HUB_CLK_SRC + 4) / 4]) == MGB_HUB_DIV_FIELD + 1);
    CHECK((b.cc[MGB_CC_CX_GMU_CLK / 4] & MGB_CBCR_ENABLE) != 0);
    CHECK((b.cc[MGB_CC_SMMU_VOTE_CLK / 4] & MGB_CBCR_ENABLE) != 0);
    CHECK(b.violations == 0);
}

static void test_clock_bringup_requires_readable_gpucc(void) {
    struct bank b; bank_ready(&b);
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; struct mgb_clock_result c;
    memset(&r, 0, sizeof r);
    r.gcc = MGB_DONE; r.cfg_ahb_after = 0x80000001u; r.cc = MGB_SKIPPED_PREREQ;
    mgb_gmu_clocks_on(&io, &r, (1u << MGB_WIN_GPUCC), &c);
    CHECK(c.failed == 3 && c.step == 1);
    CHECK(b.writes == 0);
}

static void test_clock_bringup_reports_lock_timeout(void) {
    struct bank b; bank_ready(&b); b.lock_never = 1;
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; struct mgb_clock_result c;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC), &r);
    mgb_gmu_clocks_on(&io, &r, (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_GCC_VOTE), &c);
    CHECK(c.failed == 1 && c.step == 3 && c.lock_polls > 0);
    CHECK((b.cc[(MGB_CC_PLL1 + MGB_CC_PLL_MODE) / 4] & MGB_PLL_OUTCTRL) == 0);  // outputs never turned on
}

static void test_clock_bringup_reports_rcg_timeout(void) {
    struct bank b; bank_ready(&b); b.root_off_stuck = 1;
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; struct mgb_clock_result c;
    mgb_run(&io, 1, (1u << MGB_WIN_GCC_GPU) | (1u << MGB_WIN_GPUCC), &r);
    mgb_gmu_clocks_on(&io, &r, (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_GCC_VOTE), &c);
    CHECK(c.failed == 1 && c.step == 4);
}


static void test_cx_already_on(void) {
    struct bank b; bank_ready(&b);
    b.cc[MGB_CC_CX_GDSCR / 4] = MGB_GDSC_PWR_ON | 0x800u;
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; uint32_t gdscr = 0, hw = 0; unsigned polls = 0;
    __builtin_memset(&r, 0, sizeof r);
    CHECK(mgb_power_cx(&io, &r, &gdscr, &hw, &polls) == 0);
    CHECK(b.writes == 0 && polls == 0);
}

static void test_cx_turns_on_with_retention(void) {
    struct bank b; bank_ready(&b);
    b.cc[MGB_CC_CX_GDSCR / 4] = MGB_GDSC_SW_COLLAPSE | 0x800u;
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; uint32_t gdscr = 0, hw = 0; unsigned polls = 0;
    __builtin_memset(&r, 0, sizeof r);
    CHECK(mgb_power_cx(&io, &r, &gdscr, &hw, &polls) == 1);
    CHECK(mgb_gdsc_on(gdscr));
    CHECK((gdscr & (MGB_GDSC_RETAIN_MEM | MGB_GDSC_RETAIN_PERIPH | MGB_GDSC_RETAIN_FF)) ==
          (MGB_GDSC_RETAIN_MEM | MGB_GDSC_RETAIN_PERIPH | MGB_GDSC_RETAIN_FF));
    CHECK(polls >= 1 && mgb_gdsc_on(hw));
    CHECK(b.writes == 3);                       // collapse clear, then two retention updates
}

static void test_cx_never_powers(void) {
    struct bank b; bank_ready(&b); b.cx_stuck = 1;
    b.cc[MGB_CC_CX_GDSCR / 4] = MGB_GDSC_SW_COLLAPSE | 0x800u;
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; uint32_t gdscr = 0, hw = 0; unsigned polls = 0;
    __builtin_memset(&r, 0, sizeof r);
    CHECK(mgb_power_cx(&io, &r, &gdscr, &hw, &polls) == 2);
    CHECK(!mgb_gdsc_on(gdscr) && polls >= 1 && polls <= 210);
}


static void test_observe_powered_reads_smmu_and_gpu(void) {
    struct bank b; bank_ready(&b);
    b.cc[MGB_CC_CX_GDSCR / 4] = MGB_GDSC_PWR_ON;
    b.cc[MGB_CC_GX_GDSCR / 4] = MGB_GDSC_PWR_ON;
    b.cc[MGB_CC_CX_GDS_HW_CTRL / 4] = MGB_GDSC_PWR_ON;
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; __builtin_memset(&r, 0, sizeof r);
    mgb_observe_powered(&io, &r, (1u << MGB_WIN_GPUCC));
    CHECK(r.smmu == MGB_NOMAP && r.gpu == MGB_NOMAP);   // windows not mapped: no reads
}

static void test_observe_powered_gates_on_state(void) {
    struct bank b; bank_ready(&b);
    b.cc[MGB_CC_CX_GDSCR / 4] = MGB_GDSC_SW_COLLAPSE;    // CX still off
    b.cc[MGB_CC_SMMU_VOTE_CLK / 4] = 0x80000000u;        // vote clock gated
    struct mgb_io io = { &b, brd, bwr, NULL };
    struct mgb_result r; __builtin_memset(&r, 0, sizeof r);
    mgb_observe_powered(&io, &r, (1u << MGB_WIN_GPUCC) | (1u << MGB_WIN_SMMU) | (1u << MGB_WIN_GPU));
    CHECK(r.smmu == MGB_SKIPPED_PREREQ && r.gpu == MGB_SKIPPED_PREREQ);
    CHECK(b.violations == 0);
}

int main(void) {
    test_mode1_reads_when_enabled();
    test_mode1_never_writes();
    test_mode2_enables_ahb_once();
    test_mode2_is_idempotent();
    test_smmu_and_gpu_gated();
    test_missing_windows();
    test_io_error_is_recorded();
    test_clock_bringup_succeeds();
    test_clock_bringup_requires_readable_gpucc();
    test_clock_bringup_reports_lock_timeout();
    test_clock_bringup_reports_rcg_timeout();
    test_cx_already_on();
    test_cx_turns_on_with_retention();
    test_cx_never_powers();
    test_observe_powered_reads_smmu_and_gpu();
    test_observe_powered_gates_on_state();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("PASS: 16 cases, cliffs offsets, gating, one sanctioned write, clock bring-up order and timeouts, CX power\n");
    return 0;
}
