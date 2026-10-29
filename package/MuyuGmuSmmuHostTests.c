/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host test of the board GMU SMMU plan against software register peers.
 *
 * The register-ownership checks are the ones ShengGpuSmmuHostTests.c already enforces for
 * the board lifecycle (only owned bank/slot/global registers may be written, a context bank
 * is never disabled before its streams are proved FAULT). What is new here is the muyu
 * plan: bank >= 2 for stream 5, a 4 MiB pool whose intermediate tables cover the whole
 * [icache,debug] aperture and the five exact extents, each mapped in its own transaction. */
#include "MuyuGmuSmmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define POOL_PAGES 1024u

static uint32_t regs[SHENG_GPU_SMMU_BYTES / 4];
static struct {
    unsigned calls, fail_call, after, writes, route_fail, fault_fail, timeout;
} f;
static unsigned g_bank = SHENG_GPU_SMMU_GMU_BANK_MIN, g_slot = 2, g_cb;
static ArmSmmu4KIo io;
static unsigned publish_calls, publish_count;
static uint64_t publishes[64];

static int event(void) { return ++f.calls == f.fail_call; }

static int read32(void *cookie, size_t at, uint32_t *v)
{
    CHECK(cookie == &f && at % 4 == 0 && at + 4 <= sizeof regs);
    CHECK(at != 0x4000);   /* the QSMMU IMPL_DEF page: an external abort on the board */
    if (event()) return 1;
    *v = (at == g_cb + 0x7f4 && f.timeout) ? 1 : regs[at / 4];
    return 0;
}

static int read64(void *cookie, size_t at, uint64_t *v)
{
    CHECK(cookie == &f && at % 8 == 0 && at + 8 <= sizeof regs);
    if (event()) return 1;
    memcpy(v, regs + at / 4, 8);
    return 0;
}

static int write32(void *cookie, size_t at, uint32_t v)
{
    CHECK(cookie == &f && at % 4 == 0 && at + 4 <= sizeof regs);
    CHECK(at != 0x4000);
    ++f.writes;
    int fail = event();
    if (fail && !f.after) return 1;
    unsigned own = at == 0x1000 + 4 * g_bank || at == 0x1800 + 4 * g_bank ||
                   (at >= g_cb && at < g_cb + 4096) ||
                   at == 0x800 + 4 * g_slot || at == 0xc00 + 4 * g_slot;
    CHECK(own);   /* no other bank, slot, global or device register */
    if (at == g_cb && !v)
        CHECK(((regs[(0xc00 + 4 * g_slot) / 4] >> 16) & 3) == 2);   /* stream already FAULT */
    if (f.fault_fail && (v & 0x30000) == 0x20000 && at >= 0xc00 && at < 0x1000) return 1;
    if (at == g_cb + 0x58) regs[at / 4] &= ~v; else regs[at / 4] = v;
    if (f.route_fail && at >= 0xc00 && at < 0x1000 && !(v & 0x30000)) return 1;
    return fail;
}

static int write64(void *cookie, size_t at, uint64_t v)
{
    CHECK(cookie == &f && (at == g_cb + 0x20 || at == g_cb + 0x28));
    ++f.writes;
    int fail = event();
    if (fail && !f.after) return 1;
    memcpy(regs + at / 4, &v, 8);
    return fail;
}

static int barrier(void *cookie) { CHECK(cookie == &f); return event(); }

static int publish(void *cookie, uint64_t physical, size_t bytes)
{
    CHECK(cookie == &f && physical && bytes && bytes % 4096 == 0);
    ++publish_calls;
    if (publish_count < sizeof publishes / sizeof publishes[0]) publishes[publish_count++] = physical;
    return 0;
}

static MuyuGmuSmmuCache cache = { &f, publish };

