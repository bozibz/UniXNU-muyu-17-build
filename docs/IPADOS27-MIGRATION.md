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


### 6.8 顺序被纠正：HID 在 prelink 之后；而 prelink 暴露了"核心太旧"

**HID 不是阶段 3 的一块，而是阶段 4 的下游。** `build-sheng-hid-input.py` 的做法是拿 **prelink 候选镜像**
里的原始 `com.apple.iokit.IOHIDFamily` 去核对：`IOHIDDevice` 的 vptr（17 线是 `0xfffffff005d045c0`）、
207 个槽位、18 个 native-base 覆盖下标、构造函数/D2 的精确字节码。这些都写在脚本里、**来自 prelink 产物**，
所以 24A446 的 HID 绑定必须等 prelink。

**prelink 的第一步（RELEASE/LTO 参考核）在我们这套核心上过不去。** 用
`scripts/build-fileset-release-reference.py --tree <tree> --lto` 跑，编译全部通过、链接失败：

```
ld: symbol(s) not found for architecture arm64e.kernel
    "_t8110dart_vo_tt_index", referenced from: <initial-undefines>
    "_uat_get_desc", ...
```

这些是 **RELEASE 配置自带的 `<initial-undefines>`**（Apple 自己内核里由内联汇编/别处提供的符号）。我们的核心
是 `44cf475`，而 uke 那条线用的 `899fdcb` 里 `build-fileset-release-reference.py` **被重写了 300 行**
（`git diff 44cf475 899fdcb -- scripts/build-fileset-release-reference.py` = 203 行新增 / 98 行删除），
正是处理这类链接期缺口的地方。

⇒ **结论（计划需要修一次）**：27 线不能"核心停在 origin/main"就做完 —— 至少
`build-fileset-release-reference.py`（prelink 参考核）要换成 `899fdcb` 的版本。可行的两条路：

| 路线 | 内容 | 影响 |
| --- | --- | --- |
| A. 核心升到 `899fdcb` | 与 uke 同一条核心；27 的系列/prelink/文件集路径全部按上游走 | 17 线**不再从同一棵树重建**（镜像已冻结，历史可查），设备脚本要按 schema 2 重排 |
| B. 只把 `build-fileset-release-reference.py` 等两三个文件从 `899fdcb` 取过来 | 改动面小，仍叫"停在 origin/main" | 后面大概率还会撞到同类"核心太旧"的钉子，一颗一颗拔 |

**建议 A**：既然要做 27，就把核心升到 uke 验收时用的那个 commit，一次把 prelink/文件集/ABI 数据对齐；
17 线的镜子和历史都在，不存在"重建不了"的损失。


### 6.9 核心已按路线 A 切到 `899fdcb`（独立 worktree，不动旧历史）

```bash
cd UniXNU && git worktree add ../UniXNU-27 899fdcb   # 只读对象，开新工作树
cd ../UniXNU-27 && git checkout -b muyu-27          # 我们的 27 核心增量放这里
```

旧核心（`44cf475`）的历史和工作树**原封不动**，17 线不受影响。

**直接效果：HID 那道"只支持 arm64 iOS 17"的守卫消失了。** 它是旧核心
`build-sheng-hid-input.py` 里的产物；`899fdcb` 里这个脚本本来就按 24A446/arm64e 写的。换核之后 HID 能往下走到
真正编译适配器，暴露的是两个具体问题，而不是一句拒绝：

1. `clang: error: using sysroot for 'macOS 27.2' but targeting 'arm64e-apple-ios27.0.0' [-Werror,-Wincompatible-sysroot]`
   —— 就是 `HANDOFF.md` §7 第 3 条那个 Xcode 27 环境问题。已在 `899fdcb` 上补
   `-Wno-incompatible-sysroot`，commit `151bfea`（分支 `muyu-27`）。
2. `#include "OriginalHidDeviceABI.h"` 找不到：新核心的构建器把这个头**写在编译之后**
   （`scripts/build-sheng-hid-input.py:181`），所以要么它预期该头已由上一阶段生成，要么调用顺序需要调整。
   uke 的 27 HID 驱动（`UniXNU/ShengHidInput@24f1f3f`）**同样 include 这个头**，所以不是我们适配器独有。

