# muyu GPU / Metal 移植方案（2026-10-28 调研）

> 目标：把 ~1.4 fps 的 CPU 逐帧拷贝换成真正的 GPU 合成（官方 `Adreno740` + `AdrenoMetal` 路线）。
> 本文只记录**已核实的事实**和**分阶段计划**；没有一行是"应该能行"。

## 1. 结论摘要

- muyu 的 GPU 是 **Adreno 735（Gen7，`qcom,adreno-gpu-gen7-11-0`）**，
  官方组件 `Adreno740` 是给 **A740** 的 ⇒ **不能直接用，需要一层 A735 后端**。
- 好消息：muyu 是 **4K 页**（`build-xnu-4k-baseline.sh`），与上游 sheng 一致；
  `ios-vermeer-lab/GPU-PORT.md` 里那类"16K 页 vs GPU 4K SMMU 粒度"的问题**在这里不存在**。
- 坏消息：**GPU 固件不在本地**（`a735_sqe.fw` / GMU 固件 / ZAP 都在安卓的
  `vendor`、`vendor_dlkm` 分区里，我们没有这两个镜像），而它们是第一阶段的前提。

## 2. 现状（已核实）

| 项 | 事实 |
| --- | --- |
| 显示路径 | `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`（41 KB），`kickOnce()` 里用 `memcpy` 逐帧拷贝 |
| 帧率 | ~1.4 fps（HANDOFF-2026-10-27 §H.1 用 swap 计数实测：每分钟 +60~80 帧） |
| GPU 加速 | **完全没有**。没有 GMU、没有 CP、没有 Adreno 驱动被编进 muyu 内核 |
| 现有 GPU 探针 | `kits/AdrenoFirmware`、`kits/AdrenoPM4` 已在本机构建（`build-adreno-firmware-probe.py` 等），只是**只读探针** |
| GPU scanout 接口 | muyu 的显示驱动里**没有任何** GPU scanout 引用；上游 `NativeMetal` 有 `GpuScanoutClient.h` / `GpuScanoutPresenter.h` |

## 3. 官方要求（`unixnu.com/porting/gpu`，2026-10-28 读）

按文档顺序，四项硬前提：
1. **GPU 代际 + OS ABI 必须匹配**："For another GPU generation, implement the matching kernel backend
   and compiler support before selecting its frontend." ⇒ A735 需要自己的后端。
2. **电源与固件输入**：`NativeAdrenoGmuPlatform` 的 bind/services、映射窗口、定时器、固件；
   并移植 `ShengGmuVotes.c` / `ShengGmuVotesSource.h` 那套 **OPP 与带宽策略**到目标板。
3. **内存与命令提交**：`ArmSmmu4K` 的 context/stream/地址宽度/缓存维护；CP 初始化、ring 前进、fence 完成。
4. **构建与 Metal**：`build-turnip-darwin.py` → `build-adreno-metal-plugin.py` → `add-gpu-plugin-data.py`，
   产物作为 `--data-ramdisk` 打 payload。

文档给出的分阶段"完成标准"很明确：
GMU 启动并响应 HFI → 有界提交后 fence 前进 → 渲染一张已知图像 → 再做 UI。

## 4. muyu 的硬事实

| 项 | 值 | 来源 |
| --- | --- | --- |
| GPU 型号 | **Adreno 735** | `fw/muyu_dt.txt:7660` `qcom,gpu-model` |
| compatible | `qcom,adreno-gpu-gen7-11-0`, `qcom,kgsl-3d0` | 同上 `:7668` |
| GPU 寄存器 | `kgsl_3d0_reg_memory`, `rscc`, `cx_dbgc`, `cx_misc`, `qdss_*` | `:7665-7666` |
| CPU 页大小 | **4K**（与上游 sheng 同） | `scripts/build-xnu-4k-baseline.sh` |
| SMMU | apps-smmu @0x15000000，SID 0x40 已知有 identity 路由（USB 用） | HANDOFF-2026-10-28 §14.2 |

## 5. 本地已具备 / 缺失

**已具备（都已 checkout，可直接读）**
- `UniXNU/drivers/NativeAdreno`（792 KB）：GMU、CP、ring、SMMU、user client、A740 硬初始化
- `UniXNU/drivers/NativeMetal`（1.0 MB）：Metal 前端、Turnip 桥、scanout presenter
- `UniXNU/kits/AdrenoFirmware`（GMU/HFI/startup/votes）、`kits/AdrenoPM4`、`kits/ArmSmmu4K`
- 脚本：`build-native-adreno-kits.py`、`build-adreno-compiler-host.py`、
  `prepare-sheng-gmu.py`、`prepare-sheng-gmu-votes.py`、`check-sheng-gmu-startup.py`、
  `add-gpu-plugin-data.py`、`audit-*` 一组

**已取得（2026-10-29，阶段 0 完成）**
A735 固件已从设备上抽出，落在 `fw/gpu-firmware/`：

| 文件 | 大小 | 来源 | sha256 |
| --- | --- | --- | --- |
| `gen71100_gmu.bin` | 80,680 B | `/vendor/firmware/` | `435c7bac9a328b8a100df930b84b65b4d579e42be44785d0e60e3729291bb864` |
| `gen71100_sqe.fw` | 66,036 B | `/vendor/firmware/` | `b6df660d109504dbea5a266910b037a33d893a11c6de52f524a5fd112008c401` |
| `gen71100_zap.mbn` | 12,088 B | `/odm/firmware/` | `463ba9b7d2e3f03ac0e6fa6358bf9c886c689b43af14face6a734bf289439fb4` |

