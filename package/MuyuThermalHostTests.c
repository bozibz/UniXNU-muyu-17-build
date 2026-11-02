/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests for muyu's TSENS decode and thermal row (package/MuyuThermal.h).
 *
 * Software fixtures only: every status word below is a synthetic 32-bit Sn_STATUS value shaped
 * like the qcom,tsens-v2 layout, and nothing here touches hardware or the device tree. Their job
 * is to pin the two things a reading hangs on - the deciCelsius sign extension and which sensor
 * ids roll up into which region - so a later refactor cannot quietly turn 25 C into 250 C.
 *
 * scripts/check-muyu-thermal-host.py compiles and runs this, and separately re-derives the
 * sensor map from fw/muyu_dt.txt so the constants in the header cannot drift from the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MuyuThermal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "thermal line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* Sn_STATUS word from a deciCelsius value, with VALID set unless told otherwise. */
static uint32_t status_of(int deci_celsius, int valid) {
    return ((uint32_t)(deci_celsius & 0xfff)) | (valid ? (1u << MUYU_TSENS_STATUS_VALID_BIT) : 0u);
}

static void test_decode(void) {
    /* 25.0 C */
    CHECK(muyu_tsens_status_valid(status_of(250, 1)) == 1);
    CHECK(muyu_tsens_status_to_mc(status_of(250, 1)) == 25000);
    /* 125.0 C: the top of what the firmware's own trip range allows */
    CHECK(muyu_tsens_status_to_mc(status_of(1250, 1)) == 125000);
    /* -40.0 C: deciCelsius 0xe70, i.e. two's complement across bit 11 */
    CHECK(muyu_tsens_status_to_mc(status_of(0xe70, 1)) == -40000);
    /* -1.0 C is the sign-extension boundary */
    CHECK(muyu_tsens_status_to_mc(status_of(0xfff, 1)) == -100);
    /* 0 C is not "no reading" - VALID is what says that */
    CHECK(muyu_tsens_status_to_mc(status_of(0, 1)) == 0);
    CHECK(muyu_tsens_status_valid(status_of(250, 0)) == 0);
    /* Bits outside 11:0 must not leak into the temperature. */
    CHECK(muyu_tsens_status_to_mc(0x00100FA | (1u << 12)) == 25000);
}

static void test_degrees(void) {
    CHECK(muyu_thermal_degrees(25000) == 25);
    CHECK(muyu_thermal_degrees(0) == 0);
    CHECK(muyu_thermal_degrees(-40000) == -40);
    CHECK(muyu_thermal_degrees(25499) == 25);   /* rounds down below the half degree */
    CHECK(muyu_thermal_degrees(25500) == 26);   /* rounds up at it */
    CHECK(muyu_thermal_degrees(-25500) == -26); /* and away from zero for negatives */
    CHECK(muyu_thermal_degrees(125000) == 125); /* the firmware's own upper trip */
}

static void test_ranges(void) {
    int32_t temps[16];
    for (unsigned i = 0; i < 16; ++i) temps[i] = MUYU_THERM_NONE;
    CHECK(muyu_thermal_max_range(temps, 16, 1, 3) == MUYU_THERM_NONE);
    temps[2] = 40000;
    CHECK(muyu_thermal_max_range(temps, 16, 1, 3) == 40000);
    temps[3] = 30000;
    CHECK(muyu_thermal_max_range(temps, 16, 1, 3) == 40000);  /* id 3 is included, and it is lower */
    temps[5] = 90000;
    CHECK(muyu_thermal_max_range(temps, 16, 1, 3) == 40000);  /* 5 is outside the range */
    CHECK(muyu_thermal_max_range(temps, 16, 3, 1) == MUYU_THERM_NONE);  /* reversed range */
    CHECK(muyu_thermal_max_range(temps, 16, 1, 20) == MUYU_THERM_NONE); /* past the end */
}