⇒ 下一步就是弄清这个头的生成时机/路径，然后把 HID 组件接上；这是 27 归档链最后一块硬骨头。


### 6.10 新核心下的进展与"最后一公里"的形态

换到 `899fdcb` 之后连续跑通的部分：

1. **RELEASE/LTO 参考核建成了**（`build/xnu/fileset-reference/mach.release.bcm2837.unstripped.noctf`，
   15,853,984 B）。新核心在链接失败后会自动 `removed 67 unavailable forced exports; linking again` —— 这正是
   旧核心缺的那 300 行。
2. 归档链按新核心的规则重排：**`archives` 里每个被 `[components] include` 的组件必须写成
   `component:<ID>`**（脚本条目会被拒绝：`archives needs component entries for: …`）。这是 schema 2 的硬规则，
   uke 的 board 就是这个形状。
3. `native_prelink` 模块：新核心不会把 `kits/XnuPrelink` 链进树（它是核心子模块，不是 feed），
   而我们的多个归档脚本 `import native_prelink`。已把该模块（自包含、仅标准库）随设备 `scripts/` 走，
   并在文档记明来源（`UniXNU-27/kits/XnuPrelink@b74ed7c`）。
4. **依赖 prelink 候选的归档必须后置**：`build-sheng-platform-power.py` → `audit-amfi-pmgr-contract.py`
   报 `No reference candidate available`，`build-sheng-hid-input.py` 同理。所以 27 的正确顺序是
   **先出 prelink 候选，再回头跑这些归档**（17 线当年就是靠"跑两遍"收敛的，只是没写下来）。

剩下的是同一类小错配，不是设计问题：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `test -f "$UFS_ARCHIVE"` 失败 | 内核脚本找 `build/sheng-ufs/libShengUfs.a`，新核心的组件产物在 `build/native-ufs/` | 对齐路径（改脚本取后者，或让组件输出到前者） |
| 配置里 `CONFIG_XNU_NATIVE_UFS` vs uke 的 `CONFIG_XNU_SOURCE_STORAGE` | 两代 storage 模型 | 27 应走 uke 的 `SOURCE_STORAGE` + `build-sheng-source-storage.py`（核心自带），`build-software-aes-27.py` 还需要从 uke 仓库取 |


### 6.11 切到 uke 的 27 配置后，storage 的真面目露出来了

按 uke 的形状（`SOURCE_STORAGE` + owner 链 + `component:` 归档 + `build-sheng-source-storage.py` +
`build-software-aes-27.py`，后者从 uke 仓库取）重排之后，卡点变成一句很具体的话：

```
build-sheng-source-storage.py: error: cannot identify the device package under <tree>/kits: []
```

它要的是 `kits/*/SourceStorage.cpp`。uke 那边这个文件在 **`package/SourceStorage.cpp`**
（`git ls-tree -r c7aefb9 | grep SourceStorage` → `package/SourceStorage.cpp`），也就是**设备 kit 里的一份
"用公共 IOStorageFamily 接口实现 IOBlockStorageDevice"的源码**。

⇒ **27 的 storage 移植 = 把 `SourceStorage.cpp` 这个模型搬到 muyu**：它不再绑定 original UFS 的私有 ABI
（那是我们 17 线 `drivers/ShengUfs/NativeUfs.cpp`，2122 行），而是按公共接口编译，UFS 硬件访问仍在我们的驱动里。
这是目前 27 线上最大的一块新增工作。

顺带一条环境事实（`HANDOFF.md` §7 第 2 条被再次验证）：`unixnu` 内部调用 `python3`，本机 `/opt/homebrew/bin/python3`
不是 3.12，必须让 **3.12 在 PATH 最前**（我用了 `/tmp/pybin/python3 -> python3.12` 的垫片），否则
`import tomllib` 直接炸。


### 6.12 storage 启动器已落地；policy 材料是下一个 17 硬编码点

`package/SourceStorage.cpp`（126 行，照 uke `c7aefb9` 的模型）已写好：加载 IOStorageFamily →
校验 `IOBlockStorageDevice` 公共 ABI 布局 → 加载 policy → APFS → 注册源码类 → `native_ufs_source_create()`
建 provider。**没实现的部分用 weak 桩明确喊出来**（`native_ufs_source_create`、`native_source_root_policies_load`、
`native_ufs_root_step/failure`），所以它会链得上、并在真正跑到时打一句"还没移植"，不会假装能用。
boot 参数用 `muyu-source-storage=1`，与 17 线完全隔开。

