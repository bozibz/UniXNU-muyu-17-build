// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the A735 hwcg application.
#include "MuyuA735Hwcg.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct sink {
    uint32_t reg[128];
    uint32_t value[128];
    unsigned n;
    unsigned fail_at;
};

static int write_ok(void *cookie, uint32_t reg, uint32_t value) {
    struct sink *s = cookie;
    if (s->n >= 128) return -1;
    if (s->fail_at && s->n + 1 == s->fail_at) return -1;
    s->reg[s->n] = reg;
    s->value[s->n] = value;
    ++s->n;
    return 0;
}

static void test_apply_all(void) {
    struct sink s;
    memset(&s, 0, sizeof s);
    unsigned failed_at = 0xffffffffu;
    int written = MuyuA735ApplyHwcg(write_ok, &s, &failed_at);
    CHECK(written == MUYU_A735_HWCG_COUNT);
    CHECK(failed_at == 0xffffffffu);
    CHECK(s.n == MUYU_A735_HWCG_COUNT);
    for (unsigned i = 0; i < MUYU_A735_HWCG_COUNT; ++i) {
        CHECK(s.reg[i] == MuyuA735Hwcg[i][0]);
        CHECK(s.value[i] == MuyuA735Hwcg[i][1]);
    }
}

static void test_apply_failure(void) {
    struct sink s;
    memset(&s, 0, sizeof s);
    s.fail_at = 7;
    unsigned failed_at = 0xffffffffu;
    int written = MuyuA735ApplyHwcg(write_ok, &s, &failed_at);
    CHECK(written == -1);
    CHECK(failed_at == 6);
    CHECK(s.n == 6);
}

static void test_chip_select(void) {
    CHECK(MuyuA735Matches(0x07110000u) == 1);
    CHECK(MuyuA735Matches(0x07030001u) == 0);   /* gen7-03-00 */
    CHECK(MuyuA735Matches(0x00000000u) == 0);
}

static void test_table_shape(void) {
    /* The A735 table must carry the a730 hwcg values, not the A740 ones. */
    int seen_mode_cp = 0, seen_hyst_uche = 0;
    for (unsigned i = 0; i < MUYU_A735_HWCG_COUNT; ++i) {
        if (MuyuA735Hwcg[i][0] == MUYU_A735_RBBM_CLOCK_MODE_CP) {
            CHECK(MuyuA735Hwcg[i][1] == 0x00000223u);
            seen_mode_cp = 1;
        }
        if (MuyuA735Hwcg[i][0] == MUYU_A735_RBBM_CLOCK_HYST_UCHE) {
            CHECK(MuyuA735Hwcg[i][1] == 0x00000004u);
            seen_hyst_uche = 1;
        }
    }
    CHECK(seen_mode_cp && seen_hyst_uche);
}

int main(void) {
    test_apply_all();
    test_apply_failure();
    test_chip_select();
    test_table_shape();
    if (failures) {
        printf("MuyuA735Hwcg host tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("MuyuA735Hwcg host tests OK (%d entries)\n", MUYU_A735_HWCG_COUNT);
    return 0;
}