命名与 DTB 的 `compatible = "qcom,adreno-gpu-gen7-11-0"` 完全对应（`gen71100`）。
设备上还并存 `gen70900_*` 一套（另一个 GPU SKU），注意别拿错。

**取法（记录下来，换设备/换 ROM 可复用）**：`/vendor/firmware` 与 `/odm/firmware` 对 adb shell 是
SELinux 隐藏的，必须用 root：`su -c 'cp /vendor/firmware/gen71100_* /odm/firmware/gen71100_zap.mbn /sdcard/...'`
再 `adb pull`。本机用 Magisk 刷 `init_boot` 取 root（原厂镜像在 `fw/stock/init_boot.img`）。

**已取得（2026-10-29 晚，阶段 1b 收尾）**
- **GPUCC/GCC/GDSC 的权威寄存器表**：`docs/GPU-CLIFFS-MMIO.md`。cliffs 的 GPUCC 与 SM8650
  **不同布局**（`gpu_cc_ahb_clk=0x90c8`、`gpu_cc_cx_gmu_clk=0x90e4`、`gmu_clk_src=0x9168`…），
  由厂商 `muyu-v-oss` 源码 + 设备上加载的 `gpucc-cliffs.ko`（BTF）+ DT 的 `reg` 三方互证，
  断言脚本 `scripts/check-muyu-gpu-mmio.py`。
- **DT 陷阱**：`qcom,gdsc@3d99108` 这类**节点名是 SM8650 的残留**，真地址在 `reg`
  （实际 `0x3d9908c`）。旧文档里按节点名推出的 `0x9108/0x905c` 是错的。
- **运行中 Android 的实测**：11 档频率 255–1100 MHz、GMU clk 220 MHz、hub 200 MHz、
  A735 chip id `0x43030b00`、GPU SMMU：24 SMR 组 / 22 context bank / 48-bit VA→36-bit IPA /
  26 个中断、AOP cmd-db 在 `0x81c60000`+`0x20000`。

- **A735 GMU 固件容器与 sheng 相同**：`gen71100_gmu.bin` 用 `ShengGmu.c` 的三 region 划分
  解出 **15 个 block**（ITCM 0x0/0x1dc、ICACHE 0x4000/0x1381c、DTCM 0x10007f98 与 0x10007fdc），
  与加载器要求一致 ⇒ 固件加载/TCM 回读逻辑可复用，只换固件本体/chip id/寄存器常量。
  复核：`tools/gmu-firmware-plan.c`（host，不碰设备）。

**仍缺（按阻塞顺序）**
1. ~~A735 固件~~ —— **已取得，见上表**。
2. **A735 硬初始化**：现有 `NativeAdrenoA740HwInit.h` 是逐型号的寄存器序列，需要 A735 版本
   （`NativeAdrenoPwrupRegs.h`、`NativeAdrenoPwrup.cpp` 里也有 A740 常量）。
3. **板级 GMU votes / OPP / 带宽策略**：`ShengGmuVotes.c` 的 muyu 对应物。
4. **GPU 的 SID / stream、扫描输出布局、电源域**：从 muyu DT 的 `kgsl_3d0` 与 `iommus` 取。
5. **显示侧的 GPU scanout 挂接**：muyu 的 `NativeLegacyDisplay` 还没有接
   `GpuScanoutClient`/`GpuScanoutPresenter`。

## 6. 分阶段计划（每阶段都有可验证的完成标准）

| 阶段 | 内容 | 完成标准 |
| --- | --- | --- |
| **0. 取证** | 从原厂 ROM 取 `vendor`/`vendor_dlkm`，抽出 A735 固件，记录 `proprietary-files.txt` | 三个固件文件的 sha256 与来源分区都记下来 |
| **1. GMU 起来** | 板级 `NativeAdrenoGmuPlatform::bind/services` + 映射窗口 + 定时器 + 固件；`prepare-sheng-gmu.py` 改成 muyu | boot log 出现 GMU 启动与 HFI 响应 |
| **2. 内存与提交** | `ArmSmmu4K` context/stream、GPU VA 窗口、CP 初始化、ring | 一次有界提交后 fence 前进；失败能报出 request/handle/fence |
| **3. 已知图像** | 用 `AdrenoPM4` 编码一个最小绘制 | 渲染结果与预期像素一致 |
| **4. Metal + 合成** | turnip + metal 插件 + `add-gpu-plugin-data.py` 打进 data seed；显示侧接 GPU scanout | Metal 设备可见、UI 由 GPU 合成、帧率显著高于 1.4 fps |

官方文档建议的验证顺序就是 1→2→3→4，**不要跳步**。

## 7. 前置条件与风险

- **前置条件**：完整的原厂 fastboot ROM（同时用于救砖和取 GPU 固件）。
  这一步和"设备恢复"是同一件事，所以先解决设备。
- **风险 1**：A735 与 A740 的差异可能不止寄存器序列（CP 微码接口、GMU 版本）。
  若差异过大，阶段 1 就会卡住——那也是有价值的结论，能立刻止损。
- **风险 2**：无热管理（无 thermal zone、无 DVFS、`wdt=-1`）。GPU 起来后功耗会显著上升，
  **热管理必须和 GPU 同步推进**，否则有硬件风险（AGENTS §5 第 6 条）。
- **风险 3**：Metal 前端依赖 iOS 17 的 GPU/IOSurface ABI；muyu 是 4K 页、iOS 17.7.11，
  与上游 sheng 同源，这是相对有利的一面。

## 8. 现在为什么不开始写代码

阶段 0（固件）拿不到，阶段 1 就没有输入；而拿固件需要完整 ROM，也就需要先把设备救回来。
**所以正确的顺序是：恢复设备 → 拿到 ROM → 取固件 → 阶段 1。**
