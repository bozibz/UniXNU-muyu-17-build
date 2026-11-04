# GPU：对齐 uke 已验收的 iPadOS 27 基线（2026-11-04，离线）

> 用途：把"muyu 的 GPU 现在在哪、和 uke 那条被用户验收的线差什么、按什么顺序补"写死在一处。
> 本文只记录**核实过的事实**与**待办**；任何"应该能行"的推断都标成假设。

## 1. 基线：uke `c7aefb9`（"Promote the user-accepted iPadOS27 GPU build"）

uke 在 2026-11-03 把 iPadOS 27 的 GPU 栈跑成并验收（baseline CRC `90B6BC70…`）。它的 pin 表在
`feeds.toml` 里，2026-11-04 从上游直接核对（不是从记忆里抄）：

| 角色 | 仓库 | commit | 说明 |
| --- | --- | --- | --- |
| GPU 驱动（我们的 `drivers/Adreno740`） | `https://github.com/Allenwdk/NativeAdreno.git` | `22abfe4` | 上游 main `6fa7bae` + **gen7_11_0 profile** + 有界失败记录 |
| 显示（我们的 `drivers/GopFramebuffer`） | `https://github.com/Allenwdk/NativeMobileDisplay.git` | `f3e29a2` | 上游 main `e47c1b8` + 24A446 requested-state 修复 |
| Metal 前端（我们的 `drivers/AdrenoMetal`） | `https://github.com/Allenwdk/AdrenoMetal.git` | `854f99a` | 上游 main `243d63c` + iOS 27 Core Image 库的条件重试 |
| SoC 层 | `https://github.com/Allenwdk/UniXNU-sm7675-common` | `7f857a8` | SM7675 共享层；muyu 目前**没有**这一层 |
| 核心 | `UniXNU/UniXNU` | `899fdcb` | uke 构建用的核心（我们当前 `44cf475`，落后 1627 行） |
| UEFI | uke 自己的 Silicium（`8a1153e`） | | 我们用的是 Mu-Silicium `muyuPkg` |

**关键事实：`22abfe4` 里 `NativeAdrenoProfileTable` 已经有 `0x43030b00` 这一条**（uke 的 A732 与我们的
A735 是**同一个 chip id**），所以"给 A735 单独加 profile 条目"这件事**不需要做** —— 前提是 OPP 表由板级
输入提供，而不是抄 profile（见 §3）。

## 2. 我们现在的状态

| 项 | muyu 现状 |
| --- | --- |
| 驱动 pin | `drivers/Adreno740` `a9a2cbb`、`drivers/AdrenoMetal` `a0f0287`、`drivers/GopFramebuffer` `19ef3a7`（全部是 uke 基线的**祖先**） |
| 内核里有 GPU 驱动吗 | **没有**。`board.toml [kernel] archives` 里没有任何 Adreno 构建项；只有只读探针（`kits/AdrenoFirmware`、`kits/AdrenoPM4`）与 `build-native-adreno-kits.py` 这套 sheng 形状的构建器 |
| `[gpu]` 表 | **本次已加**（生成式，见 §4） |
| ADT `gmu-memory` 节点 | 缺（`make-muyu-adt.py` 没有） |
| UEFI `GMU Pool` 预留 | 缺（Mu-Silicium `muyuPkg` 里没有） |
| GPU 启动参数 | 缺（`sheng-gpu-power=9` 等一条都没有） |
| GPU addon（Metal 插件等） | 核心**支持** `--gpu-addon`（`payload.py`/`make-gpu-addon.py` 都在），但 muyu 从来没打过 |
| 显示路径 | `drivers/GopFramebuffer/NativeLegacyDisplay.cpp` CPU 逐帧 `memcpy`，~1.4 fps |
| 固件 | `fw/gpu-firmware/gen71100_{sqe.fw,gmu.bin,zap.mbn}` 已从设备抽出；SQE/GMU 与 uke **逐字节相同**，ZAP 不同（`463ba9b7…` vs `12949928…`） |

## 3. 差异清单（按依赖顺序）

1. **板级输入**（本次完成一半）：`[gpu]` 表 + 芯片级常量。剩下的两个数字是**假设**：
   `gmu_core_clock_hz=220 MHz`、`gmu_hub_clock_hz=200 MHz`、`gmu_pool_base/bytes=0xC8000000/32 MiB`
   —— 这些是 uke 的 A732 值（同一个 GPUCC/GCC），不是我们量出来的，表里已注明。
