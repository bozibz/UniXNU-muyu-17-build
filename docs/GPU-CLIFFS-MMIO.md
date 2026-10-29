# muyu（cliffs / SM8635）GPU MMIO 映射 —— 权威表

> **为什么有这份文档**：`HANDOFF-2026-10-29` §2.4/§5 里「GPUCC/GCC 偏移 7/8 与上游 SM8650 一致」
> 的结论**是错的**。cliffs 的 GPUCC 与 SM8650 **不是同一套寄存器布局**（例如 `gpu_cc_ahb_clk`
> 在 cliffs 是 **0x90c8**，SM8650 是 0x911c）。照抄 sm8650 / sheng 的值去读写，会打到**别的时钟、
> 别的 GDSC**，而且读一个时钟关着的块会挂总线，不会有报错。
>
> 全部数值由 `scripts/check-muyu-gpu-mmio.py` 对着三个第一方来源**逐条断言**，任何漂移都会让脚本
> 非零退出。本文不手抄签名，数字都可以被脚本重放。

## 0. 来源与证据

| 来源 | 具体文件 / 标识 | sha256 |
| --- | --- | --- |
| 厂商内核源码（cliffs 时钟驱动） | `MiCode/Xiaomi_Kernel_OpenSource` 分支 **`muyu-v-oss`** 的 `drivers/clk/qcom/gpucc-cliffs.c` | `fdeacf41b361b8e589674aea99928397f7e9262a88913d8359a9f39f5a305a97` |
| 同上（GCC） | 同分支 `drivers/clk/qcom/gcc-cliffs.c` | `7d19636bc7e89f4837f5e044436d1bbae854f7ac927289f729867c0290e3e544` |
| 设备上实际加载的模块 | `/vendor/lib/modules/gpucc-cliffs.ko`（**未 strip，带 BTF**） | `ce445ae65d43838e2d196c44dfa825ae3b933d0055d20860128e7c560a74cbcc` |
| 设备自己的 devicetree | `fw/stock/dtb_3.dtb`（= 设备 live DT 的同一份） | `c0974ed235ad3abf883e87a5dcc5b84cd54d00890564e3bbd0df8c0a9ae5dfc4` |
| 运行中的 Android | `logs/2026-10-29-gpu/clk-summary-gpu.txt`、`logs/2026-10-29-gpu/dmesg.txt` | — |

厂商源码与设备模块是**同一套代码**：把模块 `.data` 里每个 `struct clk_branch` 的
`halt_reg`/`enable_reg` 字段读出来，与源码逐条相同（字段偏移用模块自带的 BTF 确认：
`clk_branch{+0x0 hwcg_reg, +0x4 halt_reg, …, +0x18 clkr}`、`clk_rcg2{+0x0 cmd_rcgr, …}`、
`clk_alpha_pll{+0x0 offset, …}`）。工具：`tools/elf-const-scan.py`、`tools/btf-dump.py`。

## 1. ⚠ 一个必须记住的坑：DT 节点名是 SM8650 的残留

muyu 的 DT 里有这些节点：

```
/soc/qcom,gdsc@3d99108   reg = <0x3d9908c 0x4>   regulator-name = "gpu_cc_cx_gdsc"
/soc/qcom,gdsc@3d9905c   reg = <0x3d99034 0x4>   regulator-name = "gpu_cc_gx_gdsc"
/soc/syscon@3d99168      reg = <0x3d990a0 0x4>   （gpu_cc_cx_gdsc 的 hw-ctrl-addr 指向它）
```

**节点名里的 unit-address 是过时的**（照着 SM8650 抄的），真正的地址在 **`reg`** 里。
用节点名算偏移会得到 0x9108 / 0x905c —— 而 0x9108 在 cliffs 上是
`gpu_cc_cx_accu_shift_clk` 的 CBCR，写进去等于去动一个时钟的使能位。

**规则：一切 GPU 寄存器地址从 `reg`/源码取，不从来名字取。**

## 2. GPUCC（base `0x03d90000`，size `0xa000`）

### 2.1 时钟（源码 `gpucc-cliffs.c`；`halt_reg == enable_reg`，bit0 使能）

