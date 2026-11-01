// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MuyuA735Hwcg.h"

int MuyuA735ApplyHwcg(MuyuA735Write32 write32, void *cookie, unsigned *failed_at) {
    if (failed_at) *failed_at = 0xffffffffu;
    if (!write32) return -1;
    for (unsigned i = 0; i < MUYU_A735_HWCG_COUNT; ++i) {
        if (write32(cookie, MuyuA735Hwcg[i][0], MuyuA735Hwcg[i][1])) {
            if (failed_at) *failed_at = i;
            return -1;
        }
    }
    return (int)MUYU_A735_HWCG_COUNT;
}