2. **构建链移植**（下一步）：uke 的 `archives` 是
   `prepare-uke-gmu.py` → `component:Adreno740` → `build-adreno740-kits.py` →
   `build-adreno740-accelerator.py` → `build-native-legacy-display-27.py`。
   我们这边对应要写成 `prepare-muyu-gmu.py`（读 `[gpu]` + `scripts/data/muyu-gpu-inputs.json`，
   生成 `build/sheng-gmu/ShengGmu{Source,VotesSource,CommandDbSource}.h`），并把
   `component:Adreno740` 换成本仓库的 `build-arm-driver.py Adreno740`（我们的核心还没有
   `component:` 语法）。**注意**：uke 的 `build-native-legacy-display-27.py` 是 24A446 的显示后端，
   我们跑 21H461，这份要按我们的 `IUISurface`/`IOMobileFramebuffer` ABI 重新对，不能照抄。
3. **ADT `gmu-memory` 节点**：uke 由 `scripts/make-uke-adt.py` 生成，池子 `0xC8000000+32 MiB`，
   必须与 UEFI 的 `GMU Pool` 条目、`[gpu] gmu_pool_*` **三处一致**。
4. **UEFI `GMU Pool` 预留**：`MemoryMapLib` 里在 2–4 GiB 空洞划一块，标记成不分配。
5. **启动参数**：`sheng-gpu-power=9`、`sheng-gpu-metal=1`、`sheng-gpu-preempt=2`、`sheng-gpu-dcvs=1`、
   `sheng-fb-gpu=1`、`sheng-gpu-addon=1`（board.toml boot-args 有 ~1000 字节上限，加之前先数）。
6. **gpu addon**：`make-gpu-addon.py` + owner trust cache（`--owner-trust-cache`）。
7. **ZAP**：我们自己的 `gen71100_zap.mbn`（`463ba9b7…`），不要用 uke 的。

## 4. 本次已完成（可复查）

- `scripts/data/adreno-profiles.json`：把 `NativeAdreno@22abfe4` 的
  `NativeAdrenoProfile.cpp` 里 **A740 与 A732 两条 profile 逐字段抄下来**，带仓库/commit/文件 sha256
  （`6d71ee96…`）。这是"六个不知道的字段"的合法来源：它们是**按 chip id 索引的芯片级常量**，不是板级
  测量值，所以同 chip id 的 muyu 可以直接用；`memory` 里那六个字段仍然保持 `null`（那是 DTB 派生区，
  DTB 没有就是没有）。
- `scripts/derive-muyu-gpu-inputs.py`：把这份 pin 合进记录，并做**交叉校验** ——
  `gmu_gpu_delta` 必须等于 DTB 的 `gmu_base - gpu_base`（实测 `0x68000` ✓），`ubwc_version` 必须等于
  DTB 的 `qcom,ubwc-mode`（4 ✓），firmware 名字必须是我们这几个文件。
- 🐛 **修掉一个真 bug**：`smmu_stream()` 原来取 `iommus` 的**最后一格**（mask），于是
  `sid_user/sid_secure/sid_gmu` 全被算成 `0`。真实绑定是 `<&apps_smmu SID MASK>`，SID 在 phandle 之后
  那一格：**0 / 2 / 5**，与 uke 完全一致。这个 bug 如果不修，SMMU context 会用 0 号 stream 建起来。
- `scripts/gen-muyu-gpu-table.py`：从记录生成 `board.toml` 的 `[gpu]` 表（uke 的键名），
  `--check` 验漂移、`--write` 重写块。`check-muyu-gpu-inputs.py` 现在 27 条断言全绿（含表漂移检查）。

## 7. 可行性探针（2026-11-04，离线编译）

在把 pin 换掉之前先问了一个更便宜的问题：**uke 验收的那份 NativeAdreno 源码，能不能在我们的
21H461 内核树上编过？** 做法：拿本树 `IOPlatformExpert.cpo.json` 的编译配方，把 `/tmp/na`
（`22abfe4`）里全部 `NativeAdreno*.cpp` 逐个 `-fsyntax-only`。

结果：**25 / 33 通过**，失败恰好三类，都是可以定位的：

