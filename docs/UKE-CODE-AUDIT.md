# uke 代码资产审计（2026-11-08）

对象：`~/pad7pro-xnu/UniXNU-uke`（小米平板 7 / SM7675 / 12 GB / 256 GB UFS）
结论一句话：**uke 的核心资产不是驱动，是三种工程方法——离线验证、探针化、结构化证据链。**
它的驱动代码我们可以抄，但那只是最表层的东西，抄了也快不起来。

---

## 1. 资产规模

| 项 | 数量 |
| --- | --- |
| 设备仓库源文件（C/C++/H/MM） | 240 个，19,081 行 |
| `package/` 探针与组件 | 102 个文件（31 `.c` / 26 `.cpp` / 20 `.mm` / 19 `.h`） |
| `scripts/` 构建脚本 | 126 个（几乎每个探针配一个 `build-*.py`） |
| `configs/` defconfig | 9 个（`bootstrap` / `cpu` / `full` / `full-plus` / `gpu` / `gpu-userland` / `userland` / `ipados27`） |
| `patches/xnu-12377/` | 小型系列，一个补丁一个假设 |
| `docs/` | 11 篇（PROGRESS 为主，另有 GPU/DPU/TOUCH/RECOVERY 专题） |
| `porting.toml` | 420 行，12 个移植领域的结构化记录 |

内核本体不在这个仓库里（在 `UniXNU` + `feeds` 拉取的 kit 中），所以 19,081 行全是**设备相关代码**，没有一行浪费。

---

## 2. 三种工程方法（真正值得抄的东西）

### 2.1 离线验证：18 个 `Offline*.mm`，在 Mac 上跑，不用刷机

`package/` 里有 18 个 Objective-C++ 文件，全部以 `Offline` 开头：

```
OfflineGOPComposition.mm     OfflineGOPPresentation.mm    OfflineGOPResources.mm
OfflineLegacySwap.mm         OfflineMetalAPIDiagnostic.mm OfflineMetalDepthStencil.mm
OfflineMetalPluginProbe.mm   OfflineMetalPrivateTexture.mm
OfflineMetalProcesses.mm     OfflineMetalSharedEvents.mm  OfflineMetalSharedSurfaces.mm
OfflineMetalVertexInputs.mm  OfflineRenderBoxDevice.mm    OfflineRenderBoxDraw.mm
OfflineGPUBrokerProbe.cpp    OfflineGPUTransportDiagnostic.cpp  OfflineGPUWire.h
OfflineMetalAddon.h
```

`.mm` 意味着它们直接在 macOS 上编译、链接真实的 Metal / CoreAnimation 框架跑。
也就是说：**表结构、字位序、共享事件、纹理格式、surface 生命周期这些逻辑，uke 全在主机上验完了才上机。**

这解释了为什么 uke 第 4 天就能把画面点起来：它上机验证的只是"寄存器路径通不通"，不是"逻辑对不对"。
我们每一轮都在同时验证逻辑和硬件，所以每轮只能得到一比特信息。

### 2.2 探针化：22 个 `*Probe`，一个假设一个单元

`package/` 里 22 个文件名带 `Probe`，按子系统分：

| 子系统 | 探针 |
| --- | --- |
| 启动 | `BootProgress`(5 文件) / `BootLatency`(4) / `BootUiTrace`(3) / `BootProcessProbe` / `BootFaultProbe` |
| 显示 | `LegacyDisplayProbe` / `DpuProbe` / `MobileGraphicsProbe` |
| GPU | `GpuPowerProbe` / `AdrenoPM4Probe` / `AdrenoFirmwareProbe` / `AdrenoCompilerProbe` |
| 输入 | `TouchProbe` / `HidInputProbe` |
| 存储 | `SourceStorage` / `SourceRootBinding` / `SourceRootPolicies` / `UfsDpuStubs` |
| 平台 | `ArmSmmu4KProbe` / `ShengGmuCommand` |

每个探针 = 一个 `.c/.cpp` + 一个 `build-*.py` + 一到四行 board 参数（`muyu-xxx=N` 这种模式）。
**探针是常驻的**，不删；出问题时换参数重跑，不用重编内核。