| 时钟 | 寄存器 | 备注 |
| --- | --- | --- |
| `gpu_cc_pll0` | `0x0000` | 510 MHz 配置（`l=0x1a, alpha=0x9000`）|
| `gpu_cc_pll1` | `0x1000` | 440 MHz 配置（`l=0x16, alpha=0xeaaa`）；GMU 220 MHz 由它 /2 |
| `gpu_cc_xo_clk_src` | `0x9010` | RCG |
| `gpu_cc_xo_div_clk_src` | `0x9028` | divider |
| `gpu_cc_demet_div_clk_src` | `0x902c` | divider |
| `gpu_cc_freq_measure_clk` | `0x9008` | branch |
| `gpu_cc_ahb_clk` | **`0x90c8`** | branch（SM8650 是 0x911c）|
| `gpu_cc_crc_ahb_clk` | `0x90cc` | branch |
| `gpu_cc_sleep_clk` | `0x90dc` | branch, VOTED |
| `gpu_cc_cx_gmu_clk` | **`0x90e4`** | branch, AON（SM8650 是 0x913c）|
| `gpu_cc_cxo_clk` | `0x90f0` | branch（SM8650 是 0x9144）|
| `gpu_cc_hub_cx_int_clk` | `0x90f4` | branch, AON（SM8650 是 0x9148）|
| `gpu_cc_cx_ff_clk` | `0x90f8` | branch |
| `gpu_cc_memnoc_gfx_clk` | `0x90fc` | branch |
| `gpu_cc_cx_accu_shift_clk` | `0x9108` | branch（**不是 GDSCR**）|
| `gpu_cc_gmu_clk_src` | **`0x9168`** | RCG（SM8650 是 0x9318），CFG 在 +4 |
| `gpu_cc_mnd1x_0_gfx3d_clk` | `0x9158` | branch |
| `gpu_cc_mnd1x_1_gfx3d_clk` | `0x915c` | branch |
| `gpu_cc_hub_aon_clk` | `0x9198` | branch |
| `gpu_cc_hub_clk_src` | **`0x919c`** | RCG（SM8650 是 0x93ec）|
| `gpu_cc_ff_clk_src` | `0x91b8` | RCG |
| `gpu_cc_hlos1_vote_gpu_smmu_clk` | `0x7000` | VOTED，**与 SM8650/8550 相同** |

**没有 CPU 侧的 `gpu_cc_gx_gfx3d_clk` 分支**（SM8650 有，0x90a8）。A735 的 GX 时钟/电源由
GMU 通过 `gx_clkctl_gx_gdsc`（`0x3d68024`，在 GMU 窗口里）控制，`gpu_cc_gx_gfx3d_clk` 只在
`debugcc-cliffs` 里作为 `measure_only` 存在。⇒ 移植时**不能**照 sheng 的 hw_init 去写
gfx3d 的 CBCR，那一位在这里是 `mnd1x_*`/别的时钟。

### 2.2 复位（BCR，源码 `gpu_cc_cliffs_resets`；GDSCR = BCR + 4）

| 名称 | BCR | 说明 |
| --- | --- | --- |
| `gpu_cc_xo_bcr` | `0x9000` | |
| `gpu_cc_gx_bcr` | `0x9030` | ⇒ **GX GDSCR = 0x9034** |
| `gpu_cc_cx_bcr` | `0x9088` | ⇒ **CX GDSCR = 0x908c** |
| `gpu_cc_gfx3d_aon_bcr` | `0x910c` | |
| `gpu_cc_rbcpr_bcr` | `0x9118` | |
| `gpu_cc_gmu_bcr` | `0x9164` | ⇒ GMU block reset |
| `gpu_cc_acd_bcr` | `0x9180` | |
| `gpu_cc_cb_bcr` | `0x918c` | |
| `gpu_cc_fast_hub_bcr` | `0x9194` | |
| `gpu_cc_ff_bcr` | `0x91b4` | |
| `GPU_CC_FREQUENCY_LIMITER_IRQ_CLEAR` | `0x9538` | 不是 BCR，是中断清除 |

### 2.3 电源域（GDSC，来自 DT 的 `reg`）

