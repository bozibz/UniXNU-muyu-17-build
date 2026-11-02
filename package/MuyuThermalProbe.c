/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* muyu thermal survey: the MMIO half.
 *
 * `muyu-thermal=<n>` (default off):
 *   1  read-only: report TSENS_EN / SW_RST / TRDY / version for the three controllers and
 *      decode every sensor's Sn_STATUS into milliCelsius, rolled up into the four regions
 *      a thermal policy cares about (little CPU, mid+big CPU, GPU, DDR);
 *   2  the same plus one kernel-log line per block with every sensor's temperature.
 *
 * `muyu-thermal-panic=1` additionally panics with the summary line, so the readout is frozen on
 * the panic screen for a photograph (the board has no console log channel that is readable while
 * iOS runs; the panic screen is the reliable one).
 *
 * Read-only by construction: the driver only ever *writes* TSENS_EN on VER_0 IP, and every
 * controller here is v2 (mainline init_common() refuses to run unless the firmware already
 * enabled TSENS and left TRDY set). So this probe never writes a register. If a block reports
 * en=0 or trdy=0 the survey says so and leaves it alone - that is a finding, not a failure to
 * paper over.
 *
 * Register model and the DT-derived sensor map are in MuyuThermal.h; the decode and the row
 * formatting are host-tested by scripts/check-muyu-thermal-host.py.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#include "MuyuThermal.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* The window each controller publishes in DT is 0x1ff bytes; a page is what ml_io_map wants. */
#define MUYU_THERM_WINDOW 0x1000u
#define MUYU_THERM_MAX_SENSORS 16u

/* Filled every survey, and left behind for the throttle that will read it later. */
struct muyu_thermal_snapshot native_thermal_last;
/* The panel row (package/BootProgress.c prints it under the CPU row). Empty = no survey ran. */
char native_thermal_row[112];

static uint32_t therm_read(uintptr_t window, uint32_t offset) {
    return *(volatile uint32_t *)(window + offset);
}

static int therm_map(uint64_t base, uintptr_t *window) {
    vm_offset_t mapped = ml_io_map((vm_offset_t)base, MUYU_THERM_WINDOW);
    if (!mapped) return 0;
    *window = (uintptr_t)mapped;
    return 1;
}

static void therm_log_block(unsigned index, const struct muyu_tsens_block *block,
                            const struct muyu_thermal_snapshot *snapshot, unsigned detailed) {
    const int32_t hottest = muyu_thermal_max_range(snapshot->temps[index], MUYU_THERM_MAX_SENSORS,
                                                  0, MUYU_THERM_MAX_SENSORS - 1);
    IOLog("MUYU THERM %s en=%u trdy=%u max=%d\n", block->name,
          (snapshot->enabled_mask >> index) & 1u, (snapshot->ready_mask >> index) & 1u,
          hottest == MUYU_THERM_NONE ? -1000 : (int)muyu_thermal_degrees(hottest));

    if (!detailed) return;

    char line[160];
    unsigned n = (unsigned)snprintf(line, sizeof line, "MUYU THERM %s deg:", block->name);
    for (unsigned id = 0; id < block->sensors && id < MUYU_THERM_MAX_SENSORS; ++id) {
        int32_t mc = snapshot->temps[index][id];
        if (mc == MUYU_THERM_NONE)
            n += (unsigned)snprintf(line + n, sizeof line - n, " ?");
        else
            n += (unsigned)snprintf(line + n, sizeof line - n, " %d", (int)muyu_thermal_degrees(mc));
    }
    IOLog("%s\n", line);
}

extern void muyu_thermal_probe(void);
void muyu_thermal_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("muyu-thermal", &mode, sizeof mode) || !mode) return;

    unsigned panic_after = 0;
    (void)PE_parse_boot_argn("muyu-thermal-panic", &panic_after, sizeof panic_after);

    struct muyu_thermal_snapshot *snapshot = &native_thermal_last;
    muyu_thermal_block_init(snapshot);

    for (unsigned index = 0; index < MUYU_THERM_BLOCKS; ++index) {
        const struct muyu_tsens_block *block = &kMuyuTsensBlocks[index];
        uintptr_t tm = 0, srot = 0;
        if (!therm_map(block->tm_base, &tm)) {
            IOLog("MUYU THERM %s tm=none\n", block->name);
            continue;
        }
        if (!therm_map(block->srot_base, &srot)) {
            IOLog("MUYU THERM %s srot=none\n", block->name);
            continue;
        }

        const uint32_t version = therm_read(srot, MUYU_TSENS_SROT_HW_VER);
        const uint32_t control = therm_read(srot, MUYU_TSENS_SROT_CTRL);
        const uint32_t trdy = therm_read(tm, MUYU_TSENS_TM_TRDY);
        if (control & MUYU_TSENS_SROT_ENABLED) snapshot->enabled_mask |= 1u << index;
        if (trdy & MUYU_TSENS_TRDY_READY) snapshot->ready_mask |= 1u << index;

        IOLog("MUYU THERM %s ver=%u.%u.%u ctrl=%08x trdy=%08x n=%u\n", block->name,
              (version >> 28) & 0xfu, (version >> 16) & 0xfffu, version & 0xffffu,
              control, trdy, block->sensors);

        for (unsigned id = 0; id < block->sensors && id < MUYU_THERM_MAX_SENSORS; ++id) {
            const uint32_t status =
                therm_read(tm, MUYU_TSENS_TM_SN_STATUS + id * 4u);
            if (!muyu_tsens_status_valid(status)) {
                /* Only a sensor we report on counts as a gap; unused ids are not findings. */
                if (muyu_thermal_id_used(block, id)) ++snapshot->invalid_count;
                continue;
            }
            snapshot->temps[index][id] = muyu_tsens_status_to_mc(status);
            ++snapshot->valid_count;
        }
        therm_log_block(index, block, snapshot, mode >= 2);
    }

    muyu_thermal_summarise(snapshot);
    muyu_thermal_format_row(native_thermal_row, sizeof native_thermal_row, snapshot);
    IOLog("MUYU THERM summary %s valid=%u\n", native_thermal_row, snapshot->valid_count);

    /* Panic last, after the log and the panel row are in place, so both readouts agree. */
    if (panic_after) panic("MUYU THERM %s\n", native_thermal_row);
}
