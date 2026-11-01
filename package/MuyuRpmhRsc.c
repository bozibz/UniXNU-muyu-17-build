// SPDX-License-Identifier: BSD-2-Clause-Patent
// CPU-side RPMh ARC voter for muyu's GX rail; see MuyuRpmhRsc.h for the
// pinned DT/source facts and the call order. Every register access goes
// through the MuyuRpmhRscIo peer so host tests get a software register model
// and the kernel gets the IOMemoryDescriptor map.
#include "MuyuRpmhRsc.h"

#include <string.h>

static int io_read(const MuyuRpmhRscIo *io, unsigned window, uint32_t offset, uint32_t *out) {
    if (!io || !io->read32) return 1;
    return io->read32(io->cookie, window, offset, out);
}

static int io_write(const MuyuRpmhRscIo *io, unsigned window, uint32_t offset, uint32_t value) {
    if (!io || !io->write32) return 1;
    return io->write32(io->cookie, window, offset, value);
}

static void io_delay(const MuyuRpmhRscIo *io, unsigned us) {
    if (io && io->delay_us) io->delay_us(io->cookie, us);
}

const char *MuyuRpmhRscStatusText(MuyuRpmhRscStatus status) {
    switch (status) {
    case MuyuRpmhRscSuccess: return "success";
    case MuyuRpmhRscArgument: return "argument";
    case MuyuRpmhRscDbVersion: return "cmd-db version";
    case MuyuRpmhRscDbCorrupt: return "cmd-db corrupt";
    case MuyuRpmhRscDbMissing: return "cmd-db resource missing";
    case MuyuRpmhRscLevel: return "level table empty";
    case MuyuRpmhRscMap: return "rsc window unmapped";
    case MuyuRpmhRscConfig: return "rsc config invalid";
    case MuyuRpmhRscVersion: return "rsc major < 3";
    case MuyuRpmhRscBusy: return "no free ACTIVE TCS";
    case MuyuRpmhRscTimeout: return "command completion timeout";
    case MuyuRpmhRscIoError: return "mmio peer failed";
    }
    return "unknown";
}

// --- command-db walk (mirrors ShengGmuCommandDb.c / cmd-db.c) ---------------
//
// Header = version u32 + magic bytes, then 8x16-byte slave rows at data+8.
// Entries live at data + 144 + header_offset, 24 bytes each. The aux blob is
// at data + 144 + data_offset + entry.offset.

static uint16_t load_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t load_u32(const uint8_t *p) {
    return (uint32_t)load_u16(p) | ((uint32_t)load_u16(p + 2) << 16);
}

static int in_range(size_t at, size_t len, size_t size) {
    return at <= size && len <= size - at;
}

static int overlaps(size_t a, size_t n, size_t b, size_t m) {
    return n != 0 && m != 0 && a < b + m && b < a + n;
}

static int id_equal(const uint8_t *entry, const char *id) {
    for (unsigned i = 0; i < 8; ++i)
        if (entry[i] != (uint8_t)id[i]) return 0;
    return 1;
}

static MuyuRpmhRscStatus arc_from_entry(const uint8_t *entry, const uint8_t *aux,
                                        size_t aux_off, unsigned slave, size_t cmd_db_bytes,
                                        MuyuRpmhRscArc *arc) {
    uint32_t address = load_u32(entry + 16);
    size_t len = (size_t)load_u16(entry + 20);
    size_t blob_at = (size_t)load_u16(entry + 22);
    size_t offset = aux_off + blob_at;

    if (!address || (address & 3u) != 0) return MuyuRpmhRscDbCorrupt;
    if (((address >> 16) & 7u) != slave) return MuyuRpmhRscDbCorrupt;
    if (slave != MUYU_RSC_ARC_SLAVE) return MuyuRpmhRscDbCorrupt;
    if (len < 2u || (len & 1u) != 0u || len > MUYU_RSC_CMD_DB_MAX_LEVELS * 2u)
        return MuyuRpmhRscDbCorrupt;
    if (!in_range(offset, len, cmd_db_bytes)) return MuyuRpmhRscDbCorrupt;

    arc->address = address;
    arc->count = 0;
    for (unsigned i = 0; i < len / 2u; ++i) {
        uint16_t lv = load_u16(aux + blob_at + 2u * i);
        if (i != 0 && lv == 0) break;      // trailing zeros pad the 16-entry table
        if (arc->count >= MUYU_RSC_CMD_DB_MAX_LEVELS) return MuyuRpmhRscDbCorrupt;
        arc->levels[arc->count++] = lv;
    }
    if (arc->count == 0) return MuyuRpmhRscDbCorrupt;
    return MuyuRpmhRscSuccess;
}

