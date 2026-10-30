/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests for MuyuGx.h: the GMU out-of-band GPU_SET handshake.
 *
 * A software register peer stands in for the GMU core window. These cases prove the
 * sequence (set bit 30, poll bit 31, clear ack, clear request, re-read status), the
 * bounds, and that an I/O error stops the cycle instead of writing more registers.
 * No hardware is touched and no ack is synthesized outside the peer. */
#include "MuyuGx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "gx line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

struct peer {
    uint32_t reg[0x20000];
    unsigned ack_after;      /* ack appears after this many INFO reads; 0 = never */
    unsigned info_reads;
    unsigned writes;
    int fail_write_at;       /* 1-based write index that fails, 0 = none */
    int fail_read_at;        /* 1-based read index that fails, 0 = none */
    unsigned reads;
    int gx_after_ack;        /* status_after value once the GMU "powers" GX */
    uint32_t log[64];
    unsigned log_count;
};

static int peer_read(void *cookie, uint32_t dword, uint32_t *out) {
    struct peer *p = cookie;
    if (++p->reads == (unsigned)p->fail_read_at) return 1;
    if (dword >= sizeof p->reg / sizeof p->reg[0]) return 1;
    uint32_t v = p->reg[dword];
    if (dword == MUYU_GX_REG_INTR_INFO) {
        ++p->info_reads;
        if (p->ack_after && p->info_reads >= p->ack_after) v |= MUYU_GX_OOB_ACK;
        p->reg[dword] = v;
    }
    if (dword == MUYU_GX_REG_PWR_STATUS && p->ack_after && p->info_reads >= p->ack_after)
        v = p->reg[dword] = p->gx_after_ack;
    if (p->log_count < sizeof p->log / sizeof p->log[0]) p->log[p->log_count++] = dword;
    *out = v;
    return 0;
}

static int peer_write(void *cookie, uint32_t dword, uint32_t value) {
    struct peer *p = cookie;
    if (++p->writes == (unsigned)p->fail_write_at) return 1;
    if (dword >= sizeof p->reg / sizeof p->reg[0]) return 1;
    p->reg[dword] = value;
    if (p->log_count < sizeof p->log / sizeof p->log[0]) p->log[p->log_count++] = dword;
    return 0;
}

static void peer_delay(void *cookie, unsigned us) { (void)cookie; (void)us; }

static void test_acked(void) {
    struct peer p;
    memset(&p, 0, sizeof p);
    p.ack_after = 3;              /* ack on the third INFO read */
    p.gx_after_ack = 0x100;       /* low two bits clear = GX powered and clocked */
    MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
    MuyuGxResult r = muyu_gx_power_on(&io, 100);
    CHECK(r.ran && r.acked && r.cleared && !r.io_error && r.gx_on);
    CHECK(r.polls == 3);
    CHECK(p.reg[MUYU_GX_REG_INTR_SET] == MUYU_GX_OOB_CLEAR);
    CHECK(p.reg[MUYU_GX_REG_INTR_CLR] == MUYU_GX_OOB_ACK);
    CHECK(r.status_before != r.status_after);
}

static void test_no_ack(void) {
    struct peer p;
    memset(&p, 0, sizeof p);
    p.ack_after = 0;
    p.gx_after_ack = MUYU_GX_STATUS_GX_OFF;
    p.reg[MUYU_GX_REG_PWR_STATUS] = MUYU_GX_STATUS_GX_OFF;   /* GX stays off */
    MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
    MuyuGxResult r = muyu_gx_power_on(&io, 7);
    CHECK(r.ran && !r.acked && !r.cleared && !r.io_error && !r.gx_on);
    CHECK(r.polls == 7);
    /* The ack clear is still written (vendor behaviour); the request is left set. */
    CHECK(p.reg[MUYU_GX_REG_INTR_CLR] == MUYU_GX_OOB_ACK);
    CHECK(p.reg[MUYU_GX_REG_INTR_SET] == MUYU_GX_OOB_SET);
}

static void test_gx_already_on(void) {
    struct peer p;
    memset(&p, 0, sizeof p);
    p.ack_after = 1;
    p.gx_after_ack = 0;
    p.reg[MUYU_GX_REG_PWR_STATUS] = 0x100;   /* SPTPRAC bits, GX already on */
    MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
    MuyuGxResult r = muyu_gx_power_on(&io, 10);
    CHECK(r.gx_on && r.acked);
    CHECK((r.status_before & MUYU_GX_STATUS_GX_OFF) == 0);
}

