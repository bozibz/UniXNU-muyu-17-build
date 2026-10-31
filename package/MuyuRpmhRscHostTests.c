// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the CPU-side RPMh ARC voter (software register peer, not hardware).
#include "MuyuRpmhRsc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

#define TCS_BASE MUYU_RSC_TCS_BASE_OFFSET
#define TCS(i) (TCS_BASE + MUYU_RSC_TCS_OFFSET * (i))
#define MSGID (MUYU_RSC_CMD_MSGID_LEN | MUYU_RSC_CMD_MSGID_WRITE | MUYU_RSC_CMD_MSGID_RESP_REQ)
#define RSC_DATA_BASE 144u

struct model {
    uint32_t rsc_id, prnt;
    uint32_t irq_enable;                  // tcs_base + 0x00 (IRQ mask)
    struct {
        uint32_t wait, control, status, enable;
        uint32_t msgid[2], addr[2], data[2], cmd_status[2];
    } tcs[3];
    unsigned pending_ack, pending_tcs;
    unsigned no_ack;
    unsigned writable_fail_offset;
    unsigned read_fail_offset;
    unsigned trace_n, delays;
    struct { uint32_t off, val; } trace[64];
};

static void idle_model(struct model *m) {
    memset(m, 0, sizeof *m);
    m->rsc_id = 3u << 16;                 // major 3
    m->prnt = (16u << 27) | (8u << 12);   // ncpt=16, max_tcs=8 for drv2
    m->irq_enable = 0x1ffu;
    for (unsigned i = 0; i < 3; ++i) m->tcs[i].status = 1;
    m->pending_ack = 0;
    m->writable_fail_offset = ~0u;
    m->read_fail_offset = ~0u;
}

static int reg_of(uint32_t off) {
    return (int)((off - TCS_BASE) % MUYU_RSC_TCS_OFFSET);
}

static int tcs_of(uint32_t off) {
    return (int)((off - TCS_BASE) / MUYU_RSC_TCS_OFFSET);
}

static int rd(void *c, unsigned window, uint32_t off, uint32_t *out) {
    struct model *m = c;
    if (window != MUYU_RSC_WINDOW) return -1;
    if (off == m->read_fail_offset) return -1;
    if (off == 0) { *out = m->rsc_id; return 0; }
    if (off == MUYU_RSC_DRV_PRNT_CHLD_CONFIG) { *out = m->prnt; return 0; }
    if (off < TCS_BASE) return -1;
    unsigned i = (unsigned)tcs_of(off);
    if (i >= 3) return -1;
    int r = reg_of(off);
    if (r < 0x10) {
        if (r == 0x00) { *out = m->irq_enable; return 0; }
        if (r == 0x04) { *out = 0; return 0; }     // irq status, never used here
        if (r == 0x08) { *out = 0; return 0; }     // irq clear, write-only
        return -1;
    }
    switch (r) {
    case MUYU_RSC_CMD_WAIT_FOR_CMPL: *out = m->tcs[i].wait; return 0;
    case MUYU_RSC_CONTROL: *out = m->tcs[i].control; return 0;
    case MUYU_RSC_STATUS: *out = m->tcs[i].status; return 0;
    case MUYU_RSC_CMD_ENABLE: *out = m->tcs[i].enable; return 0;
    }
    if (r >= (int)MUYU_RSC_CMD_MSGID && r < (int)(MUYU_RSC_CMD_MSGID + 4 * MUYU_RSC_CMD_OFFSET)) {
        unsigned slot = (unsigned)(r - MUYU_RSC_CMD_MSGID) / MUYU_RSC_CMD_OFFSET;
        unsigned field = (unsigned)(r - MUYU_RSC_CMD_MSGID) % MUYU_RSC_CMD_OFFSET;
        if (slot > 1 || field % 4) return -1;
        switch (field) {
        case 0: *out = m->tcs[i].msgid[slot]; return 0;
        case 4: *out = m->tcs[i].addr[slot]; return 0;
        case 8: *out = m->tcs[i].data[slot]; return 0;
        case 12: *out = m->tcs[i].cmd_status[slot]; return 0;
        }
    }
    return -1;
}

