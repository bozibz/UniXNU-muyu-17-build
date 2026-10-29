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

## 4. GPU SMMU

`gmu` 节点的 `iommus = <0x1c1 0x5 0x0>` ⇒ **stream 5**，由 phandle `0x1c1` 指向的 SMMU 实例负责。
注意这与 USB 用的 `apps-smmu@15000000`（phandle `0x129`）**不是同一个**，接手时要先把
`0x1c1` 解析成具体节点，别拿 apps-smmu 的 SID 去配 GPU。

## 5. 与 sheng 的差异（必须逐条核对，不能照抄）

| 项 | sheng（SM8550）| muyu（SM8635 / cliffs）|
| --- | --- | --- |
| GPU | Adreno 740 | **Adreno 735**（`gen7-11-0`）|
| 固件 | `gmu_gen70200.bin` / `a740_sqe.fw` / `a740_zap.mbn` | **`gen71100_gmu.bin` / `gen71100_sqe.fw` / `gen71100_zap.mbn`** |
| GMU base | 需从 sheng 的 DT 取 | `0x03d68000` |
| HFI IRQ | 需从 sheng 的 DT 取 | **304** |

## 6. 阶段 1 剩余工作

1. 解析 phandle `0x1c1` 找到 GPU SMMU 节点，取出它的 base 与 stream 配置；
2. 把上表填进 muyu 的 `NativeAdrenoGmuPlatform` 调用点（窗口映射 + 时钟/电源回调）；
3. `prepare-sheng-gmu.py` 改成读 `fw/gpu-firmware/gen71100_*`；
4. **完成标准**：boot log 出现 GMU 启动 + HFI 响应。
