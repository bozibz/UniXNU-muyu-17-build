// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off, gated observation of the Adreno power/clock state on muyu (SM8635 / cliffs).
//
//   muyu-gpu-power=1   read GCC GPU branches, then (only if gcc_gpu_cfg_ahb_clk is enabled) the GPUCC
//                      registers this board actually has, then (only if CX is on and the SMMU vote clock
//                      runs) the GPU SMMU global space 0, then (only if CX and GX are on) RBBM_STATUS.
//                      Nothing is written.
//   muyu-gpu-power=2   as 1, plus the single write Linux's gcc_cliffs probe makes: enable
//                      gcc_gpu_cfg_ahb_clk (0x71004 bit 0) if it is clear.
//   muyu-gpu-power-panic=1
//                      after the run, panic with the whole summary, so the numbers can be photographed
//                      on the panic screen (this board has no persistent kernel log).
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
    if (mode > 2) mode = 2;
    unsigned panicDump = 0;
    PE_parse_boot_argn("muyu-gpu-power-panic", &panicDump, sizeof panicDump);

    unsigned have = 0;
    for (unsigned i = 0; i < 5; ++i)
        if (mapWindow(i)) have |= 1u << i;
    IOLog("MUYU GPU POWER m%u mapped gcc=%d gccvote=%d gpucc=%d smmu=%d gpu=%d\n", mode,
          !!(have & (1u << MGB_WIN_GCC_GPU)), !!(have & (1u << MGB_WIN_GCC_VOTE)),
          !!(have & (1u << MGB_WIN_GPUCC)), !!(have & (1u << MGB_WIN_SMMU)),
          !!(have & (1u << MGB_WIN_GPU)));

    struct mgb_io io = { nullptr, read32, write32, delayUs };
    struct mgb_result r;
    mgb_run(&io, mode, have, &r);

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

    if (panicDump) {
        panic("MUYU GPU POWER m%u gcc=%s cc=%s smmu=%s gpu=%s ioerr=%d\n"
              "ahb=%08x/%08x en=%u memnoc=%08x dvm=%08x ddrss=%08x gpll0=%08x\n"
              "cx=%08x gx=%08x hwctrl=%08x ccahb=%08x crc=%08x vote=%08x gmu=%08x cxo=%08x hubcx=%08x\n"
              "accu=%08x sleep=%08x ccmemnoc=%08x cxff=%08x hubaon=%08x\n"
              "pll1=%08x/%08x l=%08x user=%08x gmurcgr=%08x gmucfg=%08x hubrcgr=%08x hubcfg=%08x\n"
              "smmu scr0=%08x id0=%08x id1=%08x id2=%08x gfsr=%08x rbbm=%08x",
              mode, mgb_state_text(r.gcc), mgb_state_text(r.cc), mgb_state_text(r.smmu), mgb_state_text(r.gpu),
              r.io_error, r.cfg_ahb, r.cfg_ahb_after, r.enabled_ahb, r.memnoc, r.dvm, r.ddrss_axi, r.pll_vote,
              r.cx_gdscr, r.gx_gdscr, r.cx_hw_ctrl, r.cc_ahb, r.cc_crc_ahb, r.cc_smmu, r.cc_gmu, r.cc_cxo,
              r.cc_hub_cx_int, r.cc_accu_shift, r.cc_sleep, r.cc_memnoc, r.cc_cx_ff, r.cc_hub_aon,
              r.pll1_mode, r.pll1_opmode, r.pll1_l, r.pll1_user, r.gmu_rcgr, r.gmu_cfg, r.hub_rcgr, r.hub_cfg,
              r.scr0, r.id0, r.id1, r.id2, r.gfsr, r.rbbm_status);
    }
}
