// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// CPU-side RPMh ARC voter for muyu's GX rail (phase 2 first shot).
//
// gpu_cc_gx_gdsc's parent supply is the ARC voter resource mxc.lvl
// (regulator-pm_v6c_s6_gfx_voter_level, init level 0x30), whose own parent is
// gfx.lvl (regulator-pmxr2230-s4-level, init level 0x10). Both live on slave 3
// of the RPMh RSC share that starts at 0x17a20000 (drv@2). Android's Linux
// grants GX by asking the GMU; the GMU uses its RSC-PDC sequencer. On this
// port there is no AOP/Linux voter, so the CPU has to send the two ARC level
// votes itself before the gx GDSC can answer.
//
// Hardware facts are pinned from the vendored sources (rpmh-rsc.c,
// rpmh-regulator.c, cmd-db.c, qcom-rpmh-rsc.h) and from muyu's fs-decoded DT:
//   - drv@2 qcom,drv-id=2, qcom,tcs-offset=0xd00; tcs_base = 0x17a20d00
//   - channel@0 qcom,tcs-config pairs (2,3),(0,2),(1,2),(3,0),(4,1); in pair
//     order ACTIVE has 3 TCSes at ids {0,1,2}, then SLEEP {3,4}, WAKE {5,6},
//     CONTROL none, FAST_PATH {7}
//   - ver_3_0 register layout: per-TCS stride 672, command stride 24; the IRQ
//     enable/status/clear regs sit at tcs_base itself, NOT per-TCS
//   - cmd-db walk: header magic, 8x16-byte slave rows at data+144, 24-byte
//     entries, aux blobs little-endian, u16 ARC level table
//   - an ARC command has wait=true, writes selector to resource addr + 0
#ifndef MUYU_RPMH_RSC_H
#define MUYU_RPMH_RSC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// DRV2 of the RSC at /soc/rsc@17a00000. The probe maps this 0x10000 share as
// window index 5; the package's IO peer only ever uses that window.
#define MUYU_RSC_WINDOW 5u
#define MUYU_RSC_DRV2_BASE UINT64_C(0x17a20000)
#define MUYU_RSC_DRV2_BYTES UINT64_C(0x10000)
#define MUYU_RSC_DRV_ID 2u
#define MUYU_RSC_DRV_ID_REG 0x00000000u
#define MUYU_RSC_DRV_PRNT_CHLD_CONFIG 0x0000000cu
#define MUYU_RSC_TCS_BASE_OFFSET 0x00000d00u

// ACTIVE TCS assignment derived by rpmh_probe_channel_tcs_config (pair order,
// not enum order): ids {0,1,2}.
#define MUYU_RSC_ACTIVE_TCS_FIRST 0u
#define MUYU_RSC_ACTIVE_TCS_COUNT 3u
#define MUYU_RSC_MAX_CMDS 16u

// ver_3_0 register offsets, from the TCS base. Regs < 0x10 are the IRQ
// enable/status/clear masks addressed at tcs_base without a TCS stride.
#define MUYU_RSC_IRQ_ENABLE 0x00000000u
#define MUYU_RSC_IRQ_STATUS 0x00000004u
#define MUYU_RSC_IRQ_CLEAR 0x00000008u
#define MUYU_RSC_CMD_WAIT_FOR_CMPL 0x00000020u
#define MUYU_RSC_CONTROL 0x00000024u
#define MUYU_RSC_STATUS 0x00000028u
#define MUYU_RSC_CMD_ENABLE 0x0000002cu
#define MUYU_RSC_CMD_MSGID 0x00000034u
#define MUYU_RSC_CMD_ADDR 0x00000038u
#define MUYU_RSC_CMD_DATA 0x0000003cu
#define MUYU_RSC_CMD_STATUS 0x00000040u
#define MUYU_RSC_TCS_OFFSET 672u
#define MUYU_RSC_CMD_OFFSET 24u

#define MUYU_RSC_CMD_MSGID_LEN 8u
#define MUYU_RSC_CMD_MSGID_RESP_REQ (1u << 8)
#define MUYU_RSC_CMD_MSGID_WRITE (1u << 16)
#define MUYU_RSC_CONTROL_TRIGGER (1u << 24)
#define MUYU_RSC_CONTROL_ENABLE (1u << 16)
#define MUYU_RSC_CMD_STATUS_ISSUED (1u << 8)
#define MUYU_RSC_CMD_STATUS_COMPL (1u << 16)
#define MUYU_RSC_CMD_STATUS_ACK (MUYU_RSC_CMD_STATUS_ISSUED | MUYU_RSC_CMD_STATUS_COMPL)

#define MUYU_RSC_ARC_SLAVE 3u
#define MUYU_RSC_CMD_DB_MAGIC UINT32_C(0x0c0330db)
#define MUYU_RSC_CMD_DB_MAX_LEVELS 16u