| 域 | 绝对地址 | GPUCC 偏移 | 附加 |
| --- | --- | --- | --- |
| `gpu_cc_cx_gdsc` | `0x03d9908c` | `0x908c` | `gds_hw_ctrl` = `0x03d990a0`（偏移 `0x90a0`）；`qcom,clk-dis-wait-val = 8` |
| `gpu_cc_gx_gdsc` | `0x03d99034` | `0x9034` | `qcom,support-cfg-gdscr`；`clocks = <&gcc 0x1d>`（ahb_clk）|
| `gx_clkctl_gx_gdsc` | `0x03d68024` | GMU 窗口 +0x24 | **DT 里 status = "disabled"**，由 GMU 固件自己用 |

其它 DT syscon（都按 `reg` 取，节点名同样过时）：

```
gpu_cc_gx_domain_addr    0x9504     (syscon@3d99504)
gpu_cc_gx_sw_reset       0x9030     (syscon@3d99058 → reg 0x3d99030)
gpu_cc_gx_acd_reset      0x9180     (syscon@3d99358 → reg 0x3d99180)
gpu_cc_gx_acd_iroot_reset 0x958c    (syscon@3d9958c)
```

### 2.4 PLL（`lucid_ole` 寄存器组，sheng 的 `gpb_pll_configure` 偏移可直接用）

```
pll0 @0x0000 : l=0x1a alpha=0x9000   →  510 MHz   （live clk_summary: 510000000）
pll1 @0x1000 : l=0x16 alpha=0xeaaa   →  440 MHz   （live: 440000000；gmu_clk_src=PLL1/2=220 MHz）
config_ctl_val=0x20485699  hi=0x00182261  hi1=0x82aa299c  test_hi=3 test_hi1=0x9000 test_hi2=0x34
```

## 3. GCC（base `0x00100000`）

| 用途 | 寄存器 | 说明 |
| --- | --- | --- |
| `gcc_gpu_cfg_ahb_clk` | **`0x71004`** | 没有 clk_branch 定义；`gcc_cliffs_probe` 里 `regmap_update_bits(…, BIT(0), BIT(0))` **常开** ⇒ XNU 必须先确认 bit0=1 再碰 GPUCC |
| `gcc_gpu_memnoc_gfx_clk` | `0x71010` | VOTED |
| `gcc_gpu_snoc_dvm_gfx_clk` | `0x71018` | |
| `gcc_ddrss_gpu_axi_clk` | **`0x7115c`** | （sheng 用 0x71154，cliffs 不同）|
| GPLL0 GPU 投票 | `0x52000` bit15 / bit16 | `gcc_gpu_gpll0_clk_src` / `_div_clk_src`，**与 sheng 相同** |

## 4. 运行中的 Android 实测（同日取证）

`clk-summary-gpu.txt`（debugfs 由 root 现场 mount）：

```
gpu_cc_pll0 510 MHz, gpu_cc_pll1 440 MHz
gpu_cc_gmu_clk_src 220 MHz → gpu_cc_cx_gmu_clk 220 MHz (Y, used by qcom,gmu@3d69000)
gpu_cc_hub_clk_src 200 MHz → gpu_cc_hub_cx_int_clk 200 MHz (Y)
gpu_cc_ahb_clk 200 MHz (N, consumers: kgsl-3d0 / kgsl-smmu)
gpu_cc_hlos1_vote_gpu_smmu_clk (consumer 3da0000.kgsl-smmu)
```

kgsl sysfs（root）：`num_pwrlevels=11`，`freq_table_mhz = 1100 1000 950 900 835 736 684 633 500 353 255`，
`default_pwrlevel=10`，`gpuclk=255 MHz`，`qcom,ubwc-mode=4`，`qcom,initial-pwrlevel=10`，
Adreno chip id **`0x43030b00`**（DT `qcom,chipid`）。

`dmesg`（`dmesg.txt`）：

```
arm-smmu 3da0000.kgsl-smmu: SMMUv2, stage 1, coherent table walk, 24 register groups,
                            22 context banks (0 stage-2 only), 48-bit VA -> 36-bit IPA,
                            page sizes 0x61311000, preserved 0 boot mappings
adreno-gen7-gmu 3d68000.qcom,gmu:  (iommu group 38)
kgsl-3d 3d00000.qcom,kgsl-3d0: bound 3da0000 (gfx3d_user/secure), 3d68000.qcom,gmu (gen7_gmu_component_ops)
```