| 失败 | 文件数 | 原因 | 处理 |
| --- | --- | --- | --- |
| `ShengGmu.h` 找不到 | 4 | 新驱动仍要 `kits/*/ShengGmu.c` 那套 + `build/sheng-gmu/` 生成头 | 移植 `prepare-muyu-gmu.py`（写 `ShengGmu.h` / `ShengGmuSource.h`） |
| `../AdrenoMetal/OriginalAcceleratorABI.h` 找不到 | 1 | `build-adreno740-accelerator.py` 要兄弟目录里的 Metal ABI 头 | 换 pin 时 AdrenoMetal 必须**同一个提交级**跟上 |
| `task_get_gpu_role` / `PRIO_DARWIN_GPU_BACKGROUND` / `_UTILITY` 未声明 | 1 | 这是 **iOS 26/27 的 API**，21H461 没有 | ✅ 已用 `package/MuyuGpuRoleCompat.{h,c}` 解决（见下） |

（另外两个文件是 host 单元测试，要 `cassert`，本来就不进内核。）

### 7.1 17.7.11 的 GPU role 兼容层

驱动在 `NativeAdrenoUserClient::submitPriority()` 里读"这个进程的 GPU role"来决定把两个 Metal 队列
优先级中的哪一个交给 ring。21H461 没有这个 API，但同样的信息存在**任务策略属性**里
（`TASK_POLICY_ROLE` → `proc_task_role_to_darwin_role()` → `PRIO_DARWIN_ROLE_*`，见
`bsd/kern/kern_resource.c` 的 `proc_get_darwin_role()`）。所以兼容层是**读取**既有字段，而不是造值：

- `package/MuyuGpuRoleCompat.h`：给 C++ 的声明 + 两个新常量名（映射到 `PRIO_DARWIN_ROLE_NON_UI` /
  `UI_NON_FOCAL`，**这是假设**，只在选优先级时有影响，不影响正确性）；
- `package/MuyuGpuRoleCompat.c`：C 侧实现，`proc_get_task_policy(task, TASK_POLICY_ATTRIBUTE,
  TASK_POLICY_ROLE)` → 转成 `PRIO_DARWIN_ROLE_*`。必须分离成 C，因为 `kern/task.h`、
  `kern/policy_internal.h` 进不了 C++ TU（`ledger.h:151` 把 `template` 当参数名）。

已验证：`MuyuGpuRoleCompat.c` 用本树 C 配方编过；`NativeAdrenoUserClient.cpp` / `Submit` /
`Publish` / `Ring` / `Gpu` 带上 `-include MuyuGpuRoleCompat.h` 后**全部 0 error**（之前 UserClient 报 5 个
未声明）。接进构建时，kit builder 要给这 5 个 TU 加 `-include`，并把 `.c` 编进同一个 archive。

## 5. 风险 / 不要做的事

- **uke 的验收是 iPadOS 27（24A446），我们跑 17.7.11（21H461）**。驱动内核侧（GMU/CP/SMMU/ring）跟
  系统版本关系不大，但 **Metal 插件与 display 后端是按 24A446 的 ABI 对的**，不能假设 21H461 直接吃下。
  真要做 Metal，得按 17.7.11 的 `NativeMetal`/`MobileDisplay` ABI 重新验。
- **OPP 表不能抄**：uke 9 档顶 950 MHz，我们 11 档顶 1100 MHz，`initial_pwrlevel` 10 vs 8；
  `acd_level` 也不同。抄过去就是给 GPU 上错误的电压/频率组合。
- **不要照抄 uke 的 ZAP**（board-specific）。
- **GPU 编进内核会改启动路径**：在启动确定性（`tools/muyu-boot-census.sh`，5 轮 ≥4 轮进桌面）还没
  关掉之前，不要把 GPU 项加进默认镜像；要试就做独立 vendor_boot（`fw/vendor_boot_gpu*.img`）并单独记台账。

## 6. 下一步（离线可做）

```bash
cd UniXNU-muyu
/opt/homebrew/bin/python3.12 scripts/derive-muyu-gpu-inputs.py   # 重新派生（改 DTB 或 pin 后）
/opt/homebrew/bin/python3.12 scripts/check-muyu-gpu-inputs.py    # 27 条断言 + A732 对照表
/opt/homebrew/bin/python3.12 scripts/gen-muyu-gpu-table.py --check
```

然后按 §3 的顺序做 2→7；每做完一项都要能让 `check-muyu-gpu-inputs.py` 与
`check-sheng-gmu-startup.py`（等 port 完之后）保持绿。