MuyuRpmhRscStatus MuyuRpmhRscArcsParse(const uint8_t *cmd_db, size_t cmd_db_bytes,
                                       uint32_t cmd_db_version,
                                       uint32_t gfx_init_level, uint32_t mxc_init_level,
                                       MuyuRpmhRscArcs *out) {
    if (!cmd_db || !out) return MuyuRpmhRscArgument;
    memset(out, 0, sizeof *out);
    if (cmd_db_bytes < 144u + 8u * 16u) return MuyuRpmhRscDbCorrupt;
    if (load_u32(cmd_db) != cmd_db_version) return MuyuRpmhRscDbVersion;
    if (load_u32(cmd_db + 4) != MUYU_RSC_CMD_DB_MAGIC) return MuyuRpmhRscDbCorrupt;

    size_t heads[8] = {0}, sizes[8] = {0}, aux[8] = {0};
    unsigned rows = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *r = cmd_db + 8 + 16u * i;
        unsigned slave = load_u16(r);
        unsigned count = load_u16(r + 6);
        if (!slave) break;
        heads[i] = 144u + (size_t)load_u16(r + 2);
        sizes[i] = (size_t)count * 24u;
        aux[i] = 144u + (size_t)load_u16(r + 4);
        if (!in_range(heads[i], sizes[i], cmd_db_bytes)) return MuyuRpmhRscDbCorrupt;
        if (!in_range(aux[i], 0, cmd_db_bytes)) return MuyuRpmhRscDbCorrupt;
        ++rows;
    }
    if (!rows) return MuyuRpmhRscDbMissing;

    static const char gfx_id[8] = {'g', 'f', 'x', '.', 'l', 'v', 'l', 0};
    static const char mxc_id[8] = {'m', 'x', 'c', '.', 'l', 'v', 'l', 0};
    unsigned seen = 0;
    MuyuRpmhRscArc tmp_gfx, tmp_mxc;
    memset(&tmp_gfx, 0, sizeof tmp_gfx);
    memset(&tmp_mxc, 0, sizeof tmp_mxc);

    for (unsigned i = 0; i < rows; ++i) {
        unsigned slave = load_u16(cmd_db + 8 + 16u * i);
        for (size_t at = heads[i]; at < heads[i] + sizes[i]; at += 24u) {
            const uint8_t *e = cmd_db + at;
            if (e + 24u > cmd_db + cmd_db_bytes) return MuyuRpmhRscDbCorrupt;
            MuyuRpmhRscArc *target = 0;
            unsigned bit = 0;
            const char *id = 0;
            if (id_equal(e, gfx_id)) { target = &tmp_gfx; bit = 1u; id = gfx_id; }
            else if (id_equal(e, mxc_id)) { target = &tmp_mxc; bit = 2u; id = mxc_id; }
            if (!target) continue;
            if (seen & bit) return MuyuRpmhRscDbCorrupt;   // duplicate
            MuyuRpmhRscStatus st = arc_from_entry(e, cmd_db + aux[i], aux[i],
                                                  slave, cmd_db_bytes, target);
            if (st != MuyuRpmhRscSuccess) return st;
            // The aux blob must not land inside any slave's entry region.
            size_t offset = aux[i] + load_u16(e + 22);
            size_t len = load_u16(e + 20);
            for (unsigned j = 0; j < rows; ++j)
                if (overlaps(offset, len, heads[j], sizes[j])) return MuyuRpmhRscDbCorrupt;
            memcpy(target->name, id, 8);
            seen |= bit;
        }
    }

    if ((seen & 3u) != 3u) return MuyuRpmhRscDbMissing;

    unsigned gfx_selector = 0xffffu, mxc_selector = 0xffffu;
    for (unsigned i = 0; i < tmp_gfx.count; ++i)
        if (gfx_init_level <= tmp_gfx.levels[i]) { gfx_selector = i; break; }
    for (unsigned i = 0; i < tmp_mxc.count; ++i)
        if (mxc_init_level <= tmp_mxc.levels[i]) { mxc_selector = i; break; }
    if (gfx_selector == 0xffffu || mxc_selector == 0xffffu) return MuyuRpmhRscLevel;
    tmp_gfx.selector = gfx_selector;
    tmp_mxc.selector = mxc_selector;
    out->gfx = tmp_gfx;
    out->mxc = tmp_mxc;
    out->both_present = 1u;
    return MuyuRpmhRscSuccess;
}