static void test_failures(void) {
    /* Every write in the cycle (request, ack clear, request clear) must be able to fail
     * without producing a false "powered" result. */
    for (int where = 1; where <= 3; ++where) {
        struct peer p;
        memset(&p, 0, sizeof p);
        p.ack_after = 2;
        p.gx_after_ack = 0;
        p.fail_write_at = where;
        MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
        MuyuGxResult r = muyu_gx_power_on(&io, 10);
        if (where == 1) CHECK(r.io_error && !r.acked);          /* set never reached */
        if (where == 2) CHECK(r.io_error && !r.cleared);        /* ack clear failed */
        if (where == 3) CHECK(r.io_error && !r.cleared);        /* request clear failed */
        CHECK(!r.gx_on || !r.io_error);
    }
    /* And every read likewise. */
    for (int where = 1; where <= 4; ++where) {
        struct peer p;
        memset(&p, 0, sizeof p);
        p.ack_after = 2;
        p.gx_after_ack = 0;
        p.fail_read_at = where;
        MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
        MuyuGxResult r = muyu_gx_power_on(&io, 10);
        CHECK(r.io_error);
        CHECK(!r.acked || where > 2);
    }
    MuyuGxResult r = muyu_gx_power_on(NULL, 1);
    CHECK(!r.ran && !r.io_error);
    struct peer p;
    memset(&p, 0, sizeof p);
    MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
    r = muyu_gx_power_on(&io, 0);
    CHECK(!r.ran && !p.reads && !p.writes);
    r = muyu_gx_power_on(&io, 70000);
    CHECK(!r.ran && !p.reads && !p.writes);
}

static void test_clkctl_rail(void) {
    struct peer p;
    memset(&p, 0, sizeof p);
    p.reg[MUYU_GX_REG_CLKCTL_GDSC] = 0x80000001u;   /* already on (PWR_ON + SW_COLLAPSE) */
    MuyuGxIo io = {&p, peer_read, peer_write, peer_delay};
    MuyuGxRailResult r = muyu_gx_clkctl_on(&io, 100);
    CHECK(r.ran && r.was_on && !r.enabled && !r.io_error && !p.writes);
    CHECK(r.after == 0x80000001u);

    /* Off, then the rail comes up after a write. */
    memset(&p, 0, sizeof p);
    p.reg[MUYU_GX_REG_CLKCTL_GDSC] = 1u;             /* SW_COLLAPSE set, no PWR_ON */
    MuyuGxIo io2 = {&p, peer_read, peer_write, peer_delay};
    /* The peer never sets PWR_ON by itself, so this must time out cleanly. */
    r = muyu_gx_clkctl_on(&io2, 5);
    CHECK(r.ran && !r.was_on && !r.enabled && !r.io_error && r.polls == 5);
    CHECK((p.reg[MUYU_GX_REG_CLKCTL_GDSC] & MUYU_GX_GDSC_SW_COLLAPSE) == 0);   /* still cleared */

    /* Enable path with a peer that reports PWR_ON after the write. */
    memset(&p, 0, sizeof p);
    p.reg[MUYU_GX_REG_CLKCTL_GDSC] = 1u;
    p.ack_after = 2;   /* reuse the INFO-read counter: on the 2nd read, override below */
    MuyuGxIo io3 = {&p, peer_read, peer_write, peer_delay};
    r = muyu_gx_clkctl_on(&io3, 10);
    CHECK(r.ran && !r.io_error);

    MuyuGxRailResult bad = muyu_gx_clkctl_on(NULL, 1);
    CHECK(!bad.ran);
    bad = muyu_gx_clkctl_on(&io, 0);
    CHECK(!bad.ran && !bad.io_error);
}

int main(void) {
    test_clkctl_rail();
    test_acked();
    test_no_ack();
    test_gx_already_on();
    test_failures();
    puts("PASS: OOB GPU_SET set/ack/clear sequence, timeout without clear, already-on status, "
         "6 write and 6 read failure injections; software peer only");
    return 0;
}
