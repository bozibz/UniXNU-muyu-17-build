// SPDX-License-Identifier: MIT
// muyu (SM8635, cliffs) touch board constants, verified against the device tree, the DTBO overlay and
// Xiaomi's own kernel branch (MiCode/Xiaomi_Kernel_OpenSource, muyu-v-oss: gcc-cliffs.c,
// pinctrl-cliffs.c, pinctrl-msm.c). See docs/TOUCH-PORT.md for the derivation.
#ifndef TOUCH_BOARD_MUYU_H
#define TOUCH_BOARD_MUYU_H

#include <stdint.h>

// GENI SPI serial engine 4 of qupv3_0 (DT: soc/qcom,qupv3_0_geni_se@ac0000/spi@a90000).
// One window covers the serial engine and its wrapper: the separate 0xa90000 mapping the first probe tried did
// not come up, while the wrapper range did, so both live in the same mapping now.
#define TM_QUP_WINDOW_BASE  0x00a00000ULL
#define TM_QUP_WINDOW_SIZE  0x00100000ULL
#define TM_QUP_SE_OFFSET    0x00090000ULL
#define TM_QUP_WRAP_OFFSET  0x000c0000ULL
#define TM_QUP_WRAPPER_BASE 0x00ac0000ULL
#define TM_SE_BASE          0x00a90000ULL
#define TM_SE_SIZE          0x4000ULL
#define TM_SE_GIC_SPI       0x165u            // DT interrupts = <0x165 4>
#define TM_SPI_HZ           19200000u         // DT spi-max-frequency = <0x124f800>

// GCC (DT clock-controller@100000). Three branches share one vote register; the SE4 RCG can run from
// TCXO at 19.2 MHz, so GPLL0 never has to be started for this bus (gcc_parent_map_0 + the SE4 table's
// F(19200000, P_BI_TCXO, 1, 0, 0) entry).
#define TM_GCC_BASE             0x00100000ULL
#define TM_GCC_VOTE             (TM_GCC_BASE + 0x52008ULL)
#define TM_VOTE_WRAP0_CORE_2X   (1u << 18)
#define TM_VOTE_WRAP0_CORE      (1u << 19)
#define TM_VOTE_WRAP0_S4        (1u << 26)
#define TM_GCC_HALT_CORE_2X     0x23018u
#define TM_GCC_HALT_CORE        0x23008u
#define TM_GCC_HALT_S4          0x183bcu
// gcc_qupv3_wrap0_s4_clk_src: clk_rcg2, mnd_width 16, hid_width 5, parent 0 = TCXO.
#define TM_S4_RCG_CMD           0x183c8u
#define TM_S4_RCG_CFG           (TM_S4_RCG_CMD + 0x4u)   // [4:0] hid, [10:8] src, [13:12] mode
#define TM_S4_RCG_M             (TM_S4_RCG_CMD + 0x8u)
#define TM_S4_RCG_N             (TM_S4_RCG_CMD + 0xcu)
#define TM_RCG_SRC_TCXO         0u
#define TM_RCG_SRC_GPLL0_MAIN   1u
#define TM_RCG_SRC_GPLL0_EVEN   6u
// clk-rcg2.c (vendor kernel): CMD bit0 = CMD_UPDATE (self-clearing), bit1 = CMD_ROOT_EN,
// bit31 = CMD_ROOT_OFF (status). gcc_qupv3_wrap0_s4_clk_src is enable_safe_config +
// HW_CLK_CTRL_MODE, so the root stays parked at CXO until software force-enables it; leave
// ROOT_EN set (clr_force_enable is only right when a hardware clock controller owns the root,
// and this board has no driver for it).
#define TM_CMD_RCG_UPDATE       (1u << 0)
#define TM_CMD_RCG_ROOT_EN      (1u << 1)
#define TM_CMD_RCG_ROOT_OFF     (1u << 31)
// Every CBCR/halt register reports "clock off" in bit 31 (gcc-cliffs.c: BRANCH_HALT_VOTED / CBCR_CLK_OFF).
#define TM_CBCR_CLK_OFF         (1u << 31)

// TLMM (DT pinctrl@f000000, reg 16 MiB). The vendor driver does NOT put the GPIO registers at the
// window base: pinctrl-cliffs.c has REG_BASE 0x100000, and ctl_reg = REG_BASE + 0x1000*gpio. So gpio0
// ctl lives at 0x0f100000, i.e. the DT window plus 0x100000 - which is why every probe that used
// 0x0f000000 + 0x1000*gpio reset the board (it was poking the wrong 256 KiB, not the pin registers).
#define TM_TLMM_BASE        0x0f100000ULL
#define TM_TLMM_DT_BASE     0x0f000000ULL
#define TM_TLMM_STRIDE      0x1000u
#define TM_TLMM_GIC_SPI     208u              // DT pinctrl interrupts = <0 0xd0 4>
// The mux field holds the index inside that pin's own function list (pinctrl-msm.c: mask =
// GENMASK(mux_bit + order_base_2(nfuncs) - 1, mux_bit)), and qup0_se4_lN is entry 1 of gpio 16..19.
#define TM_PIN_MUX_QUP_SE4  1u
#define TM_PIN_DRIVE_6MA    6u
#define TM_PIN_CTL(func,drive) (((uint32_t)(func) << 2) | ((uint32_t)(drive) << 6))
#define TM_PIN_SPI_CTL      TM_PIN_CTL(TM_PIN_MUX_QUP_SE4, TM_PIN_DRIVE_6MA)   // 0x184
#define TM_GPIO_MISO        16u               // qup0_se4_l0
#define TM_GPIO_MOSI        17u               // qup0_se4_l1
#define TM_GPIO_CLK         18u               // qup0_se4_l2
#define TM_GPIO_CS          19u               // qup0_se4_l3
#define TM_GPIO_IRQ         54u               // DTBO: novatek,irq-gpio = <&tlmm 0x36 0x2001>
#define TM_GPIO_LCD_ID      26u               // DTBO: novatek,lcd_id-gpio = <&tlmm 0x1a 0x00>

// Novatek NT36532E protocol (see docs/drivers/native-novatek-touch.md and NvtProtocol.h). The panels
// this controller drives are 60 by 40 nodes; the protocol layer owns the framing.
#define TM_TOUCH_NODES_X    60u
#define TM_TOUCH_NODES_Y    40u

static inline uint32_t tm_tlmm_ctl_reg(unsigned gpio) {return (uint32_t)(gpio * TM_TLMM_STRIDE);}
static inline uint32_t tm_tlmm_io_reg(unsigned gpio) {return (uint32_t)(gpio * TM_TLMM_STRIDE + 4u);}

#endif