#define MUYU_RSC_GFX_NAME "gfx.lvl"
#define MUYU_RSC_MXC_NAME "mxc.lvl"
#define MUYU_RSC_GFX_INIT_LEVEL 0x10u
#define MUYU_RSC_MXC_INIT_LEVEL 0x30u

typedef struct {
    char name[8];                          // cmd-db entry id, NUL padded
    uint32_t address;                      // cmd-db read address (ACCL_TYPE bits preserved)
    uint16_t levels[MUYU_RSC_CMD_DB_MAX_LEVELS]; // ARC level table, u16 LE
    unsigned count;
    unsigned selector;                     // first index with init <= levels[index]
} MuyuRpmhRscArc;

typedef struct {
    MuyuRpmhRscArc gfx;                    // parent, sent first
    MuyuRpmhRscArc mxc;                    // GDSC parent, sent second
    unsigned both_present;
} MuyuRpmhRscArcs;

typedef struct {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);
} MuyuRpmhRscIo;

typedef enum {
    MuyuRpmhRscSuccess = 0,
    MuyuRpmhRscArgument,
    MuyuRpmhRscDbVersion,
    MuyuRpmhRscDbCorrupt,
    MuyuRpmhRscDbMissing,
    MuyuRpmhRscLevel,                      // no ARC level table entry satisfies init
    MuyuRpmhRscMap,                        // DRV2 window bit absent from "have"
    MuyuRpmhRscConfig,                     // PRNT_CHLD_CONFIG cannot fit the two commands
    MuyuRpmhRscVersion,                    // RSC major < 3 (this package implements ver_3_0)
    MuyuRpmhRscBusy,                       // no ACTIVE TCS became idle in time
    MuyuRpmhRscTimeout,                    // a command never acked within the poll budget
    MuyuRpmhRscIoError,                         // MMIO peer failed
} MuyuRpmhRscStatus;

typedef struct {
    unsigned tcs;
    unsigned status_polls;
    unsigned command_polls;
    uint32_t rsc_id;
    uint32_t rsc_major, rsc_minor;
    uint32_t tcs_config;
    uint32_t max_tcs, ncpt;
    MuyuRpmhRscArcs arcs;
    uint32_t before_control, after_control;
    uint32_t before_status, after_status;
    uint32_t wait_for_compl, cmd_enable, irq_clear, irq_enable_before, irq_enable_after;
    uint32_t msgid[MUYU_RSC_MAX_CMDS];
    uint32_t addr[MUYU_RSC_MAX_CMDS];
    uint32_t data[MUYU_RSC_MAX_CMDS];
    uint32_t cmd_status[MUYU_RSC_MAX_CMDS];
    unsigned cleanup_io_error;
} MuyuRpmhRscResult;

// Parse the AOP command-db snapshot for the two ARC resources and derive their
// selectors from the caller's DT init levels. The walk and bounds checks mirror
// ShengGmuCommandDb.c so the probe sees the same snapshot semantics.
MuyuRpmhRscStatus MuyuRpmhRscArcsParse(const uint8_t *cmd_db, size_t cmd_db_bytes,
                                       uint32_t cmd_db_version,
                                       uint32_t gfx_init_level, uint32_t mxc_init_level,
                                       MuyuRpmhRscArcs *out);

// Send the two ARC level votes as one ACTIVE TCS transfer in the vendored
// order: find a free TCS (STATUS nonzero), sync-clear ENABLE/WAIT, buffer both
// slots with wait=true, set WAIT/ENABLE, trigger (CONTROL enable then
// enable|trigger), poll each slot's CMD_STATUS for ISSUED|COMPL, then clear
// ENABLE/WAIT and the TCS's IRQ bit. Only the RSC window bit of "have" is
// consulted.
// One ARC entry as the command DB stores it, for diagnostics: the vendor GPU driver votes on
// "mx.lvl" (or "gmxc.lvl"), while the devicetree names the rail "mxc.lvl". Printing every ARC
// entry shows which of those really exist and what the GMU is expected to vote on.
typedef struct {
    char id[9];
    uint32_t address;
    unsigned count;
} MuyuRpmhRscEntry;

// Walk the command DB and fill up to `max` ARC entries (slave 3). Returns the number written.
unsigned MuyuRpmhRscListArcs(const uint8_t *cmd_db, size_t bytes, uint32_t version,
                             MuyuRpmhRscEntry *out, unsigned max);

MuyuRpmhRscStatus MuyuRpmhRscVoteGx(const MuyuRpmhRscIo *io, unsigned have,
                                    const MuyuRpmhRscArcs *arcs, MuyuRpmhRscResult *out);

const char *MuyuRpmhRscStatusText(MuyuRpmhRscStatus status);

#ifdef __cplusplus
}
#endif
#endif // MUYU_RPMH_RSC_H