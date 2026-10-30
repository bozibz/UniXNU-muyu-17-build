/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests for muyu's generated GMU OPP/BCM policy (ShengGmuVotesSource.h).
 *
 * These are software fixtures only: the rail/BCM aux bytes are shaped like the real
 * AOP command-db records but carry no board addresses, and nothing here touches
 * hardware. Their job is to prove that the *generated* table is the one the device DT
 * describes (counts, order, ACD packing) and that the shared ShengGmuVotes.c code
 * accepts it through every GMxC/ACD/IFPC profile.
 *
 * scripts/check-muyu-gmu-votes.py compiles this with the real AdrenoHfiVotes.c and
 * runs it, and separately re-derives the OPP values from fw/stock/dtb_3.dtb. */
#include "ShengGmuVotes.h"
#include "ShengGmuVotesSource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "vote line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* 2-byte ARC/EBI level tables: off at entry 0, strictly ascending active entries. */
/* 16-bit little-endian entries: 0x0000 (off), 0x0038 (56), 0x0040 (64), 0x0080 (128),
 * 0x00c0 (192), 0x00e0 (224), 0x0100 (256), 0x0140 (320), 0x0180 (384), 0x0190 (400),
 * 0x01a0 (416), 0x01b0 (432). */
static const uint8_t kGfx[] = {0x00, 0x00, 0x38, 0x00, 0x40, 0x00, 0x80, 0x00, 0xc0, 0x00,
                               0xe0, 0x00, 0x00, 0x01, 0x40, 0x01, 0x80, 0x01, 0x90, 0x01,
                               0xa0, 0x01, 0xb0, 0x01};
static const uint8_t kMx[] = {0x00, 0x00, 0x40, 0x00, 0x80, 0x00, 0xc0, 0x00, 0x00, 0x01,
                              0xb0, 0x01};
static const uint8_t kGmxc[] = {0x00, 0x00, 0x80, 0x00, 0x80, 0x01};
/* 8-byte BCM aux data: unit (u32), width (u16), vcd (u8), reserved (u8) = 0. */
static const uint8_t kSh0[] = {0xe8, 3, 0, 0, 16, 0, 0, 0};
static const uint8_t kMc0[] = {0xe8, 3, 0, 0, 4, 0, 1, 0};
static const uint8_t kAcv[] = {0, 0, 0, 0, 0, 0, 2, 0};