构建器随后的要求变成（一步步都被实测出来）：

| 步骤 | 结果 |
| --- | --- |
| `cannot identify the device package` | ✅ 有了 `package/SourceStorage.cpp` 就过 |
| `expected full commit 7edb88fb…, found e45fbe30…` | ✅ 把 spike 的 IOStorageFamily pin 换成 27 的 `7edb88fb` 就过 |
| `scripts/policies/ipados-27.0.1-24A446/OS/trusted-key.bin` 缺失 | ⬜ 这是 **owner-root policy 材料** |

而生成 policy 的核心脚本 `build-owner-os-policy.py` **硬编码了 17 线**：

```python
source = ROOT / '.cache/firmware/target-volumes/044-76040-038.dmg'     # 21H461 的 OS dmg 名
expected_uuid = uuid.UUID('04769D26-2CA0-4825-B443-E2E5F88300F2')      # 17 线的 OS 卷 UUID
```

⇒ 27 需要把它参数化（dmg 名 + 卷 UUID 从 27 的 IPSW/镜像里取）。这是同一类"17 硬编码"问题的第三处
（前两处是 HID 的 207 槽位、storage 的 original-ABI）。**规律很清楚：凡是从 21H461 镜像里"读出常量"的脚本，
27 上都要重新参数化或重新取数。**


### 6.13 policy 生成器已参数化；顺带发现 27 的镜像格式变了（AEA）

`build-owner-os-policy.py` 现在接受 `--policy <json>`（或 `UNIXNU_OWNER_OS_POLICY`），五个值——
OS 成员名、它的 sha256、挂载点目录、卷 UUID、虚拟介质总字节——都从 JSON 读，且**仍然逐个对挂载中的镜像做断言**，
所以 17.7.11 的行为不变，27 线只要给一份 JSON。

给 27 准备这份 JSON 时发现了新的事实：**27 的 IPSW 里大镜像是 `*.dmg.aea`（AEA 加密），不是 17 那样的明文 UDIF**：

```
8254390272  043-68950-668.dmg.aea     <- OS 镜像（8.25 GB）
2348810240  043-69086-713.dmg.aea     <- 另一个大镜像（2.35 GB）
 234881024  043-69824-761.dmg.aea
```

影响三处，都是"17 假设明文 UDIF"的地方：

1. `build-owner-os-policy.py` 开头断言文件尾是 `koly`（UDIF trailer）——`.dmg.aea` 没有；
2. 我们的 `build-userdata-root-image.py` / `scripts/rebuild-userdata-image.py` 直接读 DMG 原始扇区；
3. `target-volumes` 缓存（`.cache/firmware/target-volumes/`）按明文命名。

⇒ 27 的根镜像链路要先过 AEA：`hdiutil attach 043-68950-668.dmg.aea`（现代 macOS 支持 AEA）拿到
`/dev/rdiskN` 后，`diskutil info` 读卷 UUID、`TotalSize` 读介质大小、sha256 直接对 `.aea` 文件算。
这条通了之后，policy JSON 就能填满，storage 归档也才能建出来。


### 6.14 27 的 OS 镜像已挂上，五个 policy 值全部拿到

`hdiutil attach` **不能**挂 `.dmg.aea`（`attach failed - 无法识别映像`），但新 API 可以：

```bash
diskutil image attach --readOnly --mountOptions nobrowse 043-68950-668.dmg.aea
#  /dev/disk5  ->  Apple_APFS_Container  ->  /dev/disk6s1
#  /Volumes/Rave24A446.J817J818J820J821OS
```

由此读到的 27 值（全部实测，不是猜）：

| 键 | 值 |
| --- | --- |
| `dmg` | `043-68950-668.dmg.aea`（7.69 GiB 压缩） |
| `dmg_sha256` | `ada8410520eefa0b4dfda5f8881a7a3a6c908f7705c734837b6a9c1ef267ebdd` |
| `media_bytes` | `9613344768`（9.6 GB，`diskutil info /dev/disk5` 的 Disk Size） |
| `volume_uuid` | `2F8E6EED-68D5-4261-B8A3-12571BAA2DD4`（OS 卷） |
| `mount` | `/Volumes/Rave24A446.J817J818J820J821OS` |

