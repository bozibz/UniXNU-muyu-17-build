/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "MuyuGmuSmmu.h"
#include <string.h>

#define PAGE 4096u

static uint64_t extent_iova(unsigned index)
{
    switch(index) {
    case MUYU_GMU_ICACHE: return MUYU_GMU_ICACHE_IOVA;
    case MUYU_GMU_DUMMY:  return MUYU_GMU_DUMMY_IOVA;
    case MUYU_GMU_LOG:    return MUYU_GMU_LOG_IOVA;
    case MUYU_GMU_HFI:    return MUYU_GMU_HFI_IOVA;
    default:              return MUYU_GMU_DEBUG_IOVA;
    }
}

static size_t extent_bytes(unsigned index)
{
    switch(index) {
    case MUYU_GMU_ICACHE: return MUYU_GMU_ICACHE_BYTES;
    case MUYU_GMU_DUMMY:  return MUYU_GMU_DUMMY_BYTES;
    case MUYU_GMU_DEBUG:  return MUYU_GMU_DEBUG_BYTES;
    case MUYU_GMU_LOG:    return MUYU_GMU_LOG_BYTES;
    default:              return MUYU_GMU_HFI_BYTES;
    }
}

/* Ascending native IOVA order: it is both the order the updater requires and the order
 * the table walk visits. */
static const unsigned sorted[MUYU_GMU_EXTENTS] = {
    MUYU_GMU_ICACHE, MUYU_GMU_DUMMY, MUYU_GMU_LOG, MUYU_GMU_HFI, MUYU_GMU_DEBUG
};

static unsigned permissions(unsigned index)
{
    unsigned perms = ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KPrivileged;
    if (index == MUYU_GMU_ICACHE) perms |= ArmSmmu4KExecute;   /* firmware executes there */
    return perms;
}

static int aligned(uint64_t value) { return !(value & (PAGE - 1)); }
static int overlaps(uint64_t a, uint64_t abytes, uint64_t b, uint64_t bbytes)
{
    return a < b + bbytes && b < a + abytes;
}

size_t MuyuGmuSmmuTablePages(const MuyuGmuSmmuPlan *plan)
{
    (void)plan;
    /* Mirrors ArmSmmu4KLivePrepare's layout() for the fixed aperture below: one level-1
     * root, one level-2 table per 1 GiB span (2 here) and one level-3 table per 2 MiB span
     * (771 here, from 0x4000 to 0x60406fff). Leaf descriptors live inside those pages, so
     * the mapped pages do not need table storage of their own. */
    const uint64_t low = MUYU_GMU_ICACHE_IOVA;
    const uint64_t high = MUYU_GMU_DEBUG_IOVA + MUYU_GMU_DEBUG_BYTES - 1;
    size_t pages = 1;
    for (unsigned level = 1; level < 3; ++level) {
        unsigned shift = 39 - 9 * level;
        pages += (size_t)((high >> shift) - (low >> shift) + 1);
    }
    return pages;
}

size_t MuyuGmuSmmuRanges(const MuyuGmuSmmuPlan *plan, ArmSmmu4KRange *out, size_t capacity)
{
    size_t count = 0;
    if (!plan || !out || capacity < MUYU_GMU_EXTENTS) return 0;
    for (unsigned i = 0; i < MUYU_GMU_EXTENTS; ++i) {
        const unsigned index = sorted[i];
        const MuyuGmuSmmuExtent *e = &plan->extents[index];
        out[count++] = (ArmSmmu4KRange){
            .iova = e->iova, .physical = e->physical, .bytes = e->bytes,
            .permissions = permissions(index),
            .writeback = 0,           /* Normal non-cacheable, as the vendor maps GMU RAM */
            .shareability = 2,        /* outer shareable, the A735 GMU mapping */
        };
    }
    return count;
}