对比我们：探针写在 `muyu_usb_debug.c` 里，和内核一起编，改一行就要重刷 100 MB vendor_boot。
（该驱动已于 2026-11-08 随 USB 调试通道一起删除，见 `ABANDONED-CHANNELS.md`；这条对比仍然成立，
只是现状从「探针太重」变成了「没有常驻探针」。）

### 2.3 结构化证据链：`porting.toml`

12 个领域，每个都长这样：

```toml
[[area]]
id = "display"
status = "device-verified"          # device-verified / host-verified / implemented
components = [...]                   # 用到的 kit
paths = [...]                        # 涉及的文件
inputs = [...]                       # 硬件事实、镜像 sha、DTB 偏移
configuration = [...]                # 具体的内核参数组合
procedure = [...]                    # 怎么复现
findings = [...]                     # ★ 一条一条的技术结论，含失败与根因
completion = [...]                   # 什么算通过
limits = [...]                       # 什么没验证
next_action = [...]
evidence = [ { kind, reference, result, command, revision } ]
```

现状：**9 个 device-verified，2 个 host-verified，0 个 implemented**（没有"应该能用"这种状态）。

`findings` 是这份文件最值钱的部分——它把每一次失败和根因都留下来了。例如 display 领域里：

- `swap accepted=0 reason=2 surface=0` 的真因不是呈现失败，而是**没设 `offline-legacy-base-present=1`**，`presentBase` 第一行就返回了，根本没走到日志。
- 点起来几秒后变黑屏的真因，是**空事务回落到纯黑的 default surface**，不是 panic。
- 闪烁的真因是 `dpuSlot` 轮转选到了正在被扫描的那个槽，把 IOVA 重映射到新 surface，管道还在取旧帧。

这些结论如果只写在聊天记录里，两周后就没人记得了。`porting.toml` 让它们可检索、带 revision、可复现。

**muyu 没有这个文件。** 我们有的是 29 万字的 `HANDOFF-*.md`——信息全在里面，但没有结构，要找一个结论得 grep 半天，而且没有 status 字段区分"验证过"和"应该能用"。

---

## 3. 12 个领域的验证状态

| id | 状态 | 一句话 |
| --- | --- | --- |
| configuration | host-verified | format-2 patch series，冷树可重放 |
| firmware | device-verified | ukePkg，3 MiB FD @ 0xa7100000，DarwinBoot |
| cpu | device-verified | **8 核全起，`sheng-smp=1` + `armcpu-spi-ipi=1`** |
| memory | device-verified | owned 4.75 GiB，high bank 静态映射，jetsam 不再触发 |
| storage | device-verified | ShengUfs + VerifiedRoot，2,347,008 块全量校验 |
| display | device-verified | GOP + DPU 直扫 + vsync 120 |
| input | device-verified | Novatek NT36532 + Geni SPI + HID workloop |
| gpu | device-verified | Adreno740 gen7_11_0 + Metal 用户态 |
| network / audio / power / packaging / diagnostics | 见文件 | diagnostics 是 device-verified |

---

## 4. 关于单核，uke 的原文

这是上一轮那个问题的直接答案，`porting.toml` 的 `cpu` 领域原文：

> `configuration` = "Kernel arguments **sheng-smp=1 armcpu-spi-ipi=1** armcpu-diagnostics=1."
> `findings` = "Single-CPU runs are stable past t=877s; **SMP died around t=117s in early runs (SMP/IPI or UFS concurrency), mitigated by the SPI-IPI path and watchdog policy**."
> `limits` = "Long-run SMP stability under UFS load is still being watched."
> `next_action` = "**Keep the single-CPU fallback argument documented in the record for bisecting resets.**"

读法是：单核在 uke 那里是**回退方案和 bisect 工具**，不是目标形态。他们撞到 SMP 在 t≈117 s 死，然后**用 SPI-IPI 路径 + watchdog 策略把 SMP 修好了**，最终验收版本跑的是 8 核。

我们的镜像里是 `sheng-smp=0` + `armcpu-spi-ipi=0`——**两个都关着**，而且从来没有做过"只开 smp、关 spi-ipi"那个隔离实验（§6.44 写了计划，没执行）。

所以不是"我们选了单核"，是**我们把别人的回退方案当成了目标配置，并且跳过了让它回退的那一步修复**。

---

## 5. 可以直接搬过来的六项（按优先级）