static void reset_peer(void)
{
    memset(regs, 0, sizeof regs);
    memset(&f, 0, sizeof f);
    g_bank = SHENG_GPU_SMMU_GMU_BANK_MIN;
    g_slot = 2;
    g_cb = 0x20000 + g_bank * 4096;
    publish_calls = publish_count = 0;
    io = (ArmSmmu4KIo){ &f, sizeof regs, read32, read64, write32, write64, barrier };
    /* The real muyu readings (2026-10-29): sCR0 needs the four-bit enable, and IDR0/1/2
     * carry only their low byte. MuyuGmuSmmuRun must still build a 0x22000 context from
     * them; the peer fails on any access to 0x4000, the address the unstripped IDR1 would
     * derive (and the QSMMU page that aborts on the device). */
    regs[0] = 0x406;   /* the state the probe's four-bit write establishes */
    regs[0x20 / 4] = 24u;
    regs[0x24 / 4] = 22u;
    regs[0x28 / 4] = 0x02;
    for (unsigned i = 0; i < 24; ++i) regs[(0xc00 + 4 * i) / 4] = 0x20000;   /* all FAULT */
}

static _Alignas(4096) uint8_t pool[POOL_PAGES * 4096];
static _Alignas(4096) uint8_t icache_mem[MUYU_GMU_ICACHE_BYTES];
static _Alignas(4096) uint8_t dummy_mem[MUYU_GMU_DUMMY_BYTES];
static _Alignas(4096) uint8_t debug_mem[MUYU_GMU_DEBUG_BYTES];
static _Alignas(4096) uint8_t log_mem[MUYU_GMU_LOG_BYTES];
static _Alignas(4096) uint8_t hfi_mem[MUYU_GMU_HFI_BYTES];

static MuyuGmuSmmuPlan make_plan(void)
{
    MuyuGmuSmmuPlan p;
    memset(&p, 0, sizeof p);
    p.pool = (MuyuGmuSmmuPool){ pool, UINT64_C(0x90000000), POOL_PAGES };
    p.extents[MUYU_GMU_ICACHE] = (MuyuGmuSmmuExtent){ icache_mem, UINT64_C(0x80000000), MUYU_GMU_ICACHE_IOVA, sizeof icache_mem };
    p.extents[MUYU_GMU_DUMMY] = (MuyuGmuSmmuExtent){ dummy_mem, UINT64_C(0x98000000), MUYU_GMU_DUMMY_IOVA, sizeof dummy_mem };
    p.extents[MUYU_GMU_DEBUG] = (MuyuGmuSmmuExtent){ debug_mem, UINT64_C(0x98010000), MUYU_GMU_DEBUG_IOVA, sizeof debug_mem };
    p.extents[MUYU_GMU_LOG] = (MuyuGmuSmmuExtent){ log_mem, UINT64_C(0x98020000), MUYU_GMU_LOG_IOVA, sizeof log_mem };
    p.extents[MUYU_GMU_HFI] = (MuyuGmuSmmuExtent){ hfi_mem, UINT64_C(0x98030000), MUYU_GMU_HFI_IOVA, sizeof hfi_mem };
    p.bank = SHENG_GPU_SMMU_GMU_BANK_MIN;
    p.slot = 2;
    p.poll_budget = 1000;
    return p;
}

static MuyuGmuSmmu state;

static void refuse(MuyuGmuSmmuPlan bad)
{
    reset_peer();
    CHECK(MuyuGmuSmmuRun(&io, &bad, &cache, &state) == MuyuGmuSmmuArgument);
    CHECK(state.summary.outcome == MuyuGmuSmmuArgument);
    CHECK(!f.writes && !publish_calls);
}

