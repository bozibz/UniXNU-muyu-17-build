# muyu GMU 板级映射（阶段 1 输入）

> 全部取自设备自己的 DTB（`fw/stock/dtb_3.dtb`，用 `tools` 里的 FDT 解析验证过），
> 不是从 sheng 抄的、也不是猜的。对应 `NativeAdrenoGmuPlatform::bind()` 需要的 5 个窗口。

## 1. 五个 MMIO 窗口（顺序 = `enum {ShengGmuCore, ShengGmuRscc, ShengGmuPdc, ShengGmuGpu, ShengGmuCxMisc}`）

| 索引 | `ShengGmu*` | DT `reg-names` | base | size |
| --- | --- | --- | --- | --- |
| 0 | `ShengGmuCore` | `gmu`（`/soc/qcom,gmu@3d69000`）| `0x03d68000` | `0x37000` |
| 1 | `ShengGmuRscc` | `rscc`（`/soc/qcom,kgsl-3d0@3d00000`）| `0x03d50000` | `0x10000` |
| 2 | `ShengGmuPdc` | `gmu_pdc` | `0x0b280000` | `0x10000` |
| 3 | `ShengGmuGpu` | `kgsl_3d0_reg_memory` | `0x03d00000` | `0x40000` |
| 4 | `ShengGmuCxMisc` | `cx_misc` | `0x03d9e000` | `0x1000` |

未被这 5 个窗口覆盖、但 DT 里还有的：`gmu_ao_blk_dec0` = `0x03d40000`/`0x10000`、
`cx_dbgc` = `0x03d61000`/`0x800`（调试用），以及 `qdss_*`（trace，可忽略）。

## 2. 中断（GIC SPI，DT 里是 `<0 N 4>`，N 是 SPI 号 = Linux IRQ - 32）

| 用途 | DT | SPI 号 |
| --- | --- | --- |
| GPU（`kgsl_3d0_irq`）| `0x12c` | **300** |
| GMU HFI | `0x130` | **304** |
| GMU | `0x131` | **305** |

## 3. 时钟与电源（来自 `gmu@3d69000`）

```
clock-names  "gmu_clk" "cxo_clk" "axi_clk" "memnoc_clk" "ahb_clk" "hub_clk"
clocks       gpu_cc:9, gpu_cc:11, gcc:0x12, gcc:0x20, gpu_cc:2, gpu_cc:0x14
regulator-names "vddcx" "vdd"
mboxes       aop（qcom,ipc-core）
```

`kgsl-3d0` 侧另有 `gcc_gpu_memnoc_gfx` / `gcc_gpu_snoc_dvm_gfx` / `gpu_cc_ahb` / `apb_pclk`，
以及 interconnect `gpu_icc_path`（DDR/CNOC 带宽投票）。

## 4. GPU SMMU（已解析，phandle `0x1c1`）

`gmu` 节点的 `iommus = <0x1c1 0x5 0x0>` ⇒ **stream 5**，phandle `0x1c1` 解析为：

```
/soc/kgsl-smmu@3da0000
  compatible  "qcom,qsmmu-v500" "qcom,adreno-smmu"
  reg         "base"     0x03da0000  0x40000
              "tcu-base" 0x03de6000  0x40
  #iommu-cells            2
  #global-interrupts      1
  qcom,actlr              <0x0 0x3ff 0x32b>
  interrupts              26 个（3 cells 一组），SPI 列表：
                          673, 677–687, 422, 476, 574–577, 659, 661,
                          664, 665, 666, 668, 669, 699
```

**这是独立于 `apps-smmu@15000000`（phandle `0x129`，USB 用）的第二个 SMMU 实例**，
两者都是 `qsmmu-v500` 但地址、中断、stream 空间都不同。拿 apps-smmu 的 SID 去配 GPU 会一路失败。

`ArmSmmu4K` kit 就是给 GPU 侧用的（`qsmmu-v500` + owned stream/context 生命周期），
所以这一层**不需要新写驱动**，需要的是把这组参数喂给它。

## 4.1 这些值怎么来的（可复现，不靠手抄）

`scripts/gen-muyu-gpu-platform.py` 直接读设备 DTB，生成
`build/gpu/muyu-gpu-platform.json`：

```bash
python3 UniXNU-muyu/scripts/gen-muyu-gpu-platform.py
# window 0 gmu                  0x03d68000 +0x37000
# window 1 rscc                 0x03d50000 +0x10000
# window 2 gmu_pdc              0x0b280000 +0x10000
# window 3 kgsl_3d0_reg_memory  0x03d00000 +0x40000
# window 4 cx_misc              0x03d9e000 +0x1000
# irq gpu=300 hfi=304 gmu=305   smmu /soc/kgsl-smmu@3da0000 stream=5 spis=26
```

JSON 里同时带 GPU/GMU 的时钟名、regulator 名、三个固件的 sha256、以及源 DTB 的 sha256，
所以「这份参数来自哪个 DTB」是可追溯的。**为什么要自己写：sheng 的
`prepare-sheng-gmu.py` 依赖 `scripts/data/adreno-inputs.json` 和一份 pinned 的 Linux msm 源码
缓存，这两样本仓库都没有**（只有脚本副本），跑不起来。

## 5. 与 sheng 的差异（必须逐条核对，不能照抄）

| 项 | sheng（SM8550）| muyu（SM8635 / cliffs）|
| --- | --- | --- |
| GPU | Adreno 740 | **Adreno 735**（`gen7-11-0`）|
| 固件 | `gmu_gen70200.bin` / `a740_sqe.fw` / `a740_zap.mbn` | **`gen71100_gmu.bin` / `gen71100_sqe.fw` / `gen71100_zap.mbn`** |
| GMU base | 需从 sheng 的 DT 取 | `0x03d68000` |
| HFI IRQ | 需从 sheng 的 DT 取 | **304** |

## 6. 阶段 1 剩余工作

1. ~~解析 phandle `0x1c1`~~ —— **已完成**：`/soc/kgsl-smmu@3da0000`，见 §4；
2. 把五窗口表 + 时钟/电源回调填进 muyu 的 `NativeAdrenoGmuPlatform` 调用点；
3. ~~准备固件输入~~ —— **已解决**：新增 `scripts/gen-muyu-gpu-platform.py`，
   从 DTB 生成平台 JSON（含固件 sha256），不再依赖 sheng 的 pinned Linux 源码；
4. 把 §4 的参数喂给 `ArmSmmu4K`（它已支持 `qsmmu-v500`，不需要新驱动）；
5. **完成标准**：boot log 出现 GMU 启动 + HFI 响应。
