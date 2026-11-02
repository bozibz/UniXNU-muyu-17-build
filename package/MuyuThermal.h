/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* muyu thermal survey: the pure half.
 *
 * Everything in this header is register arithmetic on values the caller already read, so it
 * builds and runs on the host (package/MuyuThermalHostTests.c) as well as in the kernel
 * archive. The MMIO side is package/MuyuThermalProbe.c.
 *
 * Register model: mainline drivers/thermal/qcom/tsens-v2.c, whose `tsens_v2_regfields` is the
 * only published layout for this IP. This board's DT names the three controllers with the plain
 * "qcom,tsens-v2" compatible (no SoC-specific variant), so `feat->adc` is 0 and each Sn_STATUS
 * word carries deciCelsius directly - there is no ADC code and no per-sensor calibration in the
 * reading path. That is the single fact that decides the decode below; if a future DT names a
 * variant with .adc = 1, the value needs code_to_degc() and the nvmem calibration instead.
 *
 * DT provenance (fw/muyu_dt.txt, "thermal-zones"):
 *   tsens0 @0x0c228000 (SROT 0x0c222000), 14 sensors: 1,2 = cpuss-0/1, 3..10 = cpu-1-*-*,
 *                                                      11..13 = cpu-2-*-*  -> the mid/big CPUs
 *   tsens1 @0x0c229000 (SROT 0x0c223000), 16 sensors: 1..3 = cpu-0-*-*, 4..7 = nsphvx/nsphmx,
 *                                                      8..11 = gpuss-0..3, 12 = video,
 *                                                      13 = ddr, 14..15 = camera
 *   tsens2 @0x0c22a000 (SROT 0x0c224000),  5 sensors: 0 = aoss-2, 1..4 = mdmss-0..3 (modem)
 */
#ifndef MUYU_THERMAL_H
#define MUYU_THERMAL_H

#include <stddef.h>
#include <stdint.h>

/* The kernel gets snprintf from <libkern/libkern.h>, host tests from <stdio.h>; both must be
 * included before this header. Redirect through a macro so a freestanding compile (the archive
 * check) can supply its own declaration without this header dragging either one in. */
#ifndef MUYU_THERMAL_SNPRINTF
#define MUYU_THERMAL_SNPRINTF snprintf
#endif

/* ----- qcom,tsens-v2 register map (tsens-v2.c) ------------------------------------ */
#define MUYU_TSENS_SROT_HW_VER      0x0000u  /* VER_MAJOR 31:28, VER_MINOR 27:16, VER_STEP 15:0 */
#define MUYU_TSENS_SROT_CTRL        0x0004u  /* bit0 TSENS_EN, bit1 TSENS_SW_RST */
#define MUYU_TSENS_TM_TRDY          0x00e4u  /* mainline says bit0 = fresh result; see the note below */
#define MUYU_TSENS_TM_SN_STATUS     0x00a0u  /* one word per sensor, stride 4 */
#define MUYU_TSENS_STATUS_TEMP_LSB  0
#define MUYU_TSENS_STATUS_TEMP_MSB  11
#define MUYU_TSENS_STATUS_VALID_BIT 21

#define MUYU_TSENS_SROT_ENABLED     (1u << 0)
#define MUYU_TSENS_SROT_SW_RESET    (1u << 1)
/* mainline's tsens-v2 layout puts "results ready" in TRDY bit0. On this board TRDY reads 0x8
 * (bit3) while every Sn_STATUS word has VALID set and plausible temperatures (2026-11-02,
 * vendor_boot_thermal2: trdy=00000008, inv=0, valid=35), so bit0 is not a usable gate here.
 * The survey records TRDY for the log but gates nothing on it; VALID is the only readiness
 * signal it trusts. */
#define MUYU_TSENS_TRDY_READY       (1u << 0)

/* ----- board layout --------------------------------------------------------------- */
#define MUYU_THERM_BLOCKS 3

struct muyu_tsens_block {
    const char *name;
    uint64_t tm_base;
    uint64_t srot_base;
    unsigned sensors;
    unsigned cpu_first, cpu_last;   /* inclusive sensor ids of this block's CPU cluster */
    unsigned gpu_first, gpu_last;   /* inclusive sensor ids of this block's GPU, 0 = none */
    unsigned ddr_id;                /* sensor id of DDR, 0xffff = none */
};