static int wr(void *c, unsigned window, uint32_t off, uint32_t val) {
    struct model *m = c;
    if (window != MUYU_RSC_WINDOW) return -1;
    if (m->trace_n < sizeof(m->trace) / sizeof(m->trace[0])) {
        m->trace[m->trace_n].off = off;
        m->trace[m->trace_n].val = val;
    }
    ++m->trace_n;
    if (off == m->writable_fail_offset) return -1;
    if (off == 0) { m->rsc_id = val; return 0; }
    if (off == MUYU_RSC_DRV_PRNT_CHLD_CONFIG) { m->prnt = val; return 0; }
    if (off < TCS_BASE) return -1;
    unsigned i = (unsigned)tcs_of(off);
    if (i >= 3) return -1;
    int r = reg_of(off);
    if (r < 0x10) {
        if (r == 0x00) { m->irq_enable = val; return 0; }
        if (r == 0x08) return 0;                   // irq clear, write-only
        return -1;
    }
    switch (r) {
    case MUYU_RSC_CMD_WAIT_FOR_CMPL: m->tcs[i].wait = val; return 0;
    case MUYU_RSC_CONTROL:
        m->tcs[i].control = val;
        if ((val & MUYU_RSC_CONTROL_TRIGGER) && (val & MUYU_RSC_CONTROL_ENABLE)) {
            m->pending_ack = 1; m->pending_tcs = i;
            m->tcs[i].status = 0;                  // busy until the delay ack
        }
        return 0;
    case MUYU_RSC_STATUS: m->tcs[i].status = val; return 0;
    case MUYU_RSC_CMD_ENABLE: m->tcs[i].enable = val; return 0;
    }
    if (r >= (int)MUYU_RSC_CMD_MSGID && r < (int)(MUYU_RSC_CMD_MSGID + 4 * MUYU_RSC_CMD_OFFSET)) {
        unsigned slot = (unsigned)(r - MUYU_RSC_CMD_MSGID) / MUYU_RSC_CMD_OFFSET;
        unsigned field = (unsigned)(r - MUYU_RSC_CMD_MSGID) % MUYU_RSC_CMD_OFFSET;
        if (slot > 1 || field % 4) return -1;
        switch (field) {
        case 0: m->tcs[i].msgid[slot] = val; return 0;
        case 4: m->tcs[i].addr[slot] = val; return 0;
        case 8: m->tcs[i].data[slot] = val; return 0;
        case 12: m->tcs[i].cmd_status[slot] = val; return 0;
        }
    }
    return -1;
}