## 5. 与 sheng 的 `GpuPowerBoard.h` 逐条差异（移植清单）

| sheng 常量 | sheng 值 | muyu 应为 | 差异 |
| --- | --- | --- | --- |
| `GPB_CC_CX_GDSCR` | 0x9108 | **0x908c** | ✗ |
| `GPB_CC_GX_GDSCR` | 0x905c | **0x9034** | ✗ |
| `GPB_CC_AHB_CLK` | 0x911c | **0x90c8** | ✗ |
| `GPB_CC_CX_GMU_CLK` | 0x913c | **0x90e4** | ✗ |
| `GPB_CC_SMMU_VOTE_CLK` | 0x7000 | 0x7000 | ✓ |
| `GPB_CC_CX_GDS_HW_CTRL` | 0x953c | **0x90a0** | ✗ |
| `GPB_CC_GMU_RCGR` | 0x9318 | **0x9168** | ✗ |
| `GPB_CC_HUB_RCGR` | 0x93ec | **0x919c** | ✗ |
| `GPB_CC_CXO_CLK` | 0x9144 | **0x90f0** | ✗ |
| `GPB_CC_HUB_CX_INT_CLK` | 0x9148 | **0x90f4** | ✗ |
| `GPB_CC_CXO_AON_CLK` | 0x9004 | **不存在**（cliffs 的 CxoAon 只在 debugcc 里）| ✗ |
| `GPB_CC_DEMET_CLK` | 0x900c | 分支不存在；只有 `demet_div_clk_src` @0x902c | ✗ |
| `GPB_CC_PLL0/PLL1` | 0x0 / 0x1000 | 相同 | ✓ |
| `GPB_CC_PLL1` L/alpha | 0x16/0xeaaa | 相同 | ✓ |
| `GPB_CC_PLL0` L/alpha | 0x1e/0xbaaa | **0x1a/0x9000** | ✗ |
| `GPB_GCC_GPU_CFG_AHB` | 0x71004 | 相同 | ✓ |
| `GPB_GCC_GPU_MEMNOC_GFX` | 0x71010 | 相同 | ✓ |
| `GPB_GCC_GPU_SNOC_DVM_GFX` | 0x71018 | 相同 | ✓ |
| `GPB_GCC_DDRSS_GPU_AXI` | 0x71154 | **0x7115c** | ✗ |
| `GPB_GCC_PLL_VOTE` | 0x52000 (bit15/16) | 相同 | ✓ |

**结论**：14 项里 6 项相同、8 项必须改（另有 2 个 cliffs 上不存在的分支）。原「7/8 与 sm8650 一致」
的判断在 cliffs 上**不成立**，原因就是它比较的是 SM8650 而不是 cliffs 自己的驱动。

## 6. 这些数还要不要再上机「分级读寄存器」？

**不需要为“确认偏移”再读。** 偏移现在有三个独立来源互证：厂商驱动源码、设备上加载的同一份模块
（BTF 字段偏移 + `.data` 值）、DT 的 `reg`。Android 能正常跑 GPU，就是这套偏移在硅上成立的
**行为证据**。仍然需要上机验证的是**语义**，不是地址：

1. XNU 侧自己把 GPLL0 投票 + `gcc_gpu_cfg_ahb` + CX GDSC 打开之后，`gpu_cc_smmu_vote`
   的 `CLK_OFF` 是否按预期清零（这是第一次真实写 cliffs 的 GPUCC）；
2. CX GDSC 的 `gds_hw_ctrl`(0x90a0) 轮询是否与 `gdsc.c` 语义一致（VOTABLE + clk_dis_wait 8）；
3. GMU 冷启动后 HFI 是否响应。

这几步都要求**分级、可回滚**，且一次只写一个域。`GpuPowerBoard.h` 的 mode 1/2/3 结构正好是
这个形状，只需换成 cliffs 的常量（§5）。

## 7. 复现