/* The sensor ids are the DT ones (see the header comment), not array indexes. */
static const struct muyu_tsens_block kMuyuTsensBlocks[MUYU_THERM_BLOCKS] = {
    /* tsens0: cpuss-0/1 at 1/2 and the cpu-1/cpu-2 cores at 3..13; no GPU, no DDR. */
    {"t0", 0x0c228000ull, 0x0c222000ull, 14, 1, 13, 0, 0, 0xffffu},
    /* tsens1: cpu-0 (little) at 1..3, GPU at 8..11, DDR at 13. */
    {"t1", 0x0c229000ull, 0x0c223000ull, 16, 1, 3, 8, 11, 13},
    /* tsens2: modem only; kept because the survey has to say what it saw there too. */
    {"t2", 0x0c22a000ull, 0x0c224000ull, 5, 1, 0, 0, 0, 0xffffu},
};

#define MUYU_THERM_NONE INT32_MIN

/* ----- decode ---------------------------------------------------------------------- */

/* One Sn_STATUS word: bits 11:0 are deciCelsius, two's complement; bit 21 is VALID. */
static inline int muyu_tsens_status_valid(uint32_t status) {
    return (int)((status >> MUYU_TSENS_STATUS_VALID_BIT) & 1u);
}

static inline uint32_t muyu_tsens_status_code(uint32_t status) {
    return (status >> MUYU_TSENS_STATUS_TEMP_LSB) &
           ((1u << (MUYU_TSENS_STATUS_TEMP_MSB - MUYU_TSENS_STATUS_TEMP_LSB + 1)) - 1u);
}

/* DeciCelsius -> milliCelsius, sign extended from bit 11. */
static inline int32_t muyu_tsens_status_to_mc(uint32_t status) {
    int32_t deci = (int32_t)muyu_tsens_status_code(status);
    if (deci & 0x0800) deci -= 0x1000;
    return deci * 100;
}

/* MilliCelsius -> whole degrees, rounded away from zero at the half degree. */
static inline int32_t muyu_thermal_degrees(int32_t mc) {
    return mc >= 0 ? (mc + 500) / 1000 : -(((-mc) + 500) / 1000);
}

/* Highest value in [first,last] of `temps`, or MUYU_THERM_NONE when the range is empty/invalid. */
static inline int32_t muyu_thermal_max_range(const int32_t *temps, unsigned count,
                                             unsigned first, unsigned last) {
    int32_t best = MUYU_THERM_NONE;
    if (last < first || last >= count) return MUYU_THERM_NONE;
    for (unsigned i = first; i <= last; ++i)
        if (temps[i] != MUYU_THERM_NONE && temps[i] > best) best = temps[i];
    return best;
}

/* Does this sensor id belong to a region the policy reports (CPU, GPU, DDR)? Sensors outside
 * that set are still read, but a missing VALID bit there is not a finding - id 0 of tsens0/1 has
 * no thermal zone on this board. */
static inline int muyu_thermal_id_used(const struct muyu_tsens_block *block, unsigned id) {
    if (block->cpu_last >= block->cpu_first && id >= block->cpu_first && id <= block->cpu_last)
        return 1;
    if (block->gpu_first != 0 && id >= block->gpu_first && id <= block->gpu_last) return 1;
    if (block->ddr_id != 0xffffu && id == block->ddr_id) return 1;
    return 0;
}

/* ----- survey snapshot ------------------------------------------------------------- */

struct muyu_thermal_snapshot {
    int32_t temps[MUYU_THERM_BLOCKS][16];  /* milliCelsius per sensor id, MUYU_THERM_NONE if unusable */
    unsigned enabled_mask;                 /* bit per block whose SROT CTRL says TSENS_EN */
    unsigned ready_mask;                   /* bit per block whose TRDY bit says ready */
    unsigned valid_count;                  /* sensors that answered with VALID set */
    unsigned invalid_count;                /* sensors whose VALID bit was clear */
    int32_t little_mc, big_mc, gpu_mc, ddr_mc, hot_mc;
};

static inline void muyu_thermal_block_init(struct muyu_thermal_snapshot *s) {
    for (unsigned b = 0; b < MUYU_THERM_BLOCKS; ++b)
        for (unsigned i = 0; i < 16; ++i) s->temps[b][i] = MUYU_THERM_NONE;
    s->enabled_mask = s->ready_mask = 0;
    s->valid_count = s->invalid_count = 0;
    s->little_mc = s->big_mc = s->gpu_mc = s->ddr_mc = s->hot_mc = MUYU_THERM_NONE;
}