static void delay_us(void *c, unsigned us) {
    struct model *m = c;
    ++m->delays;
    (void)us;
    if (m->pending_ack && !m->no_ack) {
        m->tcs[m->pending_tcs].cmd_status[0] = MUYU_RSC_CMD_STATUS_ACK;
        m->tcs[m->pending_tcs].cmd_status[1] = MUYU_RSC_CMD_STATUS_ACK;
        m->tcs[m->pending_tcs].status = 1;
        m->pending_ack = 0;
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

static void id8(uint8_t *p, const char *id) {
    for (unsigned i = 0; i < 8; ++i) p[i] = id[i] ? (uint8_t)id[i] : 0;
}

static const uint16_t GFX_LEVELS[12] = {0, 56, 64, 128, 192, 224, 256, 320, 384, 400, 416, 432};
static const uint16_t MXC_LEVELS[6] = {0, 64, 128, 192, 256, 432};

static void build_db(uint8_t *db, size_t bytes) {
    memset(db, 0, bytes);
    put32(db, 1u);                          // cmd-db version
    put32(db + 4, MUYU_RSC_CMD_DB_MAGIC);
    // Slave row 0 (data+8): ARC slave 3, 2 entries, headers at 144, aux at 208.
    put16(db + 8, MUYU_RSC_ARC_SLAVE);
    put16(db + 10, 0);                      // header offset from data start
    put16(db + 12, 64);                     // data offset from data start
    put16(db + 14, 2);                      // count
    // Entries at data+144.
    id8(db + RSC_DATA_BASE, "gfx.lvl");
    put32(db + RSC_DATA_BASE + 16, 0x00030000u);
    put16(db + RSC_DATA_BASE + 20, (uint16_t)(sizeof(GFX_LEVELS)));
    put16(db + RSC_DATA_BASE + 22, 0);
    id8(db + RSC_DATA_BASE + 24, "mxc.lvl");
    put32(db + RSC_DATA_BASE + 24 + 16, 0x00030004u);
    put16(db + RSC_DATA_BASE + 24 + 20, (uint16_t)(sizeof(MXC_LEVELS)));
    put16(db + RSC_DATA_BASE + 24 + 22, 24);
    // Aux at data+144+64.
    uint8_t *aux = db + RSC_DATA_BASE + 64;
    for (unsigned i = 0; i < sizeof(GFX_LEVELS) / 2; ++i) put16(aux + 2 * i, GFX_LEVELS[i]);
    for (unsigned i = 0; i < sizeof(MXC_LEVELS) / 2; ++i) put16(aux + 24 + 2 * i, MXC_LEVELS[i]);
}

static void test_parse_good(void) {
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    build_db(db, sizeof db);
    MuyuRpmhRscStatus st = MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs);
    CHECK(st == MuyuRpmhRscSuccess);
    CHECK(arcs.both_present == 1);
    CHECK(strcmp(arcs.gfx.name, MUYU_RSC_GFX_NAME) == 0);
    CHECK(strcmp(arcs.mxc.name, MUYU_RSC_MXC_NAME) == 0);
    CHECK(arcs.gfx.address == 0x00030000u && arcs.mxc.address == 0x00030004u);
    CHECK(arcs.gfx.count == 12 && arcs.mxc.count == 6);
    CHECK(arcs.gfx.selector == 1 && arcs.mxc.selector == 1);   // first level >= init
}

static void test_parse_corrupt(void) {
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    build_db(db, sizeof db);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 2u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbVersion);
    db[4] ^= 1;
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    build_db(db, sizeof db);
    id8(db + RSC_DATA_BASE + 24, "xxx.lvl");
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbMissing);
    build_db(db, sizeof db);
    memcpy(db + RSC_DATA_BASE + 24, db + RSC_DATA_BASE, 8);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    build_db(db, sizeof db);
    put32(db + RSC_DATA_BASE + 16, 0);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    build_db(db, sizeof db);
    put32(db + RSC_DATA_BASE + 16, 0x00050000u);   // wrong ACCL_TYPE slave
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    build_db(db, sizeof db);
    put16(db + RSC_DATA_BASE + 20, 13);            // odd aux length
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    build_db(db, sizeof db);
    put16(db + 12, 0);                             // make aux overlap the entry region
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    CHECK(MuyuRpmhRscArcsParse(db, 100, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscDbCorrupt);
    CHECK(MuyuRpmhRscArcsParse(NULL, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscArgument);
}

static void test_parse_level_above_table(void) {
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    build_db(db, sizeof db);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x1b1u, 0x30u, &arcs) == MuyuRpmhRscLevel);
}

static void test_vote_success(void) {
    struct model m;
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m); build_db(db, sizeof db);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscSuccess);
    MuyuRpmhRscStatus st = MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r);
    CHECK(st == MuyuRpmhRscSuccess);
    CHECK(r.tcs == 0);
    CHECK(r.rsc_major == 3 && r.max_tcs == 8 && r.ncpt == 16);
    CHECK(r.irq_enable_before == 0x1ffu && r.irq_enable_after == 0x1ffu && r.irq_clear == 1u);
    CHECK(r.wait_for_compl == 3u && r.cmd_enable == 3u);
    CHECK(r.before_status != 0 && r.after_status != 0);
    CHECK(r.cmd_status[0] == MUYU_RSC_CMD_STATUS_ACK && r.cmd_status[1] == MUYU_RSC_CMD_STATUS_ACK);
    CHECK(r.status_polls == 1 && r.command_polls >= 1 && m.delays >= 1);

    struct { uint32_t off, val; } const want[] = {
        {TCS(0) + 0x2c, 0},                 // sync-clear ENABLE
        {TCS(0) + 0x20, 0},                 // sync-clear WAIT
        {TCS_BASE + 0x00, 0x1feu},          // mask this TCS's IRQ while polling
        {TCS(0) + 0x34, MSGID},             // slot 0: gfx.lvl
        {TCS(0) + 0x38, 0x00030000u},
        {TCS(0) + 0x3c, 1u},
        {TCS(0) + 0x4c, MSGID},             // slot 1: mxc.lvl
        {TCS(0) + 0x50, 0x00030004u},
        {TCS(0) + 0x54, 1u},
        {TCS(0) + 0x20, 3u},                // both commands wait
        {TCS(0) + 0x2c, 3u},                // both commands enabled
        {TCS(0) + 0x24, 0u},                // clear trigger
        {TCS(0) + 0x24, 0u},                // clear enable
        {TCS(0) + 0x24, MUYU_RSC_CONTROL_ENABLE},
        {TCS(0) + 0x24, MUYU_RSC_CONTROL_ENABLE | MUYU_RSC_CONTROL_TRIGGER},
        {TCS(0) + 0x2c, 0},                 // cleanup
        {TCS(0) + 0x20, 0},
        {TCS_BASE + 0x08, 1u},
        {TCS_BASE + 0x00, 0x1ffu},
    };
    CHECK(m.trace_n == sizeof(want) / sizeof(want[0]));
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); ++i)
        CHECK(m.trace[i].off == want[i].off && m.trace[i].val == want[i].val);
    CHECK(m.tcs[0].cmd_status[0] == MUYU_RSC_CMD_STATUS_ACK && m.tcs[0].cmd_status[1] == MUYU_RSC_CMD_STATUS_ACK);
}

