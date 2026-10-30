// SPDX-License-Identifier: BSD-2-Clause-Patent
// muyu (SM8635 / cliffs, Adreno 735) GX power-up through the GMU's modern OOB handshake.
//
// After ShengGmuColdStart() the GMU is running and answering HFI, but the GX rail is still
// off: on this board the GPU's GX domain and gfx3d clocks are owned by the GMU through
// gx_clkctl_gx_gdsc@0x3d68024 (GPU-CLIFFS-MMIO.md §5), so the CPU must not collapse or
// enable the GX GDSC itself. The documented way to ask for GX is the out-of-band "GPU_SET"
// request that the vendor driver uses (a6xx_gmu.c, a6xx_gmu_set_oob / a6xx_gmu_clear_oob):
//
//   non-legacy (this board):  set bit 30 in GMU_HOST2GMU_INTR_SET
//                             GMU acknowledges with bit 31 in GMU_GMU2HOST_INTR_INFO
//                             clear the ack with GMU_GMU2HOST_INTR_CLR bit 31
//                             clear the request with GMU_HOST2GMU_INTR_SET bit 31
//
// The A7XX variant of GMU_SPTPRAC_PWR_CLK_STATUS reports the result: bit 0 is
// GX_HM_GDSC_POWER_OFF and bit 1 is GX_HM_CLK_OFF (a6xx_gmu.xml, variants="A7XX"), so
// "both clear" is the first hard evidence that the GPU block is powered and clocked.
//
// Nothing here reads the GPU register window: that is only safe once the caller has seen
// gx_on. Every step is bounded, and an I/O error stops the sequence without writing more.
#ifndef MUYU_GX_H
#define MUYU_GX_H
#include <stdint.h>

// GMU core-window dword offsets (a6xx_gmu.xml). The caller converts them to a byte offset
// the same way ShengGmu.c does: word * 4 - SHENG_GMU_CORE_GPU_DELTA.
#define MUYU_GX_REG_PWR_STATUS 0x1f8d0u   // GMU_SPTPRAC_PWR_CLK_STATUS (A7XX variant)
#define MUYU_GX_REG_INTR_CLR   0x1f991u   // GMU_GMU2HOST_INTR_CLR
#define MUYU_GX_REG_INTR_INFO  0x1f992u   // GMU_GMU2HOST_INTR_INFO
#define MUYU_GX_REG_INTR_SET   0x1f994u   // GMU_HOST2GMU_INTR_SET
// gx_clkctl_gx_gdsc, the GX clock-controller rail (DT soc/qcom,gdsc@3d68024): physical
// 0x3d68024 is byte 0x24 of the GMU core window (base 0x3d68000), i.e. dword 0x1a009 under
// the word*4 - CORE_GPU_DELTA(0x68000) convention.
#define MUYU_GX_REG_CLKCTL_GDSC 0x1a009u

#define MUYU_GX_OOB_SET   (1u << 30)      // GMU_OOB_GPU_SET, non-legacy "set_new"
#define MUYU_GX_OOB_ACK   (1u << 31)      // ack_new
#define MUYU_GX_OOB_CLEAR (1u << 31)      // clear_new

// A7XX GMU_SPTPRAC_PWR_CLK_STATUS: GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF.
#define MUYU_GX_STATUS_GX_OFF 0x3u

typedef struct {
    void *cookie;
    int (*read32)(void *cookie, uint32_t dword, uint32_t *out);      // 0 on success
    int (*write32)(void *cookie, uint32_t dword, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);                     // may be NULL
} MuyuGxIo;

typedef struct {
    unsigned ran, acked, cleared, polls;   // polls = how many times INFO was read
    int io_error;                          // an MMIO access failed; sequence stopped
    int gx_on;                             // status_after shows GX powered AND clocked
    uint32_t status_before, status_after, intr_info;
} MuyuGxResult;

typedef struct {
    unsigned ran, was_on, enabled, polls;
    int io_error;
    uint32_t before, after;
} MuyuGxRailResult;

// GDSC_PWR_ON / GDSC_SW_COLLAPSE, same layout as gpucc-cliffs.c and package/MuyuGpuPowerBoard.h.
#define MUYU_GX_GDSC_PWR_ON (1u << 31)
#define MUYU_GX_GDSC_SW_COLLAPSE (1u << 0)