**但 policy 生成器还不能直接用这份 JSON**，因为它有两处 17 假设：

1. 开头断言文件尾是 `koly`（UDIF trailer）—— AEA 没有；
2. 用 `hdiutil info` 找"已挂载的只读镜像"，而 AEA 只能由 `diskutil image attach` 挂上，`hdiutil info` 看不到。

⇒ 需要把它的**主机侧取证**从 hdiutil 迁到 diskutil（`diskutil info -plist` 提供了同样的只读/GUID/卷 UUID/TotalSize 字段），
并对 AEA 跳过 `koly` 检查（改用 `.aea` 的 sha256 + `diskutil` 的只读标志做等价断言）。工作量不大，但必须做，否则
storage 归档拿不到 `trusted-key.bin`。


### 6.15 27 的 owner policy 已生成并通过全量校验（storage 差 SystemOS 这一半）

把生成器的取证迁到 `diskutil`（`--device`/`--volume`）+ AEA 分支后，27 的 OS policy 真的跑通了：

```
PASS: original OS signature and every 2347008 block verified by real C/RSA; no expanded image
build/verified-root-os/{manifest.bin 75,104,800 B, trusted-key.bin, header.bin, signature.bin, verification.json}
```

过程里需要两样本机材料，都是**一次性生成**、不进 git：`build/verified-root-host/keys/{owner.pem,owner-public.pem}`
（RSA-3072）与 `build/verified-root-host/verified-root.dylib`（由 `kits/VerifiedRoot/src/verified_root.c` +
`tests/openssl_provider.c` 用 Homebrew openssl 编出）。

随后 storage 构建器的要求是：**OS 和 SystemOS 两份 policy 都要**（`ShengUfs root publication requires both
OS and SystemOS policies`）。所以下一步是同一套流程对 SystemOS 镜像（`043-69086-713.dmg.aea`，2.35 GB）再做一遍，
产出 `SystemOS/{manifest.bin,trusted-key.bin}`。


### 6.16 两份 policy 都生成了；storage 的下一块是 `SourceRootPolicies.cpp`

SystemOS 那一份同样跑通（`043-69086-713.dmg.aea`，6.0 GB 介质，卷 UUID `F914C62A-AD00-47C6-8281-5746291C339A`，
sha256 `ece368c2…`）：

```
PASS: original OS signature and every 1468928 block verified by real C/RSA; no expanded image
build/verified-root-systemos/{manifest.bin, trusted-key.bin, ...}
```

把两份 policy（`OS` + `SystemOS`，含 `verification.json`）按 `scripts/policies/<os>/<name>/` 摆进设备树后，
storage 构建器报的下一条是：

```
error: SourceRootPolicies.cpp not found under the device or build tree
```

uke 的对应物是 `package/SourceRootPolicies.cpp`（**103 行**）：从 `/chosen/memory-map` 的 **`RootPolicies`**
侧车取物理地址+长度 → `IOMemoryDescriptor::withPhysicalAddress` 映射 → `vr_open` 用**编译进去的 owner key**
验签 → 填 `verified_root_policies[]`。它对 muyu 的额外前提：

1. `package/SourceRootPolicies.cpp` 本体（可照 uke 改，含 `VerifiedBlockPolicy.h`/`OwnerBootIdentity.h`）；
2. ADT 里要有 **`RootPolicies`** 这个 `/chosen/memory-map` 条目（我们的 `make-muyu-adt.py` 现在没有）；
3. `verified_root_key[]` + `vr_xnu_crypto` 来自 `kits/VerifiedRoot`（我们已能编出该 kit 的 host 库）。

⇒ 27 的 storage 还差这三小块，然后归档才能建出来、内核才能链上。


### 6.17 `SourceRootPolicies.cpp` 的真实依赖链（读 uke 源码 + 新核心得到的）

uke 的加载器（103 行）除了"读侧车 + 验签"，还**把 owner 身份绑死**：

- `owner_policy_header_sha256[2]`：两份 policy 各自"签名头"的 sha256（每 OS 不同）；
- `owner_boot_set_hash` / `owner_boot_set_domain`：`/chosen/boot-manifest-hash` 与
  `/chosen/owner-boot-manifest-domain` 的期望值（每机每 OS 不同）；
