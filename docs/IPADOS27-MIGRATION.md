# 迁到 iPadOS 27：可行性 spike（2026-11-04，离线）

> 回答的问题：**"可以直接迁移 iPadOS 27 吗？"**
> 结论：**不能"直接"，但方向比留在 17 更值得做**；它是一次结构性迁移，不是换 IPSW。本文记录已经
> 验证过的事实、spike 的真实错误序列、以及分阶段计划与判定点。

## 1. 结论摘要

| 判断 | 依据 |
| --- | --- |
| 27 的 profile 就在核心仓库里，工具链**支持**两条线并存 | `UniXNU/profiles/os/ipados-27.0.1-24A446/{profile.toml,patches/xnu/series,abi/}`；核心同时有 `patches/xnu`（10063/arm64）与 `patches/xnu-12377`（arm64e）两套补丁树 |
| 我们的 board 目前**用不上**任何 profile | `UniXNU-muyu/board.toml` 是 `schema_version = 1`（没有 `[board] soc_source`/`os_profile`），核心的 profile 分层只在 schema 2 生效（`scripts/unixnu/config.py:136`） |
| 迁 27 = schema 2 + 一个 **SoC 层** + arm64e + fileset prelink + 新根镜像/信任缓存 | 见 §2–§6 |
| 27 线上游已经把 GPU/owner 那批拼图做出来了，17 线上没有 | `patches/xnu-12377/` 里有 `zzy-native-gpu-addon-mount`、`zzw-native-adreno-sandbox-grant`、`offline-native-gpu-broker`、`offline-owner-systemos-mount`、`native-pac-keys` 等 |

**关键的一句话**：留在 17 做 GPU，等于自己补 Metal/display 的 21H461 ABI；迁 27，是接手上游已经验收过的
那条线的**设备层**工作。两者工作量同量级，但后者不是孤岛。

## 2. 27 的目标（读 profile 得来的，不是猜的）

| 项 | 值 | 我们现在 |
| --- | --- | --- |
| OS profile | `ipados-27.0.1-24A446` | `ipados-17.7.11-21H461` |
| IPSW | `iPad17,3` / `j820ap` / chip `0x8142`（33090）/ board 12 | `iPad7,1` / `j120ap` / `0x8011` / board 12 |
| arch | **arm64e** | **arm64** |
| XNU / Darwin | `12377.121.6` / `25.5.0` | `10063.141.1` / `23.6.0` |
| 核心补丁树 | `patches/xnu-12377/`（68 个） | `patches/xnu/`（59 个） |
| prelink | `--fileset-reference build/xnu/fileset-reference/mach.release.bcm2837.unstripped.noctf --allow-missing` | 直接拿 21H461 kernelcache 当参考 |

## 3. 结构性前置：board 必须迁到 schema 2

schema 2 下 board 只是**设备层**，其余从四个 profile 合并进来：`arch` + `soc` + `os` + `device`
（`scripts/unixnu/profiles.py:33-56`）。要做的：

1. `schema_version = 2` + `[board] soc_source = "<soc 名>"` + `[board] os_profile = "ipados-27.0.1-24A446"`；
2. **写一个 SoC 层**（`socs/<name>/soc.toml` + 该层自己的 `patches/xnu-…/series`）。uke 用的是
   `Allenwdk/UniXNU-sm7675-common`（pin `7f857a8`），今天尝试拉取**失败两次**（`Empty reply from server`），
   说明它多半没有公开，uke 的 `feeds.toml` 也写着 `local = "../UniXNU-sm7675-common"`。⇒ **SoC 层要我们自己
   从现有 board.toml 的 SoC 级数值（时钟/regulator/pram/SMMU/CPU 拓扑）拆出来**；
3. `[components] include`、`[os_overrides."<profile>"]` 这套 per-OS 差异表（uke 的 board 有这两个表，我们没有）；
4. 设备补丁改成 `patches/xnu-12377/series` 的 series 形式（我们现在是 `patches/xnu/*.patch` 平铺、无 series）。

## 4. arm64e 带来什么（与 17 的 arm64 的真实差异）

- 核心 `patches/xnu-12377` 里带着 arm64e 专用项：`arm64e-threaded-starts`、`native-pac-keys`、
  `fileset-unmapped-call`、`zzx-pmap-sync-64bit-pa` 等；arm64e 的 patch series
  （`profiles/arch/arm64e/patches/xnu/series`，64 条）与 arm64 的 series 是**两套**。