### P0 — panic 日志跨重启保留

`porting.toml` diagnostics 领域：

> "Embedded panic log at **0xFF800000/0x380000** read by the menu **before XNU clears it**."

机制：panic 时把文本写进预留 pram，下次开机 **UEFI 菜单在 XNU 覆盖它之前读出来显示**。
配合 `scripts/xnu-panic-screen.c`（panic 屏，自带 vendored `Font.h`，scale 3 只画消息）和 `package/UkeLastLog.c`。

价值：这一条直接解决我们当下的痛点。u127 的心跳只能告诉我们"还活着吗"，panic 屏 + 跨重启日志能告诉我们"上次为什么死"。而且完全不依赖 USB、不依赖 Linux、不依赖任何未通的驱动。

### P0 — `UkeLastLog` 的日志环 + 写盘

`package/UkeLastLog.c`，配 `sheng-log=1`。日志进内存环，落 userdata 尾部窗口，用 TWRP 或 Linux 事后只读 dump 走（`docs/RECOVERY-LOG.md` 里有完整的 `dd if=/dev/block/by-name/userdata bs=4096 skip=… count=8192` 命令）。

这就是 sheng 那套"先落盘再取走"，uke 也用了。**不追求实时。**

### P1 — boot-progress 分段条

`boot-progress=2` + `BootProgress*`（5 个文件）。把启动分成阶段，每阶段一个屏幕上的条。不需要读日志就知道卡在哪一段。

### P1 — `DpuProbe` + `check-dpu-probe-host.py` 的主机门禁

`scripts/check-dpu-probe-host.py`（M1 gate）：DTS 锚点、原厂寄存器定义、`NativeUfs` 窗口符号、编译出的 `DpuProbe.o` 全过才允许上机；故意改错一个常量会变红。

这是"上机前先在主机上挡掉一类错误"的范式，我们一个都没有。

### P2 — `patches/xnu-12377/` 的小补丁系列

一个补丁一个假设：`uke-vm-init-checkpoints` / `uke-ignition-sysctl-trace` / `uke-image4-service-trace` / `uke-deferred-video-kprintf` / `zzzzz-sheng-contig-trace` / `zzzzz-sheng-ioconnect-trace`。

对比我们：内核树是一个巨大的未提交工作区（`git status` 几十个 M），u128–u156 的改动全糊在一起，没有一个是"一个假设一个补丁"。

### P2 — `porting.toml` 本身

不是代码，是结构。给 muyu 建一份，12 个领域，按 uke 的 schema 填。填的过程本身就是一次审计——你会发现很多"以为验证过"的其实只是"应该能用"。

---

## 6. 差距表

| 维度 | uke | muyu | 后果 |
| --- | --- | --- | --- |
| 逻辑验证 | 18 个 Offline 组件，主机上跑 | 无 | 每轮上机同时验逻辑+硬件 |
| 探针 | 22 个，常驻，参数切换 | 内嵌内核，改即重刷 | 一轮 5–20 分钟 |
| 诊断记录 | `porting.toml`，带 status/revision | 29 万字 HANDOFF，无结构 | 结论易丢、易重复踩 |
| panic 可见性 | 预留 pram + 菜单回读 | 靠屏幕最后一行 | 死因丢失 |
| 日志 | 环形缓冲 + 落盘 + 事后 dump | 想做成 USB 实时流（29 轮失败） | 通道 = 被调对象 |
| SMP | 8 核，SPI-IPI 修好 | 单核 + spi-ipi 全关 | 一自旋即全停 |
| 内核改动 | 6 个独立小补丁 | 一个巨大工作区 | 无法 bisect |

---

## 7. 建议的下一步顺序

1. 跑 u127 拿到用户态卡点（已在做）。
2. **同时**照第 5 节 P0 做两件事：panic 屏进预留 pram + 菜单回读；`UkeLastLog` 式日志环落盘。这两件都不碰 USB，也不依赖任何未通的组件。
3. 建 `UniXNU-muyu/porting.toml`，先把 12 个领域按现状填一遍（多数会是 `implemented`，那本身就是结论）。
4. 把内核工作区拆成 `patches/xnu-12377/` 系列。
5. 只有在 1–4 完成后，再决定要不要碰 SMP 或多核。