- 校验 `count == 2`、两个 envelope 的 offset/size 首尾相接、`cursor == length`。

⇒ muyu 需要一个新的**每 OS 生成头** `OwnerBootIdentity.h`（值从我们刚生成的两份 envelope 与 27 的 boot
manifest 推出），加上加载器本体。这不是"改几个常量"，而是和 `owner-os-policy-27.json` 同类的**取数产物**。

好消息：**ADT 侧已经不用我们做**。新核心 `payload.py` 里已有完整通路：

```python
# [payload] root_policies = "<VROOTB01 包>"
shutil.copyfile(policies, payloads / 'kernel.macho.vroot')
... devicetree.generate(..., root_policies=bool(policies))
```

而且 `scripts/unixnu/devicetree.py:339` 会自动加 `('RootPolicies', bytes(16))` 条目，`DarwinBoot.c` 也已经认识
这个条目并回填真实物理范围。**所以第 2 项（ADT）在新核心下是现成的**，我们的 `make-muyu-adt.py` 只在 schema 1
路径上才需要它。

而 `VROOTB01` 包正是 `build-sheng-source-storage.py:177` 生成的东西 —— 也就是说：**加载器写完 → storage 归档建出 →
它产出 bundle → payload 的 `root_policies` 直接吃**。链条闭环了。


### 6.18 storage 归档要求的三份设备源码：两份已写，第三份连着大工程

`build-sheng-source-storage.py` 明确要三个设备侧文件（`:224-225`）：

| 文件 | 作用 | 状态 |
| --- | --- | --- |
| `package/SourceStorage.cpp` | 启动器：加载家族、校验公共 ABI、注册源码类、建 provider | ✅ 126 行 |
| `package/SourceRootPolicies.cpp` | 读 `/chosen/memory-map` 的 `RootPolicies` 侧车，验签、绑定 owner 身份 | ✅ 145 行 |
| `package/SourceRootBinding.cpp` | 把 BSD 设备与"验过 owner 的 UFS 窗口"对上（`IOMedia` 查询 → provider 链） | ⬜ uke 121 行，可移植 |

`SourceRootBinding.cpp` 依赖 provider 侧的四个符号（`native_ufs_block_authenticates`、
`native_verified_block_root_authorized`、`native_verified_block_manifest_sha384`、
`native_verified_block_volume_device`），**它们全部来自 UFS provider** —— 也就是第 4 项（把 2122 行的
`NativeUfs.cpp` 从 original-ABI 绑法改成公共 IOStorageFamily 接口）。所以第三份可以先写、但要等 provider 才能真跑。

**owner 身份三处一致**（已落到 board 与头文件）：

| 位置 | 值 |
| --- | --- |
| `[adt.chosen.bytes] boot-manifest-hash` | `ee2c1b5c…`（SHA-384("MuyuBringup.BootSet.27\0")） |
| `[adt.chosen.bytes] owner-boot-manifest-domain` | ASCII `MuyuBringup.OwnerBootSet.v1\0`（28 B） |
| `package/OwnerBootIdentity.h` | 同一组值 + 两份 policy 的签名头摘要 |

（uke 的 27 线用的也是同一形状：domain 是 ASCII `VerifiedRoot.OwnerBootSet.v1`，不是摘要。）

### 6.19 上游这半个月攒下的东西，27 线能直接复用（2026-11-05）

`UniXNU-uke` 的 `origin/main` 已到 `fcf9fd3`（我们本地 checkout 还停在 `fefbf26`）。差别不是几千行
`MIGRATION-WORKLOG`，而是**两份正好卡在我们"剩余 archive"上的 24A446 ABI 材料**：

| 文件 | 行数 | 对我们意味着 |
| --- | --- | --- |
| `scripts/hid-device-24A446.json` | 842 | 27 的 HID 原始 ABI（vtable 目标 + diversity + 实例布局） |
| `scripts/legacy-display-24A446.json` | 2487 | 27 的显示原始 ABI（`IOMobileFramebufferLegacy` 全槽） |
| `scripts/gen-hid27-abi.py` | 28 | 上面那份的生成器 |
| `scripts/build-native-legacy-display-27.py` | — | 用上面那份夹具绑 arm64e 27 的 GOP 后端 |
| `scripts/check-legacy27-binds.py` | 67 | 绑定门禁 |
| `scripts/make-root-policy-27.py` | 209 | 27 的 policy 生成器（我们手写的那条路的对照实现） |
| `scripts/prepare-uke-gmu.py` | 632 | GMU 准备（GPU 线） |