- 我们的 17 个设备补丁只有两类能直接对上 12377 现成补丁：
  `offline-owner-root-auth-bypass` ↔ `offline-owner-root-binding-audit`（含 `-root-crypto-probe`）、
  `zzzz-offline-owner-systemos-bringup` ↔ `offline-owner-systemos-mount`；
  其余 15 个（video console、panic/hold-reboot、watchdog policy、console 探针、mount/exec trace 等）
  都要 rebase 或重写到 12377 的上下文。
- 我们**没有**任何 PAC/discriminator 代码（arm64 不需要）；上游设备文档写明 24A446 上 HID/display 的
  vtable 要按 kernelcache 里的 220 个 discriminator 重签 —— 这部分是迁 27 之后显示/触摸 bring-up 的真实成本。

## 5. prelink 与根镜像（和 17 不是一条路）

- 24A446 的 kernelcache 是 **MH_FILESET + 符号被剥**，27 profile 的 prelink 要先用
  `scripts/build-fileset-release-reference.py --lto` 造一份**带符号的 RELEASE 参考核**，再
  `--fileset-reference ... --allow-missing`（未匹配调用走 `fileset-unmapped-call.patch` 的报告桩）。
- 根镜像（userdata）要换成 27 的 OS + SystemOS DMG（`[userdata]` window 布局、SHNGROOT 清单都得重做，
  我们已有 `scripts/rebuild-userdata-image.py` / `inspect-apfs-image.py` 这套工具）；
- owner trust cache bundle 要从 27 IPSW 的 `StaticTrustCache` + `Cryptex1,SystemTrustCache` 重建
  （我们现在用的是 `/tmp/muyu-static-system.bundle`，21H461 的）；
- ADT identity 全套（chip_id/board_id/product 字符串）跟着换。

## 6. Spike 的真实错误序列（今天跑的）

命令：在一份**临时副本**（`/tmp/spike27`，不动真仓库）上把 board 改成 schema 2 并指向
`os_profile = "ipados-27.0.1-24A446"`、`soc_source = "sm7675"`，然后：

```bash
python3 unixnu build kernel -D /tmp/spike27 --os 27 --out /tmp/spike27-out
# 1) unixnu: soc sm7675: /private/tmp/sm7675 is not a directory          ← SoC 层缺失（结构性）
# 2) （放了一个 stub soc.toml 后）unixnu: no configuration: run "unixnu defconfig NAME" first
# 3) unixnu defconfig muyu-userland_defconfig                             ← 接受（配置名可以沿用）
#    -> /private/tmp/spike27-out/muyu/ipados-27.0.1-24A446/muyu-userland/.config
# 4) 重新 build kernel: 走到 fetch xnu 12377（ac9718f）
#    - 第 1 次：流中断（fetch-pack: unexpected disconnect），已收 24 MB
#    - 第 2 次（--depth 1 直接 fetch，HTTP/1.1）：收到 26% 后 early EOF
#    - 第 3 次（--depth 1 --branch xnu-12377.121.6 clone）：Empty reply from server
#    ⇒ GitHub 到本机当时不稳定（同一时段其他仓库 clone 正常），阶段 1 因此尚未跑完
```

### 6.1 继续往下推（同一天，找到 ghfast.top 之后）

`HANDOFF.md` §7 第 1 条早就写了答案：**GitHub 直连 20–30 KB/s 且会 early EOF，`ghfast.top` 镜像
400 KB/s–2 MB/s**。换成

```bash
GIT_CONFIG_COUNT=1 \
GIT_CONFIG_KEY_0=url.https://ghfast.top/https://github.com/.insteadOf \
GIT_CONFIG_VALUE_0=https://github.com/ \
python3 unixnu build kernel -D /tmp/spike27 --os 27 --out /tmp/spike27-out
```

之后 xnu 12377 的 52 MB **50 秒内拉完**，构建一路推进到下面这些**真实结论**：