void test_muyu_votes(AdrenoHfiStartupConfig *full)
{
    /* The generated table must be this board's, not a default. */
    CHECK(SHENG_GMU_VOTES_CHIP_ID == 0x43030b00u);
    CHECK(SHENG_GPU_OPPS == 12u && SHENG_GMU_OPPS == 3u && SHENG_GPU_BANDWIDTHS == 7u);
    CHECK(sheng_gpu_opps[0].hz == 0 && sheng_gpu_opps[0].level == 0);
    CHECK(sheng_gpu_opps[1].hz == 255000000u && sheng_gpu_opps[1].level == 56);
    CHECK(sheng_gpu_opps[3].hz == 500000000u && sheng_gpu_opps[3].level == 128);
    CHECK(sheng_gpu_opps[11].hz == 1100000000u && sheng_gpu_opps[11].level == 432);
    CHECK(sheng_gpu_opps[11].kbps == 16500000u);
    for (unsigned i = 1; i < SHENG_GPU_OPPS; ++i) {
        CHECK(sheng_gpu_opps[i].hz > sheng_gpu_opps[i - 1].hz);
        CHECK(sheng_gpu_opps[i].level > sheng_gpu_opps[i - 1].level);
        CHECK(sheng_gpu_opps[i].acd_present);          /* every level has qcom,acd-level */
        CHECK(sheng_gpu_opps[i].acd != 0);
    }
    CHECK(sheng_gmu_opps[1].hz == 220000000u && sheng_gmu_opps[1].level == 64);
    CHECK(sheng_gmu_opps[2].hz == 550000000u && sheng_gmu_opps[2].level == 128);
    CHECK(sheng_gpu_bandwidths[0] == 0);
    for (unsigned i = 1; i < SHENG_GPU_BANDWIDTHS; ++i)
        CHECK(sheng_gpu_bandwidths[i] > sheng_gpu_bandwidths[i - 1]);
    /* A7xx BCM: proportional SH0/MC0 and a fixed ACV perfmode at the top bandwidth. */
    CHECK(sheng_bcm_policy[0].buswidth == 16 && !sheng_bcm_policy[0].fixed);
    CHECK(sheng_bcm_policy[1].buswidth == 4 && !sheng_bcm_policy[1].fixed);
    CHECK(sheng_bcm_policy[2].fixed && sheng_bcm_policy[2].perfmode == 8);
    CHECK(sheng_bcm_policy[2].perfmode_kbps == sheng_gpu_bandwidths[SHENG_GPU_BANDWIDTHS - 1]);

    ShengGmuVoteInputs in = {.chip_id = SHENG_GMU_VOTES_CHIP_ID,
        .gfx = {kGfx, sizeof kGfx}, .cx = {kGfx, sizeof kGfx}, .mx = {kMx, sizeof kMx},
        .sh0 = {0x1000, {kSh0, sizeof kSh0}}, .mc0 = {0x1004, {kMc0, sizeof kMc0}},
        .acv = {0x1008, {kAcv, sizeof kAcv}}, .cn0_address = 0x2000,
        .enable_acd = 1, .enable_ifpc = 1};
    AdrenoHfiStartupConfig c;
    for (unsigned present = 0; present < 2; ++present)
        for (unsigned acd = 0; acd < 2; ++acd)
            for (unsigned ifpc = 0; ifpc < 2; ++ifpc) {
                in.gmxc_present = present;
                in.gmxc = present ? (ShengGmuAux){kGmxc, sizeof kGmxc} : (ShengGmuAux){0};
                in.enable_acd = acd;
                in.enable_ifpc = ifpc;
                CHECK(ShengGmuBuildStartup(&in, &c) == AdrenoHfiStartupSuccess);
                CHECK(c.gpu_count == SHENG_GPU_OPPS && c.gmu_count == SHENG_GMU_OPPS);
                CHECK(c.bandwidth_count == SHENG_GPU_BANDWIDTHS && c.ddr_count == 3);
                CHECK(c.enable_ifpc == ifpc);
                unsigned packed = 0, mask = 0;
                for (unsigned i = 0; i < c.gpu_count; ++i) {
                    CHECK(c.gpu[i].hz == sheng_gpu_opps[i].hz);
                    if (acd && sheng_gpu_opps[i].acd_present) {
                        CHECK(c.acd_data[packed++] == sheng_gpu_opps[i].acd);
                        mask |= 1u << i;
                    }
                }
                CHECK(c.acd_count == packed && c.acd_enable_mask == mask);
                if (acd) CHECK(packed == SHENG_GPU_OPPS - 1 && mask == 0xffeu);
            }
    *full = c;   /* last profile: GMxC, ACD and IFPC enabled */

    /* All-or-nothing refusals, with the generated policy in place. */
    ShengGmuVoteInputs good = in;
    for (unsigned control = 0; control < 6; ++control) {
        in = good;
        switch (control) {
        case 0: in.chip_id = 0; break;
        case 1: in.gfx.bytes = 0; break;
        case 2: in.gmxc_present = 2; break;
        case 3: in.sh0.aux.bytes = 7; break;
        case 4: in.mc0.address = in.sh0.address; break;
        case 5: in.cn0_address = 0; break;
        }
        memset(&c, 0xa5, sizeof c);
        AdrenoHfiStartupConfig before;
        memcpy(&before, &c, sizeof c);
        CHECK(ShengGmuBuildStartup(&in, &c) == AdrenoHfiStartupInvalid &&
              !memcmp(&before, &c, sizeof c));
    }
    puts("PASS: muyu's generated OPP/BCM policy (12 GPU / 3 GMU / 7 bandwidths, ACD on 11 levels), "
         "8 profiles and 6 all-or-nothing refusals; software fixtures only");
}

#ifdef MUYU_VOTES_STANDALONE
int main(void)
{
    AdrenoHfiStartupConfig c;
    test_muyu_votes(&c);
    return 0;
}
#endif