**关键核对（今天做的）**：两份夹具都钉 `kernelcache_sha256 = 38e6d9fa…`。把我们自己的
`fw/ipsw/24A446/kernelcache.release.ipad17`（IM4P，24,013,888 B payload）用
`unixnu.ipsw.im4p_payload()` + `compression_tool -decode -a lzfse` 解开，得到
**80,035,840 B、`38e6d9fa5f789b8228f4617ef4c6d2a3fd8ab564abdc9b62920f1db53fc1780a`**
——与夹具的 pin **逐字节相同**。

也就是说：**同样的 iPad17,3 / 24A446 内核**，uke 已经把 27 的 HID 与显示原始 ABI 逐槽审过了。
我们不用再从 kernelcache 反汇编一遍，只要把夹具里的绝对地址按**我们自己的 slide** 重基
（夹具写的是 `collection_slide = 0x1f000000000`，槽位是绝对地址；槽号/diversity/实例大小与 slide 无关）。

这条把 §6.5/§6.7 的"HID 在 27 上没有捷径，必须重做 original-ABI 绑定"改写成了"绑定表已有，只差重基+接线"。

其余可复用的（不属 storage，先记着）：`package/DpuProbe.cpp`（`uke-dpu=1|2` 只读快照、
`uke-dpu-test=1..4` 受控写、5 s 自动回滚）、`docs/dts-reference/mdss-cliffs-map.md`（cliffs MDSS/DPU
寄存器地图，含上电序列×寄存器×源码行号）、`scripts/parse-dpu-log.py`、`docs/DPU-PORT.md`、`docs/GPU-IOS27.md`
（27 线 GPU 候选 **2026-11-03 用户真机验收通过**）。

显示分辨率（3.2K 能不能降）的结论单独写在 `docs/DISPLAY-RESOLUTION.md`：编译期没有开关，
机制是 DPU 的 VIG 缩放器，且它不是当前瓶颈。

### 6.20 两份夹具已落库并全项验证；顺带查出 27 的 prelink slide 是错的

**不需要"重基"**。夹具里的地址本来就是**未加 slide** 的（`0xfffffe00_0a288c74` 这种），
`collection_slide` 只是把它推到最终区的那一个常数。所以只要我们的 `[kernel.prelink] slide`
等于夹具的 `collection_slide`，夹具可以**原样使用**；数字一个都不用改。

**但上游 27 profile 的 slide 是错的。** `profiles/os/ipados-27.0.1-24A446/profile.toml` 写的是
`"slide" = "-0x1000000"` —— 那是 21H461 的值，照抄过来的：

| | 未加 slide | `-0x1000000`（profile 现值） | `0x1f000000000`（正确） |
| --- | --- | --- | --- |
| AMFI `_initializeAppleMobileFileIntegrity` | `0xfffffe00092551ac` | `0xfffffdff082551ac` ❌ | `0xfffffff0092551ac` ✅ |
| `IOMobileFramebufferLegacy` C2 | `0xfffffe000a288c74` | `0xfffffdff0a288c74` ❌ | `0xfffffff00a288c74` ✅ |
| 21H461 的同名锚点（对照） | `0xfffffff006ab4ae0` | — | `0xfffffff005ab4ae0` ✅ 同区 |

24A446 的 kext 链接在 `0xfffffe00…`，必须 +`0x1f000000000` 才落到 `0xfffffff0…`
（和 21H461 最终区相同）。用 `-0x1000000` 会把整批 kext 推到 `0xfffffdff…`，**每一个绑定槽都会指错**，
而且不会在构建期报错——只会在上机时表现为 vtable 全错。uke 的板子用的就是 `0x1f000000000`。

**门禁已就位**：`scripts/check-ipados27-abi-fixtures.py`（离线，无核依赖）。它把我们自己的
`kernelcache.release.ipad17`（IM4P）解开，然后逐项核对：