int main(void)
{
    const MuyuGmuSmmuPlan plan = make_plan();
    const size_t expected_tables = MuyuGmuSmmuTablePages(&plan);
    /* 1 level-1 root + 2 level-2 tables + 771 level-3 tables over the whole aperture. */
    CHECK(expected_tables == 774);
    CHECK(expected_tables <= POOL_PAGES);

    reset_peer();
    {
        const MuyuGmuSmmuOutcome o = MuyuGmuSmmuRun(&io, &plan, &cache, &state);
        if (o != MuyuGmuSmmuDone)
            fprintf(stderr, "outcome=%s phase=%u prepare=%u start=%u map=%u observe=%u\n",
                MuyuGmuSmmuOutcomeText(o), state.summary.phase, state.summary.prepare_status,
                state.summary.start_status, state.summary.map_status, state.summary.observe_status);
        CHECK(o == MuyuGmuSmmuDone);
    }
    CHECK(state.summary.prepare_status == ArmSmmu4KSuccess);
    CHECK(state.summary.start_status == ArmSmmu4KSuccess);
    CHECK(state.summary.map_status == ArmSmmu4KSuccess);
    CHECK(state.summary.observe_status == ArmSmmu4KSuccess);
    CHECK(state.summary.phase == 5 && state.summary.outcome == MuyuGmuSmmuDone);
    CHECK(state.summary.table_pages == expected_tables);
    CHECK(state.summary.leaf_pages == 4111);   /* 4092 icache + 4 dummy + 7 debug + 4 log + 4 hfi */
    CHECK(state.summary.context_offset == 0x22000u && g_cb == 0x22000u);
    /* The library saw the capability view, not the stripped raw registers. */
    CHECK(state.summary.id0 == (regs[0x20 / 4] | MUYU_GMU_ID0_VIEW));
    CHECK(state.summary.id1 == ((4u << 28) | 22u));
    CHECK(state.summary.id2 == (MUYU_GMU_ID2_VIEW | 0x2u));
    CHECK(state.summary.scr0 == 0x406);
    CHECK(state.summary.streams_bank == SHENG_GPU_SMMU_GMU_BANK_MIN);
    CHECK(state.summary.streams_count == 1);
    CHECK(state.session.streams.streams[0].sid == SHENG_GPU_SMMU_GMU_SID);
    CHECK(state.session.streams.streams[0].slot == 2);
    /* The observe pass ends the lifecycle at phase 4 and does write the bank's registers. */
    CHECK(state.summary.start_phase == 4 && f.writes > 0);
    CHECK(state.summary.map_published >= 10u && state.summary.mapped == MUYU_GMU_EXTENTS);
    CHECK(state.summary.extents == MUYU_GMU_EXTENTS);
    CHECK(state.state.tables.leaf_pages == 4111u);
    CHECK(state.mapping.complete && !state.mapping.must_retain_resources);
    /* One publish of the prepared tables, one per extent, then the map's own pages. */
    CHECK(publish_count > 6u && publishes[0] == plan.pool.physical);
    CHECK(publishes[1] == plan.extents[MUYU_GMU_ICACHE].physical);
    /* The programmed descriptors: ascending IOVA, fixed IOVAs, icache executable. */
    ArmSmmu4KRange out[MUYU_GMU_EXTENTS];
    CHECK(MuyuGmuSmmuRanges(&plan, out, MUYU_GMU_EXTENTS) == MUYU_GMU_EXTENTS);
    for (unsigned i = 1; i < MUYU_GMU_EXTENTS; ++i) CHECK(out[i - 1].iova < out[i].iova);
    CHECK(out[0].iova == MUYU_GMU_ICACHE_IOVA && out[0].bytes == MUYU_GMU_ICACHE_BYTES);
    CHECK(out[1].iova == MUYU_GMU_DUMMY_IOVA);
    CHECK(out[2].iova == MUYU_GMU_LOG_IOVA);
    CHECK(out[3].iova == MUYU_GMU_HFI_IOVA);
    CHECK(out[4].iova == MUYU_GMU_DEBUG_IOVA && out[4].bytes == MUYU_GMU_DEBUG_BYTES);
    CHECK((out[0].permissions & ArmSmmu4KExecute) && out[0].permissions ==
          (ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged));
    for (unsigned i = 1; i < MUYU_GMU_EXTENTS; ++i)
        CHECK(!(out[i].permissions & ArmSmmu4KExecute) &&
              out[i].permissions == (ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KPrivileged));
    for (unsigned i = 0; i < MUYU_GMU_EXTENTS; ++i)
        CHECK(out[i].writeback == 0 && out[i].shareability == 2);

    /* Every malformed plan is refused before the first write and before any publish. */
    MuyuGmuSmmuPlan bad = make_plan();
    bad.bank = SHENG_GPU_SMMU_GMU_BANK_MIN - 1; refuse(bad);
    bad = make_plan(); bad.bank = 0; refuse(bad);
    bad = make_plan(); bad.slot = 256; refuse(bad);
    bad = make_plan(); bad.poll_budget = 0; refuse(bad);
    bad = make_plan(); bad.poll_budget = 1048577; refuse(bad);
    bad = make_plan(); bad.extents[MUYU_GMU_ICACHE].bytes -= 4096; refuse(bad);
    bad = make_plan(); bad.extents[MUYU_GMU_ICACHE].iova = 0x5000; refuse(bad);
    bad = make_plan(); bad.extents[MUYU_GMU_LOG].iova = MUYU_GMU_DUMMY_IOVA; refuse(bad);
    bad = make_plan(); bad.extents[MUYU_GMU_LOG].physical += 16; refuse(bad);
    bad = make_plan(); bad.extents[MUYU_GMU_HFI].physical = bad.extents[MUYU_GMU_DUMMY].physical; refuse(bad);
    bad = make_plan(); bad.pool.capacity_pages = expected_tables - 1; refuse(bad);
    bad = make_plan(); bad.pool.cpu = NULL; refuse(bad);
    bad = make_plan(); bad.extents[MUYU_GMU_DEBUG].cpu = (void *)((uintptr_t)debug_mem + 8); refuse(bad);
    {
        ArmSmmu4KIo narrow = io;
        narrow.register_bytes = 0x1000;
        reset_peer();
        CHECK(MuyuGmuSmmuRun(&narrow, &plan, &cache, &state) == MuyuGmuSmmuArgument);
        CHECK(!f.writes && !publish_calls);
        MuyuGmuSmmuCache nocache = { &f, NULL };
        reset_peer();
        CHECK(MuyuGmuSmmuRun(&io, &plan, &nocache, &state) == MuyuGmuSmmuArgument);
        CHECK(!f.writes && !publish_calls);
    }

    /* Every I/O failure leaves the SMMU retained and never reports success. */
    reset_peer();
    CHECK(MuyuGmuSmmuRun(&io, &plan, &cache, &state) == MuyuGmuSmmuDone);
    const unsigned normal = f.calls;
    CHECK(normal > 100u);
    unsigned start_failures = 0, map_failures = 0, observe_failures = 0;
    for (unsigned at = 1; at <= normal; ++at) {
        for (unsigned after = 0; after < 2; ++after) {
            reset_peer();
            f.fail_call = at;
            f.after = after;
            const MuyuGmuSmmuOutcome outcome = MuyuGmuSmmuRun(&io, &plan, &cache, &state);
            CHECK(outcome != MuyuGmuSmmuDone && outcome != MuyuGmuSmmuArgument);
            CHECK(state.summary.outcome == outcome);
            if (outcome == MuyuGmuSmmuStartFailed) ++start_failures;
            else if (outcome == MuyuGmuSmmuMapFailed) ++map_failures;
            else { CHECK(outcome == MuyuGmuSmmuObserveFailed); ++observe_failures; }
            if (outcome == MuyuGmuSmmuStartFailed && state.session.attempted)
                CHECK(state.session.poisoned && state.state.poisoned);
        }
    }
    /* A context fault or a timeout in the owned bank is reported, never swallowed. */
    reset_peer();
    f.route_fail = f.fault_fail = 1;
    CHECK(MuyuGmuSmmuRun(&io, &plan, &cache, &state) == MuyuGmuSmmuStartFailed);
    CHECK(state.summary.start_status == ArmSmmu4KIoError && state.session.poisoned);
    reset_peer();
    f.timeout = 1;
    CHECK(MuyuGmuSmmuRun(&io, &plan, &cache, &state) == MuyuGmuSmmuStartFailed);
    CHECK(state.summary.start_status == ArmSmmu4KTimeout);

    printf("PASS: %u table pages, %u extents mapped transaction by transaction, bank %u slot %u stream %u\n",
           (unsigned)state.summary.table_pages, MUYU_GMU_EXTENTS, g_bank, g_slot,
           SHENG_GPU_SMMU_GMU_SID);
    printf("PASS: %u startup and %u map and %u observe I/O failures retained, no bank disable "
           "before stream fault; software register peers only\n",
           start_failures, map_failures, observe_failures);
    return 0;
}