| 发现 | 含义 |
| --- | --- |
| `UniXNU-muyu/scripts/build-xnu-4k-baseline.sh` 和 `prepare-xnu-source.py` 在组装树里**覆盖核心版本** | 它们是 10063 时代的分叉，不认 `UNIXNU_PATCH_LIST`，会把 17 个设备补丁硬打到 12377 上，在 `osfmk/arm64/start.s:697` / `bsd/kern/kern_exec.c:7174` 失败。**迁 27 必须删掉或更新这两个覆盖**（它们不删，27 线永远起不来） |
| `zzz-boot-progress.patch` 的 hunk 依赖 `sheng_pmgr_run()` 出现在 `bsd_init.c` | 10063 线上这来自 `patches/xnu/sheng-platform-power.patch`；12377 线上**只能来自 soc 层** ⇒ SoC 层不只提供元数据，还提供**补丁**。用 5 行 stub 补上后，**69 个补丁（arch 64 + soc 1 + os 4）全部干净应用** |
| legacy 头（DriverKit/TrustCache）仍要从更老的 tag 补 | `prepare-xnu-source.py` 走 `raw.githubusercontent.com`，直连会超时；本地 17 线树的 `.cache/sources/*.json` + 已校验头文件可以直接搬；单个缺失文件可用 `cdn.jsdelivr.net/gh/apple-oss-distributions/xnu@<tag>/...`（实测 2.5 KB 正常） |
| **arm64e 工具链本身是通的** | 构建日志里出现 `libfirehose_kernel.a is architecture: arm64e` —— 头文件/静态库阶段已产出 arm64e 产物，不是“工具链不认” |
| **owner-root 策略变了** | 12377 的构建脚本强制 `OWNER_CACHE_MOUNT=1 ⇒ OWNER_ROOT_TRUST=1`（原文 *"The offline SystemOS mount requires real independent owner root enforcement"*），而 10063 线我们用的是 `offline-owner-root-auth-bypass.patch`。⇒ 迁 27 意味着**改走真实 owner-root 校验 + 源存储**（`CONFIG_XNU_OWNER_ROOT_AUDIT/TRUST` + `XNU_SOURCE_STORAGE`/virtio-block） |

**已经证明的事**：board 改成 schema 2 之后，`--os 27` 会被正确解析、配置名可以沿用、out 路径按
`<board>/<os_profile>/<config>` 分层；整条 **arm64e + 12377 的补丁序列在一个 SoC 层 stub 之上可以完全应用**，
构建推进到内核编译前的策略闸门（owner-root），说明"27 线跑不跑得起来"已经从猜测变成工程问题。


### 6.2 阶段 1 结论（2026-11-04 19:5x，已跑到底）

把 `muyu-ipados27_defconfig`（照 uke 的 `uke_ipados27_defconfig` 改名为 muyu）选上、board 的
`[kernel] archives` 只留 corecrypto 之后，**XNU 12377 的 arm64e 源码全部编译通过**：

```
objects built: 957        file .../startup.o     -> Mach-O 64-bit object arm64e
                          file .../bsd_init.o    -> Mach-O 64-bit object arm64e
LD mach.development.bcm2837.unstripped.noctf
ld: -threaded_starts_section is no longer supported
xcrun: error: unable to find utility "ld-classic"
```

⇒ **阶段 1 的答案是"能编译，卡在链接器"**，而且卡得很具体：

- `patches/xnu-12377/arm64e-threaded-starts.patch` 给链接器加了 `-threaded_starts_section`；
- Xcode 27.1 的 `ld` 明确不支持它，构建链条回退到 `ld-classic`，而 **Xcode 27 已经不带 `ld-classic`**（实测
  `XcodeDefault.xctoolchain/usr/bin/` 里只有 `ld`）。

两条路，都不涉及设备：

| 选择 | 代价 | 风险 |
| --- | --- | --- |
| 装带 `ld-classic` 的工具链（Xcode 26 的 `ld64` 或自建 `ld64`） | 一次性 | 低，最接近上游 |
| 从 series 里去掉 `arm64e-threaded-starts`，用普通链接 | 一次性，1 行 | 失去 threaded starts；arm64e 启动路径要重新验（上游可能依赖它做 per-thread 起始栈） |

### 6.3 阶段 1 收尾：链接也过了（去掉 threaded starts）

把 `arm64e-threaded-starts.patch` 从序列里拿掉（它只做两件事：给链接器加 `-threaded_starts_section`、
以及 `rebase_threaded_starts()` 相关的源码改动），其余 67 + 1（soc 的 `sheng-platform-power`）= 68 个补丁
按 arch → soc → os 的顺序应用，然后完整构建：

```
objects built: 957
LD   mach.development.bcm2837.unstripped.noctf
/tmp/spike27-out/muyu/ipados-27.0.1-24A446/muyu-ipados27/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf
  -> 23,897,328 bytes, Mach-O 64-bit executable arm64e, sha256 8bd9ff3f1ec65fb5…
build script exit=0
```

⇒ **阶段 1 完成：iPadOS 27 的 arm64e XNU 12377 内核在本机可以完整编译并链接。**
一条路径是去掉这个补丁（本次做法），代价是**失去 threaded starts**（`-threaded_starts_section` 段
与 `rebase_threaded_starts()`），arm64e 上要单独验证启动/KASLR 重定位是否依赖它；另一条是装回带
`ld-classic` 的工具链、保留补丁。两条都要在上机前定下来，但**不再阻塞"能不能编"这个判断**。