| 项 | 结果 |
| --- | --- |
| kernelcache sha256 == 两份夹具的 pin | ✅ `38e6d9fa…` |
| 两根字符串记录（`checks`）读回逐字节 | ✅ 10/10 |
| 显示 vtable 379 槽的 **chained fixup 解码 = 夹板的 target+diversity** | ✅ 379/379（3 个纯虚槽按 caller 侧记录） |
| HID vtable 207 槽同上 | ✅ 207/207 |
| 211 个槽名的 ptrauth discriminator | ✅ 211/211 |
| 限定绑定符号落在内核内（代码绑定须是 arm64e 函数入口） | ✅ 14/14 + 4/4 |

最关键的是 vtable 那一项：arm64e 内核里每个槽是一条 chained fixup，解码链必须逐槽复现夹具的
target **和** diversity。错一个槽、取自别的构建、手改过任何一处，都过不了。

夹具落在 `scripts/data/ipados-27.0.1-24A446/{legacy-display,hid-device}.json`，
来源是 uke `origin/main` 的 `scripts/{legacy-display,hid-device}-24A446.json`。

### 6.21 更正 §6.18：「2122 行重写」是个假前提，storage 只剩三小块

§6.18 把 UFS provider 记成"把 2122 行 `NativeUfs.cpp` 从 original-ABI 绑法改成公共接口"。**分层清点后不成立**：

- 我们的 `drivers/ShengUfs/NativeUfs.cpp`（2122 行）**本来就是公共 `IOBlockStorageDevice`**（`:1278`
  `class NativeUfsBlock : public IOBlockStorageDevice`），不是 original-ABI 绑定。
- 四个 provider 符号里**三个已经存在**并把实现写在这个文件里：`native_ufs_block_authenticates`（`:1503`）、
  `native_verified_block_root_authorized`（`:1657`）、`native_verified_block_volume_device`（`:1663`）。
- 第四个 `native_verified_block_manifest_sha384` 按 uke 的布局属于 `package/SourceRootBinding.cpp`，
  **不是驱动**——就是那份还没写的第三个设备源码。

**与 uke 的 27 驱动（`drivers/NativeUfs/NativeUfs.cpp`，2298 行）逐行比较**：总差异 304 行
（+240 / −64），其中 230 行是 `uke-dpu-scanout` 的显示 stage-1 窗口（**与 storage 无关**，独立开关）。
真正的差异只有四类：

| 差异 | 方向 | 处置 |
| --- | --- | --- |
| `UfsXnuCompat.h`（12377 的 `kmem_alloc`/`vm_map_protect` 兼容层） | 我们独有 | **保留**：uke 直接写 27 原生 API，我们靠兼容头同时支持 17/27 |
| `muyuLogCopy` + level 9 把日志写进 `vendor_boot_a` 尾部 | 我们独有 | **保留**：这块板没有 UART，这是唯一的落盘通道 |
| `adoptAndRead` 的 `payload` 写路径 | 我们独有 | **保留**：`native-ufs-store-write` 要用 |
| `gSmmuTakeover`（`native-ufs-smmu-takeover=1`） | 我们独有 | **保留**：固件留下的 UFS SMMU 映射必须接手 |
| DPU stage-1 窗口（+230 行） | uke 独有 | 以后要显示直扫时再拿，**不属 storage** |

也就是说 **uke 的驱动不是要"移植"的对象**——两者是同一个上游的两个分支，我们这份还多了四项板级功能。

**而且 uke 那份也没有 `native_ufs_source_create`**：全仓 grep（含 `UniXNU-sheng`、核心 `899fdcb`）只有
我们的 `SourceStorage.cpp` 声明 + 弱桩。所以这个工厂函数**没有现成实现可抄，得自己写**（约 6 行）。

**storage 的真实剩余工作量**：

| # | 动作 | 规模 | 依赖 |
| --- | --- | --- | --- |
| 1 | `package/SourceRootBinding.cpp`（uke 的 121 行） | 移植 + 改我们自己的 UUID/路径 | 无 |
| 2 | 驱动里补 `native_ufs_source_create()`，用 `-DNATIVE_UFS_SOURCE_API=1` 门控 | ~6 行 | 无 |
| 3 | 跑 `build-sheng-source-storage.py` 把 27 归档编出来 | 编译调试 | 1、2 |
| — | （以后）uke 的 DPU stage-1 窗口 | +230 行，门控 | 显示线 |