/* Roll the per-sensor values up into the four regions a thermal policy cares about. */
static inline void muyu_thermal_summarise(struct muyu_thermal_snapshot *s) {
    s->little_mc = muyu_thermal_max_range(s->temps[1], 16, 1, 3);
    s->big_mc = muyu_thermal_max_range(s->temps[0], 16, 1, 13);
    s->gpu_mc = muyu_thermal_max_range(s->temps[1], 16, 8, 11);
    s->ddr_mc = muyu_thermal_max_range(s->temps[1], 16, 13, 13);

    int32_t hot = MUYU_THERM_NONE;
    for (unsigned b = 0; b < MUYU_THERM_BLOCKS; ++b) {
        int32_t m = muyu_thermal_max_range(s->temps[b], 16, 0, 15);
        if (m != MUYU_THERM_NONE && (hot == MUYU_THERM_NONE || m > hot)) hot = m;
    }
    s->hot_mc = hot;
}

/* One line, no address or register noise: what the panel row and the kernel log both carry. */
static inline void muyu_thermal_format_row(char *out, size_t len,
                                           const struct muyu_thermal_snapshot *s) {
    char little[16], big[16], gpu[16], ddr[16], hot[16];
    char *cells[5] = {little, big, gpu, ddr, hot};
    const int32_t src[5] = {s->little_mc, s->big_mc, s->gpu_mc, s->ddr_mc, s->hot_mc};
    for (unsigned i = 0; i < 5; ++i) {
        if (src[i] == MUYU_THERM_NONE)
            (void)MUYU_THERMAL_SNPRINTF(cells[i], 16, "?");
        else
            (void)MUYU_THERMAL_SNPRINTF(cells[i], 16, "%d", (int)muyu_thermal_degrees(src[i]));
    }
    (void)MUYU_THERMAL_SNPRINTF(out, len, "L=%s B=%s G=%s D=%s hot=%s en=%u%u%u inv=%u",
                   little, big, gpu, ddr, hot,
                   (s->enabled_mask >> 0) & 1u, (s->enabled_mask >> 1) & 1u,
                   (s->enabled_mask >> 2) & 1u, s->invalid_count);
}

/* ----- trend ---------------------------------------------------------------------- */

/* A handful of timed "hottest sensor" marks, plus the peak over the whole run. Enough to see
 * whether the part is still climbing after a minute, which is the question a throttle has to
 * answer; the exact curve is not needed. */
#define MUYU_THERM_MARKS 6

struct muyu_thermal_trend {
    unsigned at[MUYU_THERM_MARKS];   /* seconds, ascending */
    int32_t hot_mc[MUYU_THERM_MARKS];
    unsigned count;
    int32_t peak_mc;
};

static inline void muyu_thermal_trend_init(struct muyu_thermal_trend *t) {
    for (unsigned i = 0; i < MUYU_THERM_MARKS; ++i) {
        t->at[i] = 0;
        t->hot_mc[i] = MUYU_THERM_NONE;
    }
    t->count = 0;
    t->peak_mc = MUYU_THERM_NONE;
}

static inline void muyu_thermal_trend_note(struct muyu_thermal_trend *t, unsigned second,
                                           int32_t hot_mc) {
    if (hot_mc != MUYU_THERM_NONE && (t->peak_mc == MUYU_THERM_NONE || hot_mc > t->peak_mc))
        t->peak_mc = hot_mc;
    if (t->count >= MUYU_THERM_MARKS) return;
    t->at[t->count] = second;
    t->hot_mc[t->count] = hot_mc;
    ++t->count;
}

/* "h0=29 h15=33 h30=35 h60=37 pk=37" - the marks only, no register noise. */
static inline void muyu_thermal_format_trend(char *out, size_t len,
                                             const struct muyu_thermal_trend *t) {
    unsigned n = 0;
    for (unsigned i = 0; i < t->count && n < len; ++i) {
        char cell[16];
        if (t->hot_mc[i] == MUYU_THERM_NONE)
            (void)MUYU_THERMAL_SNPRINTF(cell, sizeof cell, "h%u=?", t->at[i]);
        else
            (void)MUYU_THERMAL_SNPRINTF(cell, sizeof cell, "h%u=%d", t->at[i],
                                        (int)muyu_thermal_degrees(t->hot_mc[i]));
        n += (unsigned)MUYU_THERMAL_SNPRINTF(out + n, len - n, i ? " %s" : "%s", cell);
        if (n >= len) break;
    }
    char peak[16];
    if (t->peak_mc == MUYU_THERM_NONE)
        (void)MUYU_THERMAL_SNPRINTF(peak, sizeof peak, "?");
    else
        (void)MUYU_THERMAL_SNPRINTF(peak, sizeof peak, "%d", (int)muyu_thermal_degrees(t->peak_mc));
    (void)MUYU_THERMAL_SNPRINTF(out + (n < len ? n : len - 1), len - (n < len ? n : len - 1),
                                n ? " pk=%s" : "pk=%s", peak);
}

#endif /* MUYU_THERMAL_H */