### 6.4 阶段 3（归档链）进行中的实测状态

1. **schema 2 只组装 board 声明的组件**：树里 `drivers/` 只有 `[components] include` 列出的组件
   （`scripts/unixnu/components.py:47`）。不声明就出现"`drivers/SoftwareAES/NativeAES.cpp` 不存在"这种
   假故障。声明之后组件会被真正编译（`component-<ID>` 日志）。
2. 用我们 10063 的 archives 列表 + 10063 的开关跑 27 时，**已通过 6 个归档**：
   `build-corecrypto-14.py`（两次）、`ArmDTPlatform`、`ArmCpu`、`RamBlock`、`build-libpthread-kernel.py` 等。
3. **第一个真正的 API 断点：`ShengUfs`（我们的 UFS 驱动）对 12377 编不过**，5 个错误都是被移除/改名的
   内核 API，不是我们的逻辑错：

   ```
   NativeUfs.cpp:1436: unknown type name 'kma_flags_t'
   NativeUfs.cpp:1436: use of undeclared identifier 'KMA_KOBJECT'
   NativeUfs.cpp:1436: use of undeclared identifier 'KMA_DATA'
   NativeUfs.cpp:1443: use of undeclared identifier 'vm_map_protect'  (did you mean 'vm_protect'?)
   NativeUfs.cpp:1595: use of undeclared identifier 'kmem_free'
   ```

   ⇒ 这些就是"设备驱动 rebase 到 12377"的具体工作量：`kalloc_type`/`KMA_*` 迁移、`vm_map_protect` →
   `vm_protect`、`kmem_free` → `kheap_free`/`zfree`。同类问题预计还会出现在 display / thermal / hid。


### 6.5 27 根镜像已到手；UFS 已 rebase；下一个断点是 HID

- **IPSW 下完**：`fw/ipsw/iPad17,3_27.0.1_24A446_Restore.ipsw` = 11,654,176,243 B（与 CDN 的
  `Content-Range` 总长一致）；已抽出 `kernelcache.release.ipad17`（24,014,290 B）、
  `DeviceTree.j820ap.im4p`（65,439 B）与三个 `*.trustcache` 到 `fw/ipsw/24A446/`。
  注意 kernelcache 的**原始** sha256 与 profile 里的 pin 不同（profile 钉的是 IMG4→LZFSE→slice
  规范化后的值，`ipsw.kernelcache()` 才算那个），所以别拿原始文件去对 pin。
- **UFS 已 rebase（`9c1e416`）**：`drivers/ShengUfs/UfsXnuCompat.h` + `build-arm-driver.py` 传
  `-DXNU_MUYU_XNU_VERSION=<XNU 主版本>`。三处 API：`kmem_alloc` 丢 flags、`vm_map_protect` →
  `mach_vm_protect`、`kmem_free`（12377 里 kernel-private）→ `mach_vm_deallocate`。
  **归档链第一次越过 UFS 组件**。
- **下一个断点：`ShengHidInput`**，它的构建器直接拒绝：
  *"The original HID ABI builder only supports arm64 iOS 17. Use build-sheng-source-drivers.py for a
  source-interface compile audit"* —— 也就是之前记的那件事：24A446 的 HID/display 要按 kernelcache 里的
  **vtable discriminator** 重新对。现在 kernelcache 已经在本地，这条路可以开始了。


### 6.6 设备仓库里"影子脚本"清单（27 线的隐藏地雷）

设备仓库 `scripts/` 与核心 `scripts/` 有 **7 个同名文件**，组装树里设备版优先。前四个是 10063 时代的分叉，
27 线上会直接坏事或悄悄用旧逻辑：

| 设备仓库文件 | 27 线上的问题 | 处理 |
| --- | --- | --- |
| `build-xnu-4k-baseline.sh` | 不认 `UNIXNU_PATCH_LIST`，把 17 个设备补丁硬打到 12377 上 | 27 用核心版（spike 里已禁用） |
| `prepare-xnu-source.py` | 旧版只能直连 raw.githubusercontent（超时） | 27 用核心版（已禁用） |
| `prepare-storage-headers.py` | **旧版没有 `pinned_source()`**，`build-sheng-source-drivers.py` 直接 AttributeError | 27 用核心版（已禁用；核心版有该 API） |
| `build-arm-driver.py` | 我们的 arm64e 审计/部署目标补丁在这里，**要用我们的** | 保留 |