// Power the GX clock-controller rail exactly as gdsc.c enables a non-hw-ctrl GDSC: clear
// SW_COLLAPSE, then poll PWR_ON. This is the CPU-side "gxpd" vote Linux makes before the
// GMU can hand out GX clocks; nothing else in the block is touched.
static inline MuyuGxRailResult muyu_gx_clkctl_on(const MuyuGxIo *io, unsigned budget) {
    MuyuGxRailResult r;
    __builtin_memset(&r, 0, sizeof r);
    if (!io || !io->read32 || !io->write32 || !budget || budget > 65535) return r;
    r.ran = 1;
    if (io->read32(io->cookie, MUYU_GX_REG_CLKCTL_GDSC, &r.before)) {
        r.io_error = 1;
        return r;
    }
    if (r.before & MUYU_GX_GDSC_PWR_ON) {
        r.was_on = 1;
        r.after = r.before;
        return r;
    }
    if (io->write32(io->cookie, MUYU_GX_REG_CLKCTL_GDSC,
                    r.before & ~MUYU_GX_GDSC_SW_COLLAPSE)) {
        r.io_error = 1;
        return r;
    }
    if (io->delay_us) io->delay_us(io->cookie, 1);
    for (unsigned i = 0; i < budget; ++i) {
        ++r.polls;
        if (io->read32(io->cookie, MUYU_GX_REG_CLKCTL_GDSC, &r.after)) {
            r.io_error = 1;
            return r;
        }
        if (r.after & MUYU_GX_GDSC_PWR_ON) {
            r.enabled = 1;
            return r;
        }
        if (io->delay_us) io->delay_us(io->cookie, 10);
    }
    return r;
}

// One request/acknowledge/clear cycle. budget bounds the ack poll (1..65535); a 100 us
// pause is inserted between reads when delay_us is supplied. The ack clear is written even
// on timeout (as the vendor driver does); the request is only cleared after an ack.
// Parameterised variant: the non-legacy protocol uses bits 30/31, the legacy one 16/24
// (a6xx_gmu.c's a6xx_gmu_oob_bits table). It exists so the probe can ask the same GMU
// both ways if the first set is not acknowledged.
static inline MuyuGxResult muyu_gx_power_on_bits(const MuyuGxIo *io, unsigned set_bit,
                                                 unsigned ack_bit, unsigned clear_bit,
                                                 unsigned budget) {
    MuyuGxResult r;
    __builtin_memset(&r, 0, sizeof r);
    if (!io || !io->read32 || !io->write32 || !budget || budget > 65535 ||
        set_bit > 31 || ack_bit > 31 || clear_bit > 31) return r;
    r.ran = 1;
    if (io->read32(io->cookie, MUYU_GX_REG_PWR_STATUS, &r.status_before)) {
        r.io_error = 1;
        return r;
    }
    if (io->write32(io->cookie, MUYU_GX_REG_INTR_SET, 1u << set_bit)) {
        r.io_error = 1;
        return r;
    }
    for (unsigned i = 0; i < budget; ++i) {
        if (io->read32(io->cookie, MUYU_GX_REG_INTR_INFO, &r.intr_info)) {
            r.io_error = 1;
            return r;
        }
        ++r.polls;
        if (r.intr_info & (1u << ack_bit)) {
            r.acked = 1;
            break;
        }
        if (io->delay_us) io->delay_us(io->cookie, 100);
    }
    // The vendor driver clears the acknowledge interrupt whether or not the poll succeeded.
    if (io->write32(io->cookie, MUYU_GX_REG_INTR_CLR, 1u << ack_bit)) {
        r.io_error = 1;
        return r;
    }
    if (r.acked) {
        if (io->write32(io->cookie, MUYU_GX_REG_INTR_SET, 1u << clear_bit)) {
            r.io_error = 1;
            return r;
        }
        r.cleared = 1;
    }
    if (io->read32(io->cookie, MUYU_GX_REG_PWR_STATUS, &r.status_after)) {
        r.io_error = 1;
        return r;
    }
    r.gx_on = (r.status_after & MUYU_GX_STATUS_GX_OFF) == 0;
    return r;
}

static inline MuyuGxResult muyu_gx_power_on(const MuyuGxIo *io, unsigned budget) {
    return muyu_gx_power_on_bits(io, 30, 31, 31, budget);
}

#endif