static void test_summary_and_row(void) {
    struct muyu_thermal_snapshot s;
    muyu_thermal_block_init(&s);
    CHECK(s.hot_mc == MUYU_THERM_NONE);
    CHECK(s.enabled_mask == 0);

    /* tsens0: the mid/big CPUs sit at 3..10 and 11..13; id 1/2 are the cluster-level sensors. */
    s.temps[0][3] = 61000;
    /* tsens1: cpu-0 (little) at 1..3, GPU at 8..11, DDR at 13. */
    s.temps[1][2] = 30000;
    s.temps[1][8] = 55000;
    s.temps[1][11] = 50000;
    s.temps[1][13] = 49000;
    /* tsens2 is modem only, but it still counts towards "hottest sensor". */
    s.temps[2][4] = 70000;

    s.enabled_mask = 0x7;   /* en=111 */
    muyu_thermal_summarise(&s);

    CHECK(s.little_mc == 30000);
    CHECK(s.big_mc == 61000);
    CHECK(s.gpu_mc == 55000);
    CHECK(s.ddr_mc == 49000);
    CHECK(s.hot_mc == 70000);

    char row[112];
    muyu_thermal_format_row(row, sizeof row, &s);
    CHECK(strcmp(row, "L=30 B=61 G=55 D=49 hot=70 en=111 inv=0") == 0);

    /* An empty survey must still format (the panel row is skipped only on an empty string). */
    struct muyu_thermal_snapshot empty;
    muyu_thermal_block_init(&empty);
    muyu_thermal_summarise(&empty);
    muyu_thermal_format_row(row, sizeof row, &empty);
    CHECK(strcmp(row, "L=? B=? G=? D=? hot=? en=000 inv=0") == 0);
}

static void test_sensor_map(void) {
    /* The map in the header has to agree with fw/muyu_dt.txt's thermal-zones; check the shape
     * that matters here - which controller carries CPU, GPU and DDR, and the sensor counts. */
    CHECK(kMuyuTsensBlocks[0].tm_base == 0x0c228000ull);
    CHECK(kMuyuTsensBlocks[0].srot_base == 0x0c222000ull);
    CHECK(kMuyuTsensBlocks[0].sensors == 14);
    CHECK(kMuyuTsensBlocks[0].cpu_first == 1 && kMuyuTsensBlocks[0].cpu_last == 13);
    CHECK(kMuyuTsensBlocks[0].gpu_first == 0 && kMuyuTsensBlocks[0].gpu_last == 0);
    CHECK(kMuyuTsensBlocks[1].tm_base == 0x0c229000ull);
    CHECK(kMuyuTsensBlocks[1].srot_base == 0x0c223000ull);
    CHECK(kMuyuTsensBlocks[1].sensors == 16);
    CHECK(kMuyuTsensBlocks[1].cpu_first == 1 && kMuyuTsensBlocks[1].cpu_last == 3);
    CHECK(kMuyuTsensBlocks[1].gpu_first == 8 && kMuyuTsensBlocks[1].gpu_last == 11);
    CHECK(kMuyuTsensBlocks[1].ddr_id == 13);
    CHECK(kMuyuTsensBlocks[2].tm_base == 0x0c22a000ull);
    CHECK(kMuyuTsensBlocks[2].sensors == 5);
}

static void test_trend(void) {
    struct muyu_thermal_trend t;
    muyu_thermal_trend_init(&t);

    char row[112];
    muyu_thermal_format_trend(row, sizeof row, &t);
    CHECK(strcmp(row, "pk=?") == 0);

    muyu_thermal_trend_note(&t, 0, 29000);
    muyu_thermal_trend_note(&t, 15, 33000);
    muyu_thermal_trend_note(&t, 30, MUYU_THERM_NONE);   /* a bad sample must not become 0 C */
    muyu_thermal_trend_note(&t, 60, 37000);
    muyu_thermal_format_trend(row, sizeof row, &t);
    CHECK(strcmp(row, "h0=29 h15=33 h30=? h60=37 pk=37") == 0);

    /* The peak keeps the hottest sample even after a cooler one. */
    muyu_thermal_trend_note(&t, 120, 31000);
    muyu_thermal_format_trend(row, sizeof row, &t);
    CHECK(strcmp(row, "h0=29 h15=33 h30=? h60=37 h120=31 pk=37") == 0);

    /* More marks than the array holds must not write past it. */
    for (unsigned i = 0; i < 10; ++i) muyu_thermal_trend_note(&t, 200 + i, 40000);
    CHECK(t.count == MUYU_THERM_MARKS);
    CHECK(t.peak_mc == 40000);
}

int main(void) {
    test_decode();
    test_degrees();
    test_ranges();
    test_summary_and_row();
    test_sensor_map();
    test_trend();
    printf("PASS: muyu thermal decode, summary and row formatting\n");
    return 0;
}
