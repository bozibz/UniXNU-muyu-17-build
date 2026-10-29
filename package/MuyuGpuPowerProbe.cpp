// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off, gated observation of the Adreno power/clock state on muyu (SM8635 / cliffs).
//
//   muyu-gpu-power=1   read GCC GPU branches, then (only if gcc_gpu_cfg_ahb_clk is enabled) the GPUCC
//                      registers this board actually has, then (only if CX is on and the SMMU vote clock
//                      runs) the GPU SMMU global space 0, then (only if CX and GX are on) RBBM_STATUS.
//                      Nothing is written.
//   muyu-gpu-power=2   as 1, plus the single write Linux's gcc_cliffs probe makes: enable
//                      gcc_gpu_cfg_ahb_clk (0x71004 bit 0) if it is clear.
//   muyu-gpu-power=6   mode 5, then re-read SMMU global space 0 and RBBM_STATUS now that CX (and its SMMU
//                      vote clock) are up, then the panic summary.
//   muyu-gpu-power=5   mode 1, then power the GPU CX domain (gpu_cc_cx_gdsc, gdsc.c order), then mode 4's
//                      clock bring-up, then the panic summary.
//   muyu-gpu-power=4   mode 1, then the clock half of gpucc probe + a6xx_gmu_resume (GPLL0 votes, PLL0/PLL1
//                      configuration and lock, hub/gmu clock sources, the GMU branches), no power-domain
//                      write, then the panic summary.
//   muyu-gpu-power=3   as 2, then panic with the whole summary, so the numbers can be photographed on
//                      the panic screen (this board has no persistent kernel log). Folded into the mode
//                      number on purpose: the kernel command line is already at 1008/1000 bytes, so a
//                      second boot argument does not fit.
//
// Why so careful: the GPUCC, the GPU SMMU and the GPU window sit behind clocks and power domains Darwin
// has no driver for, and touching a register in a block whose clock is off hangs the bus instead of
// faulting. On 2026-10-29 walking the whole GPUCC regmap over debugfs from Android hung this board twice.
// Constants and the staged order live in MuyuGpuPowerBoard.h (asserted by check-muyu-gpu-mmio.py) and the
// sequence is host-tested by MuyuGpuPowerBoardTests.c.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "MuyuGpuPowerBoard.h"
}

namespace {

struct Window {
    uint64_t physical;
    uint64_t bytes;
    IOMemoryDescriptor *descriptor;
    IOMemoryMap *map;
    volatile uint8_t *base;
};

Window gWindows[5] = {
    {MGB_GCC_GPU_PAGE, 0x1000, nullptr, nullptr, nullptr},
    {MGB_GCC_VOTE_PAGE, 0x1000, nullptr, nullptr, nullptr},
    {MGB_GPUCC_BASE, MGB_GPUCC_SIZE, nullptr, nullptr, nullptr},
    {MGB_SMMU_BASE, MGB_SMMU_GR0_SIZE, nullptr, nullptr, nullptr},
    {MGB_GPU_BASE, MGB_GPU_WINDOW, nullptr, nullptr, nullptr},
};

bool mapWindow(unsigned index) {
    Window &w = gWindows[index];
    if (w.base) return true;
    w.descriptor = IOMemoryDescriptor::withPhysicalAddress(w.physical, (IOByteCount)w.bytes, kIODirectionInOut);
    if (!w.descriptor) return false;
    w.map = w.descriptor->map(kIOMapAnywhere);
    if (!w.map) {
        OSSafeReleaseNULL(w.descriptor);
        return false;
    }
    w.base = reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress());
    return w.base != nullptr;
}

int read32(void *, unsigned window, uint32_t offset, uint32_t *out) {
    if (window > MGB_WIN_GPU || !out || !gWindows[window].base) return -1;
    if (offset & 3 || (uint64_t)offset + 4 > gWindows[window].bytes) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gWindows[window].base + offset);
    return 0;
}

int write32(void *, unsigned window, uint32_t offset, uint32_t value) {
    if (window > MGB_WIN_GPU || !gWindows[window].base) return -1;
    if (offset & 3 || (uint64_t)offset + 4 > gWindows[window].bytes) return -1;
    *reinterpret_cast<volatile uint32_t *>(gWindows[window].base + offset) = value;
    return 0;
}

void delayUs(void *, unsigned us) { IODelay(us); }

const char *yesNo(int v) { return v ? "Y" : "N"; }

} // namespace