unsigned MuyuRpmhRscListArcs(const uint8_t *cmd_db, size_t bytes, uint32_t version,
                             MuyuRpmhRscEntry *out, unsigned max) {
    unsigned written = 0;
    if (!cmd_db || !out || !max) return 0;
    if (bytes < 144u + 8u * 16u) return 0;
    if (load_u32(cmd_db) != version) return 0;
    if (load_u32(cmd_db + 4) != MUYU_RSC_CMD_DB_MAGIC) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *r = cmd_db + 8 + 16u * i;
        unsigned slave = load_u16(r);
        if (!slave) break;
        if (slave != MUYU_RSC_ARC_SLAVE) continue;
        size_t head = 144u + (size_t)load_u16(r + 2);
        size_t size = (size_t)load_u16(r + 6) * 24u;
        if (!in_range(head, size, bytes)) return written;
        for (size_t at = head; at + 24u <= head + size && written < max; at += 24u) {
            const uint8_t *e = cmd_db + at;
            uint32_t address = load_u32(e + 16);
            if (((address >> 16) & 7u) != MUYU_RSC_ARC_SLAVE) continue;
            if (!address || (address & 3u)) continue;
            memcpy(out[written].id, e, 8);
            out[written].id[8] = 0;
            out[written].address = address;
            out[written].count = load_u16(e + 20) / 2u;
            ++written;
        }
    }
    return written;
}

// --- DRV2 active TCS submit -------------------------------------------------

static uint32_t tcs_off(uint32_t reg, unsigned tcs) {
    // IRQ enable/status/clear live at the TCS base itself (no per-TCS stride).
    if (reg < 0x10u) return MUYU_RSC_TCS_BASE_OFFSET + reg;
    return MUYU_RSC_TCS_BASE_OFFSET + MUYU_RSC_TCS_OFFSET * tcs + reg;
}

static uint32_t cmd_off(uint32_t reg, unsigned tcs, unsigned cmd) {
    return MUYU_RSC_TCS_BASE_OFFSET + MUYU_RSC_TCS_OFFSET * tcs + reg + MUYU_RSC_CMD_OFFSET * cmd;
}

static int tcs_read(const MuyuRpmhRscIo *io, uint32_t reg, unsigned tcs, uint32_t *out) {
    return io_read(io, MUYU_RSC_WINDOW, tcs_off(reg, tcs), out);
}

static MuyuRpmhRscStatus tcs_write_sync(const MuyuRpmhRscIo *io, uint32_t reg, unsigned tcs,
                                        uint32_t value) {
    uint32_t off = tcs_off(reg, tcs);
    if (io_write(io, MUYU_RSC_WINDOW, off, value)) return MuyuRpmhRscIoError;
    for (unsigned i = 0; i < 1000000u; ++i) {
        uint32_t back = 0;
        if (io_read(io, MUYU_RSC_WINDOW, off, &back)) return MuyuRpmhRscIoError;
        if (back == value) return MuyuRpmhRscSuccess;
        io_delay(io, 1u);
    }
    return MuyuRpmhRscTimeout;
}