（`patch-xnu-entry-probe.py`、`xnu-panic-screen.c`、`data/` 是 17 线专用的第 4–6 个，暂不涉及。）

**2026-11-04 已处理（`9604118`）**：前三个移进 `scripts/legacy-10063/`（附 README 说明它们如何造出 17 的镜像、
以及 27 上会怎么坏），组装树从此用核心版。验证：

- 27 链：**干净跑过 7 个归档**，停在与影子脚本无关的已知限制（`ShengHidInput` 的 arm64-iOS17 守卫）；
- 17 线：`unixnu build payload` 仍然产出 60 MiB payload（`make-muyu-adt.py` 的 `--no-filesystems` 输出不变），
  说明冻结的那条线没被打散。

### 6.7 HID：27 上没有捷径，必须重做 original-ABI 绑定

- `build-sheng-hid-input.py` 的守卫是硬的：`if ARCH != 'arm64' or IOS_VERSION.split('.')[0] != '17': raise`。
  它的工作是**用 original IOHIDDevice 的 vtable 把我们的适配器绑上去**，而 24A446 的 vtable 槽位签名
  （arm64e 上还要 re-sign）与 21H461 不同 ⇒ 这个守卫不是保守，是真的会错。
- 核心的 `build-sheng-source-drivers.py` **只编译公共源码接口**，它的 `--hid-source` 指的是
  `refer/IOHIDFamily`，不是我们的适配器；它明确写着"不绑定 original 家族、不产出 boot image、不qualify
  私有 ABI"。所以它**不能**替代 HID 适配器构建。
- ⇒ 27 的 HID 需要：用**本地已到手的 24A446 kernelcache** 读出 `IOHIDDevice` 的 vtable 槽位与
  discriminator，重做 `build-sheng-hid-input.py` 的绑定与校验。这和 GPU/display 的 24A446 对齐是同一类工作。


## 7. 分阶段计划（每一步都有判定点）

| 阶段 | 动作 | 判定点 |
| --- | --- | --- |
| 0 | 写 SoC 层（`socs/sm8635`：从现有 board.toml 拆 SoC 级数值；先只求“能解析”） | `unixnu build kernel --os 27` 能走到补丁应用阶段 |
| 1 | 下载 xnu 12377，跑 arm64e 基线（只 arch+os series，不带设备补丁） | ✅ **已完成（§6.2）：957 个 arm64e 对象全部编出，只剩链接器 `-threaded_starts_section`/`ld-classic`** |
| 2 | 把 17 个设备补丁 rebase 到 12377（两类先做：owner/root、systemos；其余按需） | 内核能编 + prelink 能出 kernel.macho |
| 3 | fileset prelink 参考核（`build-fileset-release-reference.py --lto`） | prelinker 不再报 unmapped call |
| 4 | 27 IPSW：kernelcache/DTB/两个 trust cache 拉取（range requests 只取需要的成员） | `ipsw` 步骤通过 + identity 对得上 |
| 5 | 27 根镜像（userdata）+ 持久化 Data 窗口 | 离线 APFS 检查（`inspect-apfs-image.py`）通过 |
| 6 | 上机 bring-up（mount/launchd/显示/触摸），用现成探针 | 进桌面 = 27 线成立；此后 GPU 才能按 uke 的 ABI 直接抄 |

**下次从这里继续**：网络恢复后重跑

```bash
cd /Users/diannao/pad7pro-xnu/UniXNU
python3 unixnu build kernel -D /tmp/spike27 --os 27 --out /tmp/spike27-out
# 若要保留这次已做的临时 board：/tmp/spike27 是 UniXNU-muyu 的副本，只改了 schema_version/[board]/feeds.toml
```

**阶段 1 是真正的分水岭**：它只回答一个问题 —— arm64e 内核在我们的机器/工具链上编不编得出来。
不涉及设备、不改默认镜像，最坏情况只是浪费时间。

## 8. 不做什么

- **不覆盖现有 17 线**：`fw/vendor_boot_nofs1.img`（`c7635cdb…`）是唯一已验证能进桌面的镜像；27 迁移
  走独立分支/独立 `out/` 目录，用户数据（userdata）在迁移前后都要有可回滚的镜像。
- 不把 27 迁移和 GPU 混在一起做：GPU 的上机实验等 27 能进桌面之后再说（否则两个变量同时在动）。
- 不用 uke 的 ZAP/OPP/身份数字直接抄（见 `docs/GPU-UKE-ALIGNMENT.md` §5）。