extern "C" void muyu_gpu_power_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("muyu-gpu-power", &mode, sizeof mode) || !mode) return;
    if (mode > 6) mode = 6;
    unsigned panicDump = (mode >= 3);
    unsigned runMode = (mode == 2 || mode == 3) ? 2 : 1;

    unsigned have = 0;
    for (unsigned i = 0; i < 5; ++i)
        if (mapWindow(i)) have |= 1u << i;
    IOLog("MUYU GPU POWER m%u mapped gcc=%d gccvote=%d gpucc=%d smmu=%d gpu=%d\n", mode,
          !!(have & (1u << MGB_WIN_GCC_GPU)), !!(have & (1u << MGB_WIN_GCC_VOTE)),
          !!(have & (1u << MGB_WIN_GPUCC)), !!(have & (1u << MGB_WIN_SMMU)),
          !!(have & (1u << MGB_WIN_GPU)));

    struct mgb_io io = { nullptr, read32, write32, delayUs };
    struct mgb_result r;
    mgb_run(&io, runMode, have, &r);

    IOLog("MUYU GPU POWER gcc %s ahb=%08x/%08x enabled=%u memnoc=%08x dvm=%08x ddrss=%08x gpll0=%08x\n",
          mgb_state_text(r.gcc), r.cfg_ahb, r.cfg_ahb_after, r.enabled_ahb, r.memnoc, r.dvm, r.ddrss_axi,
          r.pll_vote);
    IOLog("MUYU GPU POWER cc %s cx=%08x gx=%08x hwctrl=%08x ahb=%08x crc_ahb=%08x smmu=%08x gmu=%08x\n",
          mgb_state_text(r.cc), r.cx_gdscr, r.gx_gdscr, r.cx_hw_ctrl, r.cc_ahb, r.cc_crc_ahb, r.cc_smmu,
          r.cc_gmu);
    IOLog("MUYU GPU POWER cc cxo=%08x hubcx=%08x accu=%08x sleep=%08x memnoc=%08x cx_ff=%08x hub_aon=%08x\n",
          r.cc_cxo, r.cc_hub_cx_int, r.cc_accu_shift, r.cc_sleep, r.cc_memnoc, r.cc_cx_ff, r.cc_hub_aon);
    IOLog("MUYU GPU POWER pll1 mode=%08x opmode=%08x l=%08x user=%08x gmu_rcgr=%08x gmu_cfg=%08x hub_rcgr=%08x hub_cfg=%08x\n",
          r.pll1_mode, r.pll1_opmode, r.pll1_l, r.pll1_user, r.gmu_rcgr, r.gmu_cfg, r.hub_rcgr, r.hub_cfg);
    IOLog("MUYU GPU POWER gmu src=%u div=%u  hub src=%u div=%u  cx_on=%s gx_on=%s\n",
          mgb_rcg_src(r.gmu_cfg), mgb_rcg_div(r.gmu_cfg), mgb_rcg_src(r.hub_cfg), mgb_rcg_div(r.hub_cfg),
          yesNo(mgb_gdsc_on(r.cx_gdscr)), yesNo(mgb_gdsc_on(r.gx_gdscr)));
    IOLog("MUYU GPU POWER smmu %s scr0=%08x id0=%08x id1=%08x id2=%08x gfsr=%08x smr=%u cb=%u\n",
          mgb_state_text(r.smmu), r.scr0, r.id0, r.id1, r.id2, r.gfsr, mgb_smmu_num_smr(r.id0),
          mgb_smmu_num_cb(r.id1));
    IOLog("MUYU GPU POWER gpu %s rbbm=%08x ioerr=%d\n", mgb_state_text(r.gpu), r.rbbm_status, r.io_error);

    struct mgb_clock_result clock;
    __builtin_memset(&clock, 0, sizeof clock);
    int cx_result = -2;                 // -2: not attempted
    uint32_t cx_gdscr_after = 0, cx_hwctrl_after = 0;
    unsigned cx_polls = 0;
    if (mode >= 5) {
        cx_result = mgb_power_cx(&io, &r, &cx_gdscr_after, &cx_hwctrl_after, &cx_polls);
        IOLog("MUYU GPU POWER cx power=%d gdscr=%08x hwctrl=%08x polls=%u\n", cx_result, cx_gdscr_after,
              cx_hwctrl_after, cx_polls);
        /* Report the CX words in the gdscr column too: they are the ones the next stage acts on. */
        IOLog("MUYU GPU POWER cx before=%08x after=%08x\n", r.cx_gdscr, cx_gdscr_after);
    }
    if (mode >= 4) {
        mgb_gmu_clocks_on(&io, &r, have, &clock);
        IOLog("MUYU GPU POWER clk step=%u failed=%d lock_polls=%u hub_polls=%u gmu_polls=%u branch_polls=%u\n",
              clock.step, clock.failed, clock.lock_polls, clock.hub_polls, clock.gmu_polls, clock.branch_polls);
        IOLog("MUYU GPU POWER clk pll1=%08x/%08x gpll0=%08x gmucfg=%08x hubcfg=%08x cx_gmu=%08x cx=%08x\n",
              clock.pll1_mode, clock.pll1_opmode, clock.gpll0, clock.gmu_cfg, clock.hub_cfg, clock.cx_gmu,
              r.cx_gdscr);
        if (mode == 6) {
            mgb_observe_powered(&io, &r, have);
            IOLog("MUYU GPU POWER post smmu %s scr0=%08x id0=%08x id1=%08x id2=%08x gfsr=%08x smr=%u cb=%u\n",
                  mgb_state_text(r.smmu), r.scr0, r.id0, r.id1, r.id2, r.gfsr, mgb_smmu_num_smr(r.id0),
                  mgb_smmu_num_cb(r.id1));
            IOLog("MUYU GPU POWER post gpu %s rbbm=%08x\n", mgb_state_text(r.gpu), r.rbbm_status);
        }
        IOLog("MUYU GPU POWER clk gmu src=%u div=%u hub src=%u div=%u gmu_running=%s\n",
              mgb_rcg_src(clock.gmu_cfg), mgb_rcg_div(clock.gmu_cfg), mgb_rcg_src(clock.hub_cfg),
              mgb_rcg_div(clock.hub_cfg), yesNo(mgb_branch_running(clock.cx_gmu)));
    }

    if (panicDump) {
        panic("MUYU GPU POWER m%u gcc=%s cc=%s smmu=%s gpu=%s ioerr=%d\n"
              "ahb=%08x/%08x en=%u memnoc=%08x dvm=%08x ddrss=%08x gpll0=%08x\n"
              "cx=%08x gx=%08x hwctrl=%08x ccahb=%08x crc=%08x vote=%08x gmu=%08x cxo=%08x hubcx=%08x\n"
              "accu=%08x sleep=%08x ccmemnoc=%08x cxff=%08x hubaon=%08x\n"
              "pll1=%08x/%08x l=%08x user=%08x gmurcgr=%08x gmucfg=%08x hubrcgr=%08x hubcfg=%08x\n"
              "smmu scr0=%08x id0=%08x id1=%08x id2=%08x gfsr=%08x rbbm=%08x\n"
              "clk step=%u failed=%d lock=%u hub=%u gmu=%u br=%u pll1=%08x/%08x gpll0=%08x gmucfg=%08x hubcfg=%08x cxgmu=%08x gpll0_after=%08x\n"
              "cx power=%d gdscr=%08x hwctrl=%08x polls=%u",
              mode, mgb_state_text(r.gcc), mgb_state_text(r.cc), mgb_state_text(r.smmu), mgb_state_text(r.gpu),
              r.io_error, r.cfg_ahb, r.cfg_ahb_after, r.enabled_ahb, r.memnoc, r.dvm, r.ddrss_axi, r.pll_vote,
              r.cx_gdscr, r.gx_gdscr, r.cx_hw_ctrl, r.cc_ahb, r.cc_crc_ahb, r.cc_smmu, r.cc_gmu, r.cc_cxo,
              r.cc_hub_cx_int, r.cc_accu_shift, r.cc_sleep, r.cc_memnoc, r.cc_cx_ff, r.cc_hub_aon,
              r.pll1_mode, r.pll1_opmode, r.pll1_l, r.pll1_user, r.gmu_rcgr, r.gmu_cfg, r.hub_rcgr, r.hub_cfg,
              r.scr0, r.id0, r.id1, r.id2, r.gfsr, r.rbbm_status,
              clock.step, clock.failed, clock.lock_polls, clock.hub_polls, clock.gmu_polls, clock.branch_polls,
              clock.pll1_mode, clock.pll1_opmode, clock.gpll0, clock.gmu_cfg, clock.hub_cfg, clock.cx_gmu,
              clock.gpll0_after, cx_result, cx_gdscr_after, cx_hwctrl_after, cx_polls);
    }
}