```bash
# 1) 断言本文所有寄存器 = 厂商源码 + DT
python3 UniXNU-muyu/scripts/check-muyu-gpu-mmio.py --json UniXNU-muyu/build/gpu/muyu-gpu-mmio.json

# 2) 想看某个常量的出处（模块 .data 里哪个符号持有它）
python3 tools/elf-const-scan.py fw/android-harvest/gpucc-cliffs.ko 0x90c8 0x90e4 0x9168

# 3) 想看 struct clk_branch 的字段偏移（模块自带 BTF，需要 base BTF）
python3 tools/btf-dump.py fw/android-harvest/gpucc-cliffs.ko \
    --base fw/android-harvest/vmlinux.btf clk_branch clk_rcg2
```

`fw/android-harvest/` 里的模块、base BTF（`/sys/kernel/btf/vmlinux`，5.4 MB）与厂商源码是
**取证副本**，`fw/` 不进 git；本文的 sha256 固定了它们的身份。

## 8. 运行中 Android 的实时快照（2026-10-29 19:5x，刷回 iPadOS 前一次性取证）

采集脚本 `tools/harvest-android.sh`，副本与清单在 `fw/android-harvest/`（含 README）。

### 8.1 可用的数字（已进 `fw/android-harvest/sysfs/`）

- 时钟：`gpu_cc_pll0` 510 MHz、`gpu_cc_pll1` 440 MHz、`gpu_cc_gmu_clk_src`/`cx_gmu_clk` 220 MHz、
  `hub_clk_src`/`hub_cx_int`/`ahb` 200 MHz；消费者 = `qcom,gmu@3d69000`、`3d00000.qcom,kgsl-3d0`、
  `3da0000.kgsl-smmu`。
- 频率档位 11 个：`1100 1000 950 900 835 736 684 633 500 353 255` MHz；`default_pwrlevel=10`。
- kgsl `globals` 给出 **GMU 内存布局**：SCRATCH 4096、KERNEL/USER PROFILING 各 4096、
  GMU RB 32 KiB ×4、SMMU INFO 4096、CSW PRIV 8,994,816 ×2、COUNTER 16384、PREEMPTION 4096
  —— 阶段 1 之后给 GMU 分配 IOVA 时要照着这个尺寸表来（`ShengGmuConfig` 里的
  icache/dummy/debug/log/hfi 尺寸是 sheng 的，muyu 要按这张表核对）。
- regulator/interconnect 的投票都是 0（GPU 空闲、屏灭），没有参考价值；**要在有 3D 负载时抓**。

### 8.2 ⚠ 两个陷阱（都是当天实测）

1. **整块 dump GPUCC 寄存器会把设备读挂。** `/sys/kernel/debug/regmap/<gpucc>/registers` 的
   range 是 `0–0x9988`，全量遍历**两次都挂**（一次停在 0x0cc8、一次走 debugcc 的 dummy 视图停在
   0x332c），设备热复位后自愈。⇒ **禁止整块遍历时钟控制器寄存器**；这与 `GpuPowerBoard.h`
   的"读时钟关着的块会挂总线而不是报错"完全一致，也让"分级读"从建议变成硬规则。
   安全做法：只读 GDSC 那种 range 0–8 的单寄存器 regmap。
2. **`regmap/.../registers` 在 cache-backed regmap 上返回缓存值。** 打开
   `force_rail_on/force_clk_on` 后 GDSC 读数完全不变化（`0x00228801`/`0x00222801`），
   说明读到的是 regcache 不是硅片。**不要用 regmap debugfs 证明硬件状态**；
   用 `clk_summary` 的 rate/enable + `regulator_summary` 的使能计数。
   `vendor-source/gdsc-regulator.c` 确认语义与上游一致：
   `PWR_ON=BIT(31)`、`SW_COLLAPSE=BIT(0)`、`POWER_UP_COMPLETE=BIT(16)`。

### 8.3 这次之后的设备状态

两次挂板都是**热复位**、Android 正常回来（没有进 crash dump），说明读操作没有留下持久损伤。
取证结束后设备仍是 Android + root，可以随时刷回 iPadOS 根（代价是 Android 数据）。