const char *MuyuGmuSmmuOutcomeText(MuyuGmuSmmuOutcome outcome)
{
    switch (outcome) {
    case MuyuGmuSmmuDone:          return "done";
    case MuyuGmuSmmuArgument:      return "argument";
    case MuyuGmuSmmuPrepareFailed: return "prepare";
    case MuyuGmuSmmuPublishFailed: return "publish";
    case MuyuGmuSmmuStartFailed:   return "start";
    case MuyuGmuSmmuMapFailed:     return "map";
    default:                       return "observe";
    }
}

static ArmSmmu4KStatus validate(const ArmSmmu4KIo *io, const MuyuGmuSmmuPlan *plan,
    const MuyuGmuSmmuCache *cache)
{
    if (!io || !plan || !cache || !cache->publish) return ArmSmmu4KArgument;
    if (!io->read32 || !io->read64 || !io->write32 || !io->write64 || !io->barrier ||
        io->register_bytes != SHENG_GPU_SMMU_BYTES) return ArmSmmu4KArgument;
    if (plan->bank < SHENG_GPU_SMMU_GMU_BANK_MIN || plan->bank > 255 ||
        plan->slot > 255 || !plan->poll_budget || plan->poll_budget > 1048576)
        return ArmSmmu4KArgument;
    const MuyuGmuSmmuPool *pool = &plan->pool;
    const uint64_t physical_limit = UINT64_C(1) << MUYU_GMU_OUTPUT_BITS;
    if (!pool->cpu || !aligned((uintptr_t)pool->cpu) || !aligned(pool->physical) ||
        !pool->capacity_pages || pool->capacity_pages > (1u << 20) ||
        pool->physical >= physical_limit ||
        pool->capacity_pages * (uint64_t)PAGE > physical_limit - pool->physical)
        return ArmSmmu4KArgument;
    if (pool->capacity_pages < MuyuGmuSmmuTablePages(plan)) return ArmSmmu4KBudget;
    for (unsigned i = 0; i < MUYU_GMU_EXTENTS; ++i) {
        const MuyuGmuSmmuExtent *e = &plan->extents[i];
        if (!e->cpu || !aligned((uintptr_t)e->cpu) || !aligned(e->physical) ||
            !aligned(e->iova) || !aligned((uint64_t)e->bytes) || !e->bytes ||
            e->iova != extent_iova(i) || e->bytes != extent_bytes(i) ||
            e->physical >= physical_limit ||
            e->bytes > physical_limit - e->physical ||
            e->iova >= (UINT64_C(1) << MUYU_GMU_INPUT_BITS) ||
            e->bytes > (UINT64_C(1) << MUYU_GMU_INPUT_BITS) - e->iova)
            return ArmSmmu4KArgument;
        for (unsigned j = 0; j < i; ++j) {
            const MuyuGmuSmmuExtent *o = &plan->extents[j];
            if (overlaps(e->physical, e->bytes, o->physical, o->bytes) ||
                overlaps(e->iova, e->bytes, o->iova, o->bytes) ||
                overlaps((uintptr_t)e->cpu, e->bytes, (uintptr_t)o->cpu, o->bytes))
                return ArmSmmu4KOverlap;
        }
        if (overlaps(e->physical, e->bytes, pool->physical, pool->capacity_pages * (uint64_t)PAGE) ||
            overlaps((uintptr_t)e->cpu, e->bytes, (uintptr_t)pool->cpu, pool->capacity_pages * (uint64_t)PAGE))
            return ArmSmmu4KOverlap;
    }
    return ArmSmmu4KSuccess;
}

static int publish_page(void *cookie, volatile uint64_t *page, size_t bytes)
{
    MuyuGmuSmmu *state = cookie;
    const MuyuGmuSmmuPool *pool = &state->plan.pool;
    if (!page || !bytes || (bytes & (PAGE - 1)) || ((uintptr_t)page & (PAGE - 1)))
        return 1;
    const uintptr_t base = (uintptr_t)pool->cpu;
    const uintptr_t at = (uintptr_t)page;
    if (at < base || at - base >= pool->capacity_pages * PAGE) return 1;
    if (bytes > pool->capacity_pages * PAGE - (at - base)) return 1;
    return state->publish ? state->publish(state->publish_cookie,
        pool->physical + (at - base), bytes) : 1;
}