static void test_vote_second_tcs_when_first_busy(void) {
    struct model m;
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m); m.tcs[0].status = 0;
    build_db(db, sizeof db);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscSuccess);
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscSuccess);
    CHECK(r.tcs == 1);
    CHECK(m.trace_n > 2 && m.trace[0].off == TCS(1) + 0x2c);
}

static void test_vote_all_busy(void) {
    struct model m;
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m); m.tcs[0].status = m.tcs[1].status = m.tcs[2].status = 0;
    build_db(db, sizeof db);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscSuccess);
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscBusy);
    CHECK(r.status_polls >= 100);
    CHECK(m.trace_n == 0);
}

static void test_vote_timeout(void) {
    struct model m;
    uint8_t db[512];
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m); m.no_ack = 1;
    build_db(db, sizeof db);
    CHECK(MuyuRpmhRscArcsParse(db, sizeof db, 1u, 0x10u, 0x30u, &arcs) == MuyuRpmhRscSuccess);
    MuyuRpmhRscStatus st = MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r);
    CHECK(st == MuyuRpmhRscTimeout);
    CHECK(r.command_polls > 10000);      // never acked; the poll deadline is 1s of 10us steps
}

static void test_vote_version_and_config(void) {
    struct model m;
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m); m.rsc_id = 2u << 16;
    arcs.both_present = 1;
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscVersion);
    idle_model(&m); m.prnt = 0;
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscConfig);
}

static void test_vote_map_and_argument(void) {
    struct model m;
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m);
    arcs.both_present = 1;
    CHECK(MuyuRpmhRscVoteGx(&io, 0, &arcs, &r) == MuyuRpmhRscMap);
    arcs.both_present = 0;
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscArgument);
    CHECK(MuyuRpmhRscVoteGx(0, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscArgument);
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, 0) == MuyuRpmhRscArgument);
}

static void test_vote_io_errors(void) {
    struct model m;
    MuyuRpmhRscArcs arcs;
    MuyuRpmhRscResult r;
    MuyuRpmhRscIo io = { &m, rd, wr, delay_us };
    idle_model(&m); m.read_fail_offset = 0;
    arcs.both_present = 1;
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscIoError);
    idle_model(&m); m.writable_fail_offset = TCS_BASE + 0x08;   // cleanup irq_clear
    CHECK(MuyuRpmhRscVoteGx(&io, 1u << MUYU_RSC_WINDOW, &arcs, &r) == MuyuRpmhRscIoError);
    CHECK(r.cleanup_io_error == 1);
}

int main(void) {
    test_parse_good();
    test_parse_corrupt();
    test_parse_level_above_table();
    test_vote_success();
    test_vote_second_tcs_when_first_busy();
    test_vote_all_busy();
    test_vote_timeout();
    test_vote_version_and_config();
    test_vote_map_and_argument();
    test_vote_io_errors();
    if (failures) {
        printf("MuyuRpmhRsc host tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("MuyuRpmhRsc host tests OK\n");
    return 0;
}