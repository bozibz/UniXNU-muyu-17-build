/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * A735 (gen7-11-0) hardware-init (hwcg) application.
 *
 * The toolchain driver applies no hwcg table at all today, and the A740 table it ships is the
 * wrong value set for A735: nine registers carry different clock-gating values (see
 * scripts/gen-muyu-a735-hwinit.py and HANDOFF §16). This module writes the A735 table through a
 * caller-supplied 32-bit write callback, so it can be host-tested and wired into the GPU init path
 * without touching the official driver.
 *
 * The offsets are the same A7XX register map as A740; only the values differ.
 */
#ifndef MUYU_A735_HWCG_H
#define MUYU_A735_HWCG_H

#include <stdint.h>
#include "MuyuA735HwInit.h"

/* Chip id that selects the a730-family entry (gmu_chipid 0x07110000). */
#define MUYU_A735_GMU_CHIP_ID 0x07110000u

typedef int (*MuyuA735Write32)(void *cookie, uint32_t reg, uint32_t value);

/* Write every hwcg entry. Returns the number written, or -1 if the callback fails (the failing
 * register's index is stored in *failed_at when it is not null). */
int MuyuA735ApplyHwcg(MuyuA735Write32 write32, void *cookie, unsigned *failed_at);

/* 1 when `gmu_chip_id` selects the A735/a730-family table. */
static inline int MuyuA735Matches(uint32_t gmu_chip_id)
{
    return gmu_chip_id == MUYU_A735_GMU_CHIP_ID;
}

#endif /* MUYU_A735_HWCG_H */