MuyuRpmhRscStatus MuyuRpmhRscVoteGx(const MuyuRpmhRscIo *io, unsigned have,
                                    const MuyuRpmhRscArcs *arcs, MuyuRpmhRscResult *out) {
    if (!io || !arcs || !out || !arcs->both_present) return MuyuRpmhRscArgument;
    memset(out, 0, sizeof *out);
    out->arcs = *arcs;
    if (!(have & (1u << MUYU_RSC_WINDOW))) return MuyuRpmhRscMap;
    MuyuRpmhRscStatus st = MuyuRpmhRscSuccess;

    if (io_read(io, MUYU_RSC_WINDOW, MUYU_RSC_DRV_ID_REG, &out->rsc_id)) return MuyuRpmhRscIoError;
    out->rsc_major = (out->rsc_id >> 16) & 0xffu;
    out->rsc_minor = (out->rsc_id >> 8) & 0xffu;
    if (out->rsc_major < 3) return MuyuRpmhRscVersion;

    if (io_read(io, MUYU_RSC_WINDOW, MUYU_RSC_DRV_PRNT_CHLD_CONFIG, &out->tcs_config))
        return MuyuRpmhRscIoError;
    out->max_tcs = (out->tcs_config >> (6u * MUYU_RSC_DRV_ID)) & 0x3fu;
    out->ncpt = (out->tcs_config >> 27u) & 0x1fu;
    if (out->max_tcs == 0 || out->max_tcs < MUYU_RSC_ACTIVE_TCS_FIRST + MUYU_RSC_ACTIVE_TCS_COUNT ||
        out->ncpt < 2u)
        return MuyuRpmhRscConfig;

    // find_free_tcs: pick an ACTIVE TCS whose STATUS is nonzero (idle), with
    // the same 100us of 1us polling per candidate the vendor uses.
    unsigned tcs = MUYU_RSC_ACTIVE_TCS_FIRST;
    int found = 0;
    for (unsigned attempt = 0; attempt < 100u && !found; ++attempt) {
        for (tcs = MUYU_RSC_ACTIVE_TCS_FIRST;
             tcs < MUYU_RSC_ACTIVE_TCS_FIRST + MUYU_RSC_ACTIVE_TCS_COUNT; ++tcs) {
            uint32_t st = 0;
            if (tcs_read(io, MUYU_RSC_STATUS, tcs, &st)) return MuyuRpmhRscIoError;
            if (st != 0) { out->before_status = st; found = 1; break; }
        }
        if (!found) io_delay(io, 1u);
        ++out->status_polls;
    }
    if (!found) return MuyuRpmhRscBusy;

    // rpmh_rsc_send_data: clear stale ENABLE/WAIT synchronously, then mask the
    // TCS IRQ while we poll CMD_STATUS (no RSC IRQ handler is installed here).
    st = tcs_write_sync(io, MUYU_RSC_CMD_ENABLE, tcs, 0);
    if (st != MuyuRpmhRscSuccess) return st;
    st = tcs_write_sync(io, MUYU_RSC_CMD_WAIT_FOR_CMPL, tcs, 0);
    if (st != MuyuRpmhRscSuccess) return st;
    out->tcs = tcs;
    if (tcs_read(io, MUYU_RSC_IRQ_ENABLE, 0, &out->irq_enable_before)) return MuyuRpmhRscIoError;
    out->irq_enable_after = out->irq_enable_before & ~(1u << tcs);
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_IRQ_ENABLE, 0), out->irq_enable_after))
        return MuyuRpmhRscIoError;

    // __tcs_buffer_write: slot 0 = gfx.lvl (parent), slot 1 = mxc.lvl (GDSC
    // parent). Both ARC commands carry wait=true, so each asks for a response.
    uint32_t msgid = MUYU_RSC_CMD_MSGID_LEN | MUYU_RSC_CMD_MSGID_WRITE | MUYU_RSC_CMD_MSGID_RESP_REQ;
    out->msgid[0] = msgid; out->addr[0] = arcs->gfx.address; out->data[0] = arcs->gfx.selector;
    out->msgid[1] = msgid; out->addr[1] = arcs->mxc.address; out->data[1] = arcs->mxc.selector;
    if (io_write(io, MUYU_RSC_WINDOW, cmd_off(MUYU_RSC_CMD_MSGID, tcs, 0), msgid)) return MuyuRpmhRscIoError;
    if (io_write(io, MUYU_RSC_WINDOW, cmd_off(MUYU_RSC_CMD_ADDR, tcs, 0), out->addr[0])) return MuyuRpmhRscIoError;
    if (io_write(io, MUYU_RSC_WINDOW, cmd_off(MUYU_RSC_CMD_DATA, tcs, 0), out->data[0])) return MuyuRpmhRscIoError;
    if (io_write(io, MUYU_RSC_WINDOW, cmd_off(MUYU_RSC_CMD_MSGID, tcs, 1), msgid)) return MuyuRpmhRscIoError;
    if (io_write(io, MUYU_RSC_WINDOW, cmd_off(MUYU_RSC_CMD_ADDR, tcs, 1), out->addr[1])) return MuyuRpmhRscIoError;
    if (io_write(io, MUYU_RSC_WINDOW, cmd_off(MUYU_RSC_CMD_DATA, tcs, 1), out->data[1])) return MuyuRpmhRscIoError;

    uint32_t wfc = 0, ce = 0;
    if (tcs_read(io, MUYU_RSC_CMD_WAIT_FOR_CMPL, tcs, &wfc)) return MuyuRpmhRscIoError;
    wfc |= (1u << 0) | (1u << 1);
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_CMD_WAIT_FOR_CMPL, tcs), wfc)) return MuyuRpmhRscIoError;
    if (tcs_read(io, MUYU_RSC_CMD_ENABLE, tcs, &ce)) return MuyuRpmhRscIoError;
    ce |= (1u << 0) | (1u << 1);
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_CMD_ENABLE, tcs), ce)) return MuyuRpmhRscIoError;
    out->wait_for_compl = wfc;
    out->cmd_enable = ce;

    // __tcs_set_trigger: clear trigger, then enable, then enable|trigger.
    if (tcs_read(io, MUYU_RSC_CONTROL, tcs, &out->before_control)) return MuyuRpmhRscIoError;
    uint32_t ctrl = out->before_control & ~MUYU_RSC_CONTROL_TRIGGER;
    st = tcs_write_sync(io, MUYU_RSC_CONTROL, tcs, ctrl);
    if (st != MuyuRpmhRscSuccess) return st;
    ctrl &= ~MUYU_RSC_CONTROL_ENABLE;
    st = tcs_write_sync(io, MUYU_RSC_CONTROL, tcs, ctrl);
    if (st != MuyuRpmhRscSuccess) return st;
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_CONTROL, tcs), MUYU_RSC_CONTROL_ENABLE))
        return MuyuRpmhRscIoError;
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_CONTROL, tcs),
                 MUYU_RSC_CONTROL_ENABLE | MUYU_RSC_CONTROL_TRIGGER)) return MuyuRpmhRscIoError;
    if (tcs_read(io, MUYU_RSC_CONTROL, tcs, &out->after_control)) return MuyuRpmhRscIoError;

    // Poll both slots for ISSUED|COMPL (wait_for_compl=1, no IRQ handler yet).
    unsigned done = 0;
    for (unsigned i = 0; i < 100000u; ++i) {
        uint32_t st0 = 0, st1 = 0;
        if (tcs_read(io, MUYU_RSC_CMD_STATUS, tcs, &st0)) return MuyuRpmhRscIoError;
        if (tcs_read(io, MUYU_RSC_CMD_STATUS + MUYU_RSC_CMD_OFFSET, tcs, &st1)) return MuyuRpmhRscIoError;
        out->cmd_status[0] = st0;
        out->cmd_status[1] = st1;
        if ((st0 & MUYU_RSC_CMD_STATUS_ACK) == MUYU_RSC_CMD_STATUS_ACK &&
            (st1 & MUYU_RSC_CMD_STATUS_ACK) == MUYU_RSC_CMD_STATUS_ACK) { done = 1; break; }
        io_delay(io, 10u);
        ++out->command_polls;
    }
    if (!done) return MuyuRpmhRscTimeout;
    if (tcs_read(io, MUYU_RSC_STATUS, tcs, &out->after_status)) return MuyuRpmhRscIoError;

    // tcs_tx_done cleanup: clear ENABLE/WAIT, clear the TCS IRQ bit, restore
    // the IRQ mask only after that sticky status is gone.
    st = tcs_write_sync(io, MUYU_RSC_CMD_ENABLE, tcs, 0);
    if (st != MuyuRpmhRscSuccess) { out->cleanup_io_error = 1; return st; }
    st = tcs_write_sync(io, MUYU_RSC_CMD_WAIT_FOR_CMPL, tcs, 0);
    if (st != MuyuRpmhRscSuccess) { out->cleanup_io_error = 1; return st; }
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_IRQ_CLEAR, 0), 1u << tcs)) { out->cleanup_io_error = 1; return MuyuRpmhRscIoError; }
    out->irq_clear = 1u << tcs;
    if (io_write(io, MUYU_RSC_WINDOW, tcs_off(MUYU_RSC_IRQ_ENABLE, 0), out->irq_enable_before)) { out->cleanup_io_error = 1; return MuyuRpmhRscIoError; }
    out->irq_enable_after = out->irq_enable_before;
    return MuyuRpmhRscSuccess;
}