`build-sheng-source-storage.py`（核心里）已经把契约写死了：驱动路径 `drivers/ShengUfs/NativeUfs.cpp`、
三份设备源码 `SourceStorage.cpp` / `SourceRootPolicies.cpp` / `SourceRootBinding.cpp`、
编译开关 `-DNATIVE_UFS_SOURCE_API=1 -DNATIVE_VERIFIED_BLOCK=1 -DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1
-DNATIVE_VERIFIED_POLICY_HANDOFF=1`。而 `NATIVE_UFS_SOURCE_API` 这个开关**在两个仓库里都没有任何代码引用**，
也就是说它一直在等我们的工厂函数来兑现。

### 6.22 storage 归档已编出来（2026-11-05）

上面那三块都做完了，`libShengSourceStorage.a` 在 27 树上**编过并全符号就位**：

```
target  arm64e-apple-ios27.0
archive e0822d87c719be29…  1,653,320 B  12 个源
```

| 符号 | 定义数 | 来源 |
| --- | --- | --- |
| `native_ufs_source_create` | 1 | `drivers/ShengUfs/NativeUfs.cpp`（`#if NATIVE_UFS_SOURCE_API`） |
| `native_verified_block_root_authorized` | 1 | `package/SourceRootBinding.cpp` |
| `native_verified_block_volume_device` | 1 | `package/SourceRootBinding.cpp` |
| `native_verified_block_manifest_sha384` | 1 | `package/SourceRootBinding.cpp`（这个符号只有这条路径有） |
| `native_source_root_policies_load` | 1 | `package/SourceRootPolicies.cpp` |
| `native_ufs_root_step` | 1 | `drivers/ShengUfs/NativeUfs.cpp` |

过程中暴露的四个真问题（都是"构建期不报，上机才炸"那一类）：

1. **`UfsXnuCompat.h` 拿不到版本号**。组件构建由 `build-arm-driver.py` 传
   `-DXNU_MUYU_XNU_VERSION=12377`，而 `build-sheng-source-storage.py` 不传，于是走了 17 的 4 参数
   `kmem_alloc(kma_flags_t…)` 分支——27 头文件里没有 `kma_flags_t`。修法不是去改核心脚本，而是在驱动里
   按构建器自己的前提补默认值：那个适配器开头就有
   `parser.error('This adapter requires iPadOS 26 or later')`，所以它的 API 必然是 12377。
2. **弱桩会赢过真实现**。`SourceStorage.cpp` 里我给未移植符号留的 4 个 weak 桩，在静态库里和
   `NativeUfs.o` 的强定义并存；链接器为启动器符号拉这个 member 时，弱定义先把它占住——内核会带着
   一个返回 null 的桩 provider 起来。**已全部删除**，改成注释说明为什么不能留。
3. **`native_verified_block_root_authorized` / `..._volume_device` 会重复定义**。我们的驱动里有 17 线的
   "unverified 逃逸"版本，uke 把它们放在 `SourceRootBinding.cpp`。现在驱动那两个用
   `#if !NATIVE_UFS_SOURCE_API` 让位给验签版，17 线构建不受影响。
4. **`[root]` 的 `image_sha256` 不是 `.dmg.aea` 的摘要**。builder 比对的是
   `verification.json` 的 `image_sha256`（原始镜像映射），而容器摘要（`ada8410…` / `ece368c…`）是我们
   之前记进 `scripts/data/owner-os-policy-27.json` 的 `dmg_sha256`，两者不是一回事。board 的四个值是：

   | 键 | 值 |
   | --- | --- |
   | `os_volume_uuid` | `2F8E6EED-68D5-4261-B8A3-12571BAA2DD4` |
   | `os_image_sha256` | `8a12cdad43f8f0d33dadcf01a800a560222baf40c05f71374c460ba57ba3a739` |
   | `systemos_volume_uuid` | `F914C62A-AD00-47C6-8281-5746291C339A` |
   | `systemos_image_sha256` | `d5b4873d35c85f96eaafaa31c59602b485afe33b5067eff1af67ba31204cdb4a` |

   旁证：uke 的 `porting.toml` 记的 27 OS 窗口是 `8a12cdad…`，与我们这份独立生成的 policy 完全一致。

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
