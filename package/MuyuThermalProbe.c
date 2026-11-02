/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* muyu thermal survey: the MMIO half.
 *
 * `muyu-thermal=<seconds>` (absent = off) is the whole interface, because this board's kernel
 * command line has a 1000-byte budget and every extra argument eats into it:
 *
 *   0        one-shot read-only survey at boot: log version/ctrl/trdy per controller, decode every
 *            sensor, publish the summary row. No panic, so the boot continues to iOS.
 *   N > 0    survey now, then keep reading once a second in a kernel thread for N seconds and
 *            panic with the summary and the trend. The thread starts before userland, so the
 *            samples span the boot and compositor load - a boot-time snapshot alone only ever
 *            shows a cold device. The panic is what freezes both rows on the screen for a
 *            photograph: this board has no console log channel readable while iOS runs.
 *
 * The trend row records the hottest sensor at 0/15/30/60/120/180 s (only the marks inside the
 * window), plus the peak of the whole run.
 *
 * Read-only by construction: the driver only ever *writes* TSENS_EN on VER_0 IP, and every
 * controller here is v2 (mainline init_common() refuses to run unless the firmware already
 * enabled TSENS and left TRDY set). So this probe never writes a register. If a block reports
 * en=0 or trdy=0 the survey says so and leaves it alone - that is a finding, not a failure to
 * paper over.
 *
 * Register model and the DT-derived sensor map are in MuyuThermal.h; the decode, the summary,
 * the row and the trend text are host-tested by scripts/check-muyu-thermal-host.py.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "MuyuThermal.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern void IOSleep(unsigned milliseconds);

/* The window each controller publishes in DT is 0x1ff bytes; a page is what ml_io_map wants. */
#define MUYU_THERM_WINDOW 0x1000u
#define MUYU_THERM_MAX_SENSORS 16u
#define MUYU_THERM_TREND_LEN 112

/* The seconds a trend mark is taken at. Ascending; the sampler compares each second against these. */
static const unsigned kThermMarkSeconds[MUYU_THERM_MARKS] = {0, 15, 30, 60, 90, 120, 150, 180};

/* Mapped once: there is no unmap here, and a per-second resample must not leak mappings. */
static struct { uintptr_t tm, srot; } gThermWindow[MUYU_THERM_BLOCKS];
static int gThermMapped;

/* Filled every survey, and left behind for the throttle that will read it later. */
struct muyu_thermal_snapshot native_thermal_last;
/* Panel rows (package/BootProgress.c prints them under the CPU row). Empty = no survey ran. */
char native_thermal_row[112];
char native_thermal_trend[MUYU_THERM_TREND_LEN];
static struct muyu_thermal_trend gThermTrend;

/* Sampling window, set before the thread starts. */
static unsigned gThermSampleSeconds;
static unsigned gThermSecond;

static uint32_t therm_read(uintptr_t window, uint32_t offset) {
    return *(volatile uint32_t *)(window + offset);
}

static int therm_map_all(void) {
    if (gThermMapped) return 1;
    int mapped = 0;
    for (unsigned index = 0; index < MUYU_THERM_BLOCKS; ++index) {
        const struct muyu_tsens_block *desc = &kMuyuTsensBlocks[index];
        vm_offset_t tm = ml_io_map((vm_offset_t)desc->tm_base, MUYU_THERM_WINDOW);
        vm_offset_t srot = ml_io_map((vm_offset_t)desc->srot_base, MUYU_THERM_WINDOW);
        gThermWindow[index].tm = (uintptr_t)tm;
        gThermWindow[index].srot = (uintptr_t)srot;
        if (tm && srot) {
            ++mapped;
        } else {
            IOLog("MUYU THERM %s map failed tm=%d srot=%d\n", desc->name, tm ? 1 : 0,
                  srot ? 1 : 0);
        }
    }
    gThermMapped = 1;
    return mapped;
}

static void therm_survey(struct muyu_thermal_snapshot *snapshot, int verbose) {
    muyu_thermal_block_init(snapshot);

    for (unsigned index = 0; index < MUYU_THERM_BLOCKS; ++index) {
        const struct muyu_tsens_block *desc = &kMuyuTsensBlocks[index];
        const uintptr_t tm = gThermWindow[index].tm;
        const uintptr_t srot = gThermWindow[index].srot;
        if (!tm || !srot) continue;

        const uint32_t version = therm_read(srot, MUYU_TSENS_SROT_HW_VER);
        const uint32_t control = therm_read(srot, MUYU_TSENS_SROT_CTRL);
        const uint32_t trdy = therm_read(tm, MUYU_TSENS_TM_TRDY);
        if (control & MUYU_TSENS_SROT_ENABLED) snapshot->enabled_mask |= 1u << index;
        if (trdy & MUYU_TSENS_TRDY_READY) snapshot->ready_mask |= 1u << index;

        if (verbose)
            IOLog("MUYU THERM %s ver=%u.%u.%u ctrl=%08x trdy=%08x n=%u\n", desc->name,
                  (version >> 28) & 0xfu, (version >> 16) & 0xfffu, version & 0xffffu,
                  control, trdy, desc->sensors);

        for (unsigned id = 0; id < desc->sensors && id < MUYU_THERM_MAX_SENSORS; ++id) {
            const uint32_t status = therm_read(tm, MUYU_TSENS_TM_SN_STATUS + id * 4u);
            if (!muyu_tsens_status_valid(status)) {
                /* Only a sensor we report on counts as a gap; unused ids are not findings. */
                if (muyu_thermal_id_used(desc, id)) ++snapshot->invalid_count;
                continue;
            }
            snapshot->temps[index][id] = muyu_tsens_status_to_mc(status);
            ++snapshot->valid_count;
        }
    }

    muyu_thermal_summarise(snapshot);
}