MuyuGmuSmmuOutcome MuyuGmuSmmuRun(const ArmSmmu4KIo *io, const MuyuGmuSmmuPlan *plan,
    const MuyuGmuSmmuCache *cache, MuyuGmuSmmu *state)
{
    if (!state) return MuyuGmuSmmuArgument;
    state->publish = cache ? cache->publish : 0;
    state->publish_cookie = cache ? cache->cookie : 0;
    memset(&state->summary, 0, sizeof state->summary);
    state->session = (ShengGpuSmmuSession){0};
    state->state = (ArmSmmu4KLiveState){0};
    state->started = (ShengGpuSmmuResult){0};
    state->observed = (ShengGpuSmmuResult){0};
    state->mapping = (ArmSmmu4KLiveResult){0};

    ArmSmmu4KStatus status = validate(io, plan, cache);
    if (status != ArmSmmu4KSuccess) {
        state->summary.outcome = MuyuGmuSmmuArgument;
        state->summary.prepare_status = status;
        return MuyuGmuSmmuArgument;
    }
    state->plan = *plan;

    ArmSmmu4KRange ranges[MUYU_GMU_EXTENTS];
    const size_t count = MuyuGmuSmmuRanges(&state->plan, ranges, MUYU_GMU_EXTENTS);
    memcpy(state->ranges, ranges, sizeof ranges);
    const MuyuGmuSmmuPool *pool = &state->plan.pool;
    const uint64_t iova = MUYU_GMU_ICACHE_IOVA;
    const uint64_t bytes = MUYU_GMU_DEBUG_IOVA + MUYU_GMU_DEBUG_BYTES - iova;

    state->arena = (ArmSmmu4KLiveArena){
        .config = { MUYU_GMU_INPUT_BITS, MUYU_GMU_OUTPUT_BITS, pool->physical,
                    (size_t)(bytes / PAGE) },
        .iova = iova, .bytes = bytes, .upper_region = 0,
        .mapping = (volatile uint64_t *)pool->cpu,
        .capacity_pages = pool->capacity_pages,
        .alias_scratch = state->alias_scratch,
        .alias_scratch_entries = MUYU_GMU_EXTENTS,
    };
    state->visibility = (ArmSmmu4KLiveVisibility){ state, publish_page };

    /* 1. All intermediate tables, still empty; the GMU is held in reset, so a published
     *    but leaf-less context cannot translate anything yet. */
    memset(pool->cpu, 0, pool->capacity_pages * PAGE);
    state->summary.phase = 1;
    state->summary.capacity_pages = pool->capacity_pages;
    state->summary.arena_iova = iova;
    state->summary.arena_bytes = bytes;
    state->summary.table_physical = pool->physical;
    state->summary.bank = state->plan.bank;
    state->summary.slot = state->plan.slot;
    state->summary.extents = MUYU_GMU_EXTENTS;
    status = ArmSmmu4KLivePrepare(&state->arena, &state->state);
    state->summary.prepare_status = status;
    state->summary.table_pages = state->state.tables.table_pages;
    if (status != ArmSmmu4KSuccess) {
        state->summary.outcome = MuyuGmuSmmuPrepareFailed;
        return MuyuGmuSmmuPrepareFailed;
    }

    /* 2. Publish the tables and the staged GMU image, as the device walks them. */
    state->summary.phase = 2;
    if (cache->publish(cache->cookie, pool->physical,
            state->state.tables.table_pages * (size_t)PAGE)) {
        state->summary.outcome = MuyuGmuSmmuPublishFailed;
        return MuyuGmuSmmuPublishFailed;
    }
    for (unsigned i = 0; i < MUYU_GMU_EXTENTS; ++i)
        if (cache->publish(cache->cookie, state->plan.extents[i].physical,
                state->plan.extents[i].bytes)) {
            state->summary.outcome = MuyuGmuSmmuPublishFailed;
            return MuyuGmuSmmuPublishFailed;
        }

    /* 3. Program the disabled GMU context bank, attach stream 5 and observe it. */
    state->context = (ArmSmmu4KContextConfig){
        .lower = &state->arena.config,
        .upper = 0,
        .lower_tables = &state->state.tables,
        .upper_tables = 0,
        .id0 = 0, .id2 = 0,          /* the programmer overwrites these with real reads */
        .asid = state->plan.bank + 1,
        .coherent_walk = 1,
        .stall_on_fault = 0,
        .continue_after_fault = 1,   /* required by the pinned Adreno SMMU writer */
    };
    state->summary.phase = 3;
    unsigned slots[1] = { state->plan.slot };
    status = ShengGpuSmmuStartLive(io, SHENG_GPU_SMMU_BASE, ShengGpuSmmuGmu, state->plan.bank,
        slots, 1, &state->context, &state->arena, &state->state, state->plan.poll_budget,
        &state->session, &state->started);
    state->summary.start_status = status;
    state->summary.start_phase = state->started.phase;
    state->summary.start_writes = state->started.context.writes;
    state->summary.start_poll_reads = state->started.context.poll_reads;
    state->summary.context_offset = state->started.context.context_offset;
    state->summary.id0 = state->started.context.id0;
    state->summary.id1 = state->started.context.id1;
    state->summary.id2 = state->started.context.id2;
    state->summary.scr0 = state->started.streams.scr0;
    state->summary.global_fault = state->started.streams.global_fault;
    state->summary.streams_bank = state->session.streams.bank;
    state->summary.streams_count = state->session.streams.count;
    if (status != ArmSmmu4KSuccess) {
        state->summary.outcome = MuyuGmuSmmuStartFailed;
        return MuyuGmuSmmuStartFailed;
    }
    state->summary.leaf_pages = state->state.tables.leaf_pages;

    /* 4. Map the five extents one at a time. The single-region updater takes the set of
     *    already-mapped ranges as `before` and re-checks it, so the calls grow that set:
     *    every transaction proves all earlier mappings again, and a failure at extent k
     *    leaves exactly k-1 of them installed and reported. */
    state->summary.phase = 4;
    ArmSmmu4KRange before[MUYU_GMU_EXTENTS];
    for (size_t i = 0; i < count; ++i) {
        state->mapping = (ArmSmmu4KLiveResult){0};
        status = ArmSmmu4KLiveUpdate(&state->arena, &state->state, io, state->plan.bank,
            &state->context, &state->visibility, i ? before : 0, i, &state->ranges[i], 1,
            state->plan.poll_budget, &state->mapping);
        state->summary.map_status = status;
        state->summary.map_published += state->mapping.published_pages;
        state->summary.map_descriptor_writes += state->mapping.descriptor_writes;
        state->summary.leaf_pages = state->state.tables.leaf_pages;
        if (status != ArmSmmu4KSuccess) {
            state->summary.mapped = (unsigned)i;
            state->summary.outcome = MuyuGmuSmmuMapFailed;
            return MuyuGmuSmmuMapFailed;
        }
        before[i] = state->ranges[i];
        state->summary.mapped = (unsigned)i + 1;
    }

    /* 5. Context, streams and every mapped extent again. */
    state->summary.phase = 5;
    status = ShengGpuSmmuObserveLive(io, SHENG_GPU_SMMU_BASE, &state->session,
        state->ranges, count, &state->observed);
    state->summary.observe_status = status;
    if (status != ArmSmmu4KSuccess) {
        state->summary.outcome = MuyuGmuSmmuObserveFailed;
        return MuyuGmuSmmuObserveFailed;
    }
    state->summary.outcome = MuyuGmuSmmuDone;
    return MuyuGmuSmmuDone;
}