static void therm_log_block(unsigned index, const struct muyu_tsens_block *desc,
                            const struct muyu_thermal_snapshot *snapshot, unsigned detailed) {
    const int32_t hottest = muyu_thermal_max_range(snapshot->temps[index], MUYU_THERM_MAX_SENSORS,
                                                  0, MUYU_THERM_MAX_SENSORS - 1);
    IOLog("MUYU THERM %s en=%u trdy=%u max=%d\n", desc->name,
          (snapshot->enabled_mask >> index) & 1u, (snapshot->ready_mask >> index) & 1u,
          hottest == MUYU_THERM_NONE ? -1000 : (int)muyu_thermal_degrees(hottest));

    if (!detailed) return;

    char line[160];
    unsigned n = (unsigned)snprintf(line, sizeof line, "MUYU THERM %s deg:", desc->name);
    for (unsigned id = 0; id < desc->sensors && id < MUYU_THERM_MAX_SENSORS; ++id) {
        int32_t mc = snapshot->temps[index][id];
        if (mc == MUYU_THERM_NONE)
            n += (unsigned)snprintf(line + n, sizeof line - n, " ?");
        else
            n += (unsigned)snprintf(line + n, sizeof line - n, " %d", (int)muyu_thermal_degrees(mc));
    }
    IOLog("%s\n", line);
}

static void therm_publish(void) {
    muyu_thermal_format_row(native_thermal_row, sizeof native_thermal_row, &native_thermal_last);
    muyu_thermal_format_trend(native_thermal_trend, sizeof native_thermal_trend, &gThermTrend);
}

/* One sample, publish, and take a trend mark when the second matches one. */
static void therm_step(unsigned second) {
    therm_survey(&native_thermal_last, second == 0);
    for (unsigned i = 0; i < MUYU_THERM_MARKS; ++i)
        if (kThermMarkSeconds[i] == second) {
            muyu_thermal_trend_note(&gThermTrend, second, native_thermal_last.hot_mc);
            break;
        }
    therm_publish();
    /* Also log the trend every 15 s: a boot that leaves through the critical-exit path dumps the
     * last ~1.5 KiB of log on the panel, and the trend has to be inside that window to be read. */
    if (second && second % 15 == 0) IOLog("MUYU THERM trend %s\n", native_thermal_trend);
}

static void therm_sampler(void *arg, wait_result_t result) {
    (void)arg;
    (void)result;
    for (;;) {
        IOSleep(1000);
        ++gThermSecond;
        therm_step(gThermSecond);
        if (gThermSampleSeconds && gThermSecond >= gThermSampleSeconds) break;
    }
    IOLog("MUYU THERM trend %s\n", native_thermal_trend);
    /* A sampling run always ends in the panic: that is what freezes the two rows for a photo. */
    panic("MUYU THERM %s\n%s\n", native_thermal_row, native_thermal_trend);
}

extern void muyu_thermal_probe(void);
void muyu_thermal_probe(void) {
    unsigned window = 0;
    if (!PE_parse_boot_argn("muyu-thermal", &window, sizeof window)) return;

    if (!therm_map_all()) {
        IOLog("MUYU THERM no controller mapped\n");
        return;
    }

    muyu_thermal_trend_init(&gThermTrend);
    therm_step(0);

    IOLog("MUYU THERM summary %s valid=%u\n", native_thermal_row, native_thermal_last.valid_count);
    for (unsigned index = 0; index < MUYU_THERM_BLOCKS; ++index)
        therm_log_block(index, &kMuyuTsensBlocks[index], &native_thermal_last, 1);

    if (!window) return;   /* one-shot: the boot continues, the row stays as this snapshot */

    gThermSampleSeconds = window;
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start((thread_continue_t)therm_sampler, NULL, &thread) != KERN_SUCCESS)
        IOLog("MUYU THERM sampler thread failed\n");
    else
        thread_deallocate(thread);
}
