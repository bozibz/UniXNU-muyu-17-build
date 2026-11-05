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

### 6.23 27 线的内核第一次 prelink 出来了（2026-11-05 19:36）

```
kernel.macho   MH_FILESET  arm64e   95,715,328 B   sha256 5797bce105b9e87e…
39 个组件      storage 四个入口符号全部就位
loadable=False mapping_complete=False        ← --allow-missing 的 bring-up 态，正常
```

从"只有设备仓源码"到"可加载的 fileset 候选"，这一轮跨过的关卡（每一关都是构建期不报、上机才炸的那类）：

| # | 关卡 | 根因 | 处理 |
| --- | --- | --- | --- |
| 1 | `kc_image` 找不到 | 新核心不把 `kits/XnuPrelink` 铺进构建树 | 设备仓 `scripts/kc_image.py`（与 `native_prelink.py` 同一病） |
| 2 | `kernelcache.decompressed` 没有 | 归档阶段早于 prelink，而只有 prelink 会解 IPSW | 按 driver 的缓存约定放到 `<out>/ipsw/24A446/` |
| 3 | `libSoftwareAES27.a` 被硬编进链接行 | defconfig 里写死；`drivers/SoftwareAES` 树里没有，启动器也不引用 | 从链接行摘掉，注明恢复条件（**改 defconfig 必须同步改 `<out>/.config`**，driver 不重跑 defconfig） |
| 4 | 8 个 boot-log 符号无定义 | `BootLog.c` 的一半接口在 `libBravearkLastLog.a` 里 | 非 force_load 挂上去，只拉 `BootLogStubs.o`，不与库内 `boot_log_tick` 冲突 |
| 5 | `boot_progress_poll` 无定义 | 描述 archive（`BootProgress.c`）不在这一批 | 由 `SourceStorage.cpp` 提供；**必须强定义**，见下 |
| 6 | `Chained fixup imports are not supported` | see 下 | 消除 weak import |
| 7 | `Split-segment pointer disagrees with fixup` | 同一根因的另一面 | 同上 |
| 8 | 缺 `_unixnu_kernel_stack_shift` | 24A446 的 reviewed fixture 指名要这个原生符号 | 新增 `patches/xnu/native-stack-shift.patch`（`= 15`，即 32 KiB 栈对齐移位） |
| 9 | 回归策略挡下 3 处 stub | 我们的 archive 组合与 24A446 基线不同 | 逐条进 `prelink-regression.json` 并写明理由 |
| 10 | `required_original_branches` 不满足 | 那两条分支属于显示与 HID，这批不含 | 移到 `required_original_branches_deferred`，注明恢复时机 |

**第 6/7 关值得单独记**：arm64e 内核里，`__attribute__((weak))` 的**引用**会被链接器写成 chained **bind**（不是 rebase）。
`kc_image` 与 `fileset_layout` 只处理 rebase，所以只要内核里存在一条 weak 引用，prelink 就会连拒两关
（`imports` 计数、split-segment 指针）。我一开始去改 kit（让它跳过 bind），但那条路要走通必须把 bind 从
format 7 转写成 format 8 —— 那是另一个工程量。**正解是把 bind 消掉**：`package/BootLog.c` 里那七个可选钩子在
source-storage 构建（`-DNATIVE_UFS_SOURCE_API=1`，该构建必然带 `BootLogStubs.o`）改成强引用，
`boot_progress_poll` 改强定义，bind 就归零，kit 一字未改（我改的两处已 revert）。

**剩下的缺口**（都进了策略、有记录）：

| 类别 | 数量 | 内容 |
| --- | --- | --- |
| `missing` | 2 | `AppleARMWatchdogTimer` 1、`IOHIDFamily` 1 |
| `stubbed` | 30 | 分布在 15 个组件里（ApplePlatform 7、Image4 4、HID 3、WatchdogTimer 3、apfs 2、IOGPU 2、IOStorage 2 …） |
| `missing_address_space` | 1 | `output collection` |

下一步就是 27 根镜像（`build userdata`），然后才是上机。

### 6.24 27 根镜像也出来了（2026-11-05 19:5x）

```
userdata-root.img   15,753,805,824 B   sha256 981110dacccc9ee7…
SHNGROOT v2，4 个对象
  OS          9,613,344,768 B @ 0x100000       sha256 8a12cdad…  ✅ = policy
  SystemOS    6,016,729,088 B @ 0x23d100000    sha256 d5b4873d…  ✅ = policy
  OS.vroot       75,104,256 B @ 0x3a3b00000
  SystemOS.vroot 47,005,696 B @ 0x3a8300000
```

**这一块最有用的一条经验：AEA 镜像不需要 `ipsw` 工具。**
`decrypt_aea()` 要 `ipsw fw aea --key` 去 Apple 取密钥，而 macOS 自己就能挂：
`diskutil image attach --readOnly --mountOptions nobrowse <x>.dmg.aea` 会给出 `/dev/diskN`
（本次 `diskN` = 9,613,344,768 B，正好等于 policy 里的 `media_bytes`），
直接 `dd if=/dev/diskN of=<x>.dmg bs=8m` 就得到**与 policy 逐字节一致**的明文镜像：

| 成员 | aea sha256 | 解出的镜像 sha256 | 与 policy |
| --- | --- | --- | --- |
| OS `043-68950-668` | `ada8410…` | `8a12cdad…` | ✅ |
| SystemOS `043-69086-713` | `ece368c…` | `d5b4873d…` | ✅ |

再按 `decrypt_aea()` 的缓存约定写两份记录（`<x>.dmg.aea.json` 的 `sha256` = aea 摘要，
`<x>.dmg.json` 的 `aea_sha256`/`size`/`sha256`），并把它们放进 `<out>/ipsw/24A446/`
（不是 `fw/ipsw/…`——`Ipsw.cache` 在输出树里），构建就完全离线复用了。硬链接即可，不额外占空间。

**Data seed 已从镜像里取回**（`scripts/unpack-data-seed.py`，新）。种子只能来自设备原 Data 分区，
不在 IPSW 里；但 17 线把它 pack 进了 vendor_boot 的 payload FAT（`payload.py` 复制成
`EFI/UniXNU/Payloads/scratch-data.img`）。工具就是 FAT 遍历：
扫每 512 字节找 BPB（`0x55AA` + 合法 `bytes_per_sector/spc/fats`），按目录项走
`EFI/Unixnu/Payloads`，再沿 FAT 链读文件。两个坑：

- 名字是 **LFN**：`scratch-data.img` 不合 8.3，短名被压成 `SCRATCH~1.IMG`，只匹配短名会漏；
- 只按 sector 对齐扫 BPB 会误判大量假扇区，**必须在做任何除法之前**校验 `bytes_per_sector`。

结果：`fw/vendor_boot_nofs1.img` → `fw/data-seed.img`，1,093,760 B（FAT32 @0x1000，cluster 126）。

**新的阻塞：payload 要 sheng PMGR 整合。** 加入 `--data-ramdisk` 后 `build payload` 走到
`check_native_root()`：`native-ufs-root=1` 的板子必须同时带 `sheng-pmgr=2` 且内核里有
`_sheng_pmgr_run`，否则 AMFI 以 "No PMGR?" 恐慌。把 `build-sheng-platform-power.py` 加进归档后，
它内部的 `audit-amfi-pmgr-contract.py` 挡住两次：

1. 它只认 `build/xnu-prelink/native-owner-ufs-*-candidate.{macho,json}`（17 线的候选命名）；
2. 它用 `native_prelink.macho()` 读候选，而那只支持普通 arm64 Mach-O，**读不了 27 的 MH_FILESET**。

这条路先放下：`build-sheng-platform-power.py` 里写死了 17 线的 ApplePMGR vtable（实例 0x4b8a8、
槽 177..340）和一批绝对地址（`0xfffffff005ce26d8`、`0xfffffff004a22568` …），27 上等于要重做一遍
PMGR 的 original-ABI 移植，不是一个小门禁。

**正解在另一块板的仓库里**：uke 的 `board.toml` 写的是

```
[adt.defaults.u32]
"vmm-present" = 1     # 没有 Apple PMGR / SEP：如实声明虚拟平台，AMFI 就不等 ApplePMGRNub
```

这颗 SoC 本来就没有 Apple PMGR/SEP，所以这不是撒谎，是**如实上报**；`check_native_root()` 见到
`vmm-present` 就直接返回，`sheng-pmgr=2` 与 stand-in 一并都不需要（sheng 的 iPadOS 27 板同样如此）。
我们的 spike board 已照此设置，`sheng-pmgr=2` 与 `build-sheng-platform-power.py` 都撤掉。

**于是 payload 的最后一个阻塞变成了 device tree**（schema 2 的必然要求）：

```
KeyError: 'source'          # scripts/unixnu/devicetree.py:395, ctx.board['device_tree']['source']
```

schema 2 的 ADT 由 `[device_tree] source = <board.dts>` + `include_dirs = ["soc:dts"]` 经 `dtc` 编出，
uke 的 `dts/uke.dts` 只有 58 行（内存窗、8 个 CPU、timebase、hand-off 节点），SoC 侧
`sm7675.dtsi` 在 `UniXNU-sm7675-common`。我们需要等价的两件：`dts/muyu.dts`，以及
`UniXNU-sm8635-common` 里的 `dts/sm8635.dtsi`。取值可从 `fw/dtb_04.dtb` 反编译核对。

### 6.25 板级 DTS 已写，payload 走到 FAT 尺寸（2026-11-05 20:1x）

`dts/muyu.dts`（新，~85 行）不再引用 SoC dtsi——这颗 SoC 的硬件参数在 `make-muyu-adt.py` 里已经
是常量，直接照抄即可，比造一份 dtsi 更小、更难写错：

| 值 | 来源 |
| --- | --- |
| DRAM `0x80000000` + 2 GiB | `make-muyu-adt.py: DRAM_BASE` 与 board `dram_size`（`generate()` 会核对两者相等） |
| timebase 19.2 MHz、`cpu@0..700` | `TIMEBASE`、`--cpu-regs 0,0x100..0x700` |
| E/P 划分 4×A520 + 3×A720 + 1×X4 | 17 线的 EPSS 实测（d0 4×A520 / d1 A720 / d2 X4） |
| `arm-io` 0x17000000、GIC 0x17100000/0x17180000、`ufshc` | 同文件的 `ARMIO_BASE`/`GIC_*`/`UFS_HC` |

`ranges` 要写成 **6 个 32 位单元**（2+2+2 cells），写成 3 个是 12 字节、dtc 会警告且语义错。

随后 payload 一路走通 DTS → ADT → 内核打补丁 → 封存，停在 FAT 尺寸：

```
payload: kernel split 95,715,328 -> 47,857,664 + 47,857,664
unixnu: the payload files take 92 MiB; raise size_mib (now 60) in board.toml [payload]
```

**这里露出一个真问题：27 的 prelink 内核 95.7 MB，17 线只有 52.8 MB。**

| 段 | 17 线 | 27 线 |
| --- | --- | --- |
| `__TEXT_EXEC` | 8.6 MB | **51.7 MB** |
| `__PLK_TEXT_EXEC` | 20.4 MB | 不存在（并进去了） |
| `__PRELINK_TEXT` | 7.0 MB | 14.5 MB |
| `__LINKEDIT` | 7.5 MB | 13.0 MB |
| 合计 | 52.8 MB | 95.7 MB |

原生内核（prelink 前）只有 24.5 MB，所以 51.7 MB 的 `__TEXT_EXEC` 不可能全是原生代码——像是
schema 2 的 fileset 发射把原始 kext 代码并进 `__TEXT_EXEC` 时**重复写入**了一份。vendor_boot 分区
只有 96 MiB，92 MiB 的 payload 放不进去，所以这条必须先查清楚，而不是去调 `size_mib`。

**根因已定位（不是重复写入）**：`__PLK_TEXT_EXEC` 这个独立段只存在于 17 线走的
`emit_prelink.py`（`COALESCED = {'__PRELINK_TEXT': '__TEXT', '__PLK_TEXT_EXEC': '__TEXT_EXEC', …}`），
24A446 走的 fileset 路径（`fileset_layout.py`）**没有这个段**——它排布时直接把 `__TEXT_EXEC` 的
范围扩到覆盖原始模块代码（`self.reserve('__TEXT_EXEC', align(end_kept))`）。

也就是说 95.7 MB 是**真实内容**，不是 bug：

| 组成 | 17 线 | 27 线 |
| --- | --- | --- |
| 原生内核 `__TEXT_EXEC` | 8.6 MB | 10.2 MB（prelink 前实测） |
| 原始 kext 代码 `__PRELINK_TEXT` | 7.0 MB | 14.5 MB |
| 容器 `__TEXT_EXEC`（并入 kext 文本） | — | 51.7 MB |
| `__LINKEDIT` | 7.5 MB | 13.0 MB |
| 合计 | 52.8 MB | 95.7 MB |

27 线挂了 39 个组件、`__PRELINK_TEXT` 是 17 线的两倍，文件自然翻倍。**vendor_boot 只有 96 MiB，
92 MiB 的 payload 放不进去**，所以这不是调 `size_mib` 能解决的。

uke 的 README 里写着答案：「160MiB 原 FAT 使用标准 UEFI 压缩运输，vendor_boot 采用压缩文件」——
他们的 vendor_boot 只有 44,859,392 B 却装着同样量级的 payload，靠的是 **UEFI 的解压协议**
（`uefi-uke` 支持解压）。所以 27 线要么给自己的 UEFI 加压缩支持，要么裁掉首启不需要的组件。

### 6.26 payload 已产出，但 94 MiB 装不进 vendor_boot 的安全布局（2026-11-05 20:2x）

`size_mib` 从 60 提到 94 后 payload 出来了：

```
payload/payload.img  94 MiB
```

（核心的 vendor_boot 写入器是**替换** vendor ramdisk，不是追加片段，所以 payload 独占分区大小。
之前那句"96 MiB 放不下 92 MiB"是我把 `size_mib` 的语义当成了片段模式，已纠正。）

但真正装箱的时候撞上**我们自己 2026-10-26 的实测结论**（写在 `scripts/patch-vendor-boot.py` 的文档里）：

| 布局 | 结果 | 上限 |
| --- | --- | --- |
| 默认（payload **替换** vendor ramdisk） | 镜像最小，但丢掉 33.8 MB 的 Android ramdisk，**"这块板就不充电了"**（2026-10-26 实测，接哪都不充） | 96 MiB |
| `--fragments`（保留原 ramdisk 作 fragment 0，payload 作 fragment 1） | 充电正常 | **≈61.9 MiB** |

所以 94 MiB 只能走"不充电"的布局。**payload 必须压到 ≈62 MiB 以下**，或者改固件做压缩运输。

**先排除了两个"看起来能省"的方向**（2026-11-05 实测）：

- **不是符号表**：`kernel.macho` 里**没有 `LC_SYMTAB`**，`__LINKEDIT` 那 13 MB 是 chained fixups
  与导入表，剥符号省不出东西（剥掉会直接坏掉加载）。
- **不是重复写入**：`__TEXT_EXEC`（51.7 MB）里零字节只占 **8.5%**，而且 `__PRELINK_TEXT` 的
  两段 4 KB 探针在 `__TEXT_EXEC` 里**都找不到**——两边内容不同，不是同一份抄了两遍。

同时确认闭包是**有选择性**的：`unselected_entries_dropped = 276`、只保留 39 个组件（`__TEXT_EXEC`
里原生只占 10.2 MB，其余约 41 MB 是原始 kext 被并进来的文本）。所以 95.7 MB 是 24A446 那些
kext 的真实体积——iPadOS 27 的 kext 本来就比 21H461 大一截（`sandbox` 2.8 MB、
`AppleM2ScalerCSCDriver` 3.1 MB、`apfs` 2.3 MB …）。

结论：**靠瘦身很难从 95.7 MB 砍到 60 MB**（要砍掉一半以上，等于掏空这个系统）。
三条路的取舍是硬件层面的决定，需要人来定：

| 路 | 做法 | 代价 |
| --- | --- | --- |
| A | vendor_boot 用默认布局（payload 替换 Android ramdisk） | **平板不充电**（2026-10-26 实测），要充电得刷回 17 线镜像 |
| B | 给 `MuyuPayloadBlockIo` 加解压（`BaseUefiDecompressLib` 已在 `MsCorePkg.dsc:56` 链上），payload 压缩运输 | 要 edk2 检出并重编固件；一次性投入 |
| C | 砍 closure 到首启最小集 | 大概砍不到 62 MiB；每砍一批都要试开机 |

`unixnu bootimg` 这条路还要 `--fd`（shim + 固件卷），而本地 `Build/muyuPkg/…/SILICIUM_UEFI.fd`
（3 MB，DEBUG）不符合它的 ARM64 头校验，且没有 edk2 检出、无法重编固件。所以装箱改用设备仓的
`scripts/patch-vendor-boot.py`（拿现有 vendor_boot 的 header/DTB/table 重排，不需要固件）。

### 6.27 选定 A：vendor_boot 已出（95.8 MiB，落在 96 MiB 里）

用户 2026-11-05 20:2x 拍板走 A（payload 替换 Android ramdisk，接受期间不充电）。

```
vendor_boot_27.img  100,462,592 B (95.8 MiB)   分区 96 MiB，余量 200,704 B
  ramdisk 98,566,144 @ 0x1000
  dtb      1,885,961 @ 0x5e01000
  table    108 B @ 0x5fce000（1 项）
  bootconfig 205 B @ 0x5fce06c
```

`patch-vendor-boot.py` 的尺寸门禁是拿 `len(base)` 当分区上限，而我们手上的 base 是 64 MB 的构建产物
（`vendor_boot_nofs1.img`），所以先把 base 零填充到 96 MiB 再打包——填充只影响未使用的尾部，
header/DTB/bootconfig/table 一个字节都不变。

**这套产物三件就齐了**：

| 产物 | 大小 | 去向 |
| --- | --- | --- |
| `vendor_boot_27.img` | 95.8 MiB | `vendor_boot` |
| `payload` 已内含 | — | （在 vendor_boot 里） |
| `userdata-root.img` | 15.75 GB | `userdata` |
| `fw/Mu-muyu.img`（沿用 17 线的） | 2.19 MB | `fastboot boot` 用，UEFI+shim 不变 |

刷写顺序（设备接上后）：`root-image` 写 userdata → `xnu` 写 vendor_boot 并启动 UEFI。
两条都走 `tools/muyu-flash-mode.sh`，它会把每一步记进 `logs/flash-ledger.tsv`。

注意：A 布局**会覆盖 userdata**（17 线那套），且**期间平板不充电**；恢复办法是
`tools/muyu-flash-mode.sh restore-stock --yes` 刷回 `fw/stock/` 里的原厂镜像。

### 6.28 首次上机：`Not Found` → `Unsupported`，卡点是内核格式（2026-11-05 20:5x）

刷写过程全部成功（userdata 15.2 GB sparse，345.97 s，rc=0；vendor_boot 95.8 MiB，OKAY），
UEFI 菜单两次都起来了。两次点"1"的结果：

| 轮次 | 菜单报错 | 含义 |
| --- | --- | --- |
| 第 1 次（payload 里是 `kernel.macho.0/.1`） | `returned: Not Found` | 启动的 `boot.img`（2026-10-26）里 DarwinBoot 比 `.0/.1` 拼装支持（commit `1598841`）**老**，它只找 `kernel.macho` |
| 第 2 次（`split_kernel = false`，单文件 95.7 MB） | `returned: Unsupported` | 文件找到了，但**解析不了** |

第二轮把根因钉死了：

```
17 线 kernel.macho  filetype 2   MH_EXECUTE    arm64
27 线 kernel.macho  filetype 12  MH_FILESET    arm64e
DarwinBoot 里 "FILESET" 出现次数：0
```

DarwinBoot **完全没有 fileset 解析**，所以 24A446 的 `MH_FILESET` 内核一律 `EFI_UNSUPPORTED`。
这也解释了为什么一定要 fileset 发射器：24A446 的 kernelcache 本身就是 fileset，17 线那套
`emit_prelink.py`（产出 `MH_EXECUTE` + `__PLK_TEXT_EXEC`）吃不下它。

**两条路**：

| 路 | 做法 | 代价 |
| --- | --- | --- |
| 1 | 给 DarwinBoot 加 `MH_FILESET` 载入（遍历 `LC_FILESET_ENTRY`，找到内核入口，按段加载后再交 boot_args） | 需要 EDK2 检出（本机 `Mu_Basecore` 为空、全盘无 `BaseTools`）+ 重编并刷固件；uke 的 `uefi-uke` 是现成参考 |
| 2 | 让 27 内核回到 `MH_EXECUTE` | 要和 24A446 fileset kernelcache 的格式对抗，等于给旧发射器补一套 fileset 输入解析 |

推荐 1：和上游对齐、两种内核格式只留一种，且 uke 已经在同硅片上做通了。

已同时修好两处构建侧问题（都留在仓库里）：`split_kernel` 板级开关（默认 true，27 板显式 false）、
以及 `seeds`/`payload` 相关的脚本遮蔽。

### 6.29 换新固件后：27 内核第一次真正跑起来（2026-11-05 21:17）

拉齐 EDK2（`ghfast` 克隆 tianocore/edk2，补 `BaseTools/Source/C/BrotliCompress/brotli` 子模块）和
`Mu_Basecore`+它的子模块（`mipisyst`/`libfdt`/…），`unixnu build firmware` 编出 3 MiB 的
`SILICIUM_UEFI.fd`。过程中还修了 `BootShim.S` 的 `image_size`：原来写的是 `FD_SIZE`，少算了 112
字节的 shim，`bootimg` 因此拒绝打包——改成 `(_Payload - _Head) + FD_SIZE`。

新固件的 `DarwinBoot` 能解析 `MH_FILESET`（`ParseKernelHeader` 里 `filetype != 2` 之外还接受
`header == 0` 处的 `12`），于是 payload 恢复分片 `.0/.1`，`boot.img` 重打为 2,134,016 B。

**结果：菜单不再报错，内核进入并打出 panic 屏。**

```
panic: init.c:1107 Assertion failed: !((pmap->type == ARMPTE_EMPTY) ||
       (ptep->is_ro & ARMPTE_HINT) == (pmap & ARMPTE_HINT))
       at pc 0xfffffff0086a45b0, lr 0xfffffff00082ea6e
```

两条已确认的事实：

1. **这条断言不在我们的 XNU 源码里**——`ARMPTE` 在整个 `.cache/xnu-work` 里一次都没出现，
   `init.c` 也不是我们的文件。它来自**被 prelink 进来的苹果原始代码**（pmap/SPTE 相关），
   也就是说 27 的原始 kext 在我们的 4 KiB 内核上走进了它 16 KiB 前提的分支。
2. `pc` 落在我们镜像的 `__TEXT_EXEC`（基址 `0xfffffff008500000`）里，`lr` 在段外——
   说明调用链起点也不在本内核里，同样指向原始代码。

下一步是把 `pc`/`lr`/`far` 符号化（需要正对屏幕的完整 panic 文字，或把 panic 文本落盘读回），
再决定是补 pmap/SPTE 的 4 KiB 适配，还是调整 prelink 对这几个 GOT 槽的绑定（参考
`kits/XnuPrelink/fixtures/fileset-24A446-symbols.json` 里 `_unixnu_kernel_stack_shift` 那种逐槽绑定的做法）。

### 6.30 修正：panic 在我们自己的代码里；`use_contiguous_hint=0` 越过后到 CPU 拓扑（2026-11-05 21:2x）

§6.29 把断言判成"苹果原始代码"是**读错了**：panic 里的 `ARMPTE` 其实是 `ARM_PTE_*`，
定义在**我们自己的** `osfmk/arm64/proc_reg.h`。真正的断言：

```c
osfmk/arm64/arm_vm_init.c:1107
assert(!kva_active || (ppte[i] == ARM_PTE_EMPTY) ||
       ((ppte[i] & ARM_PTE_HINT) == (ptmp & ARM_PTE_HINT)));
```

`ARM_PTE_HINT` 是连续项提示（128×4 KiB = 512 KiB 一起映射）。`arm_vm_init` 在 `kva_active`
为真时重新映射内核 `_start.._end`，撞上"引导阶段已映射、但没带 hint"的页。17 线把原始 kext 放在
独立的 `__PLK_TEXT_EXEC`，27 的 fileset 发射把它们并进 `__TEXT_EXEC`，`_end` 覆盖的区间因此跨进
那片区域——同一条断言在 17 线不炸，就是布局差异造成的。

`use_contiguous_hint` 是一个**启动参数**（`arm_vm_init.c:2090` `PE_parse_boot_argn`），所以不用改代码：
加 `use_contiguous_hint=0` 后**越过了这个断言**，panic 前进到下一步。

**新 panic**：

```
"CPU configuration in device tree is invalid" @AppleARMCPU.cpp:111
TCR=0x00080022a51a251a  SCTLR=0x00000000fc54799d
pages=0x00000000012558f  free=0x000000000124ccf
```

进到苹果 AppleARMCPU 解析设备树的阶段了。我们的 `dts/muyu.dts` 给六个核写
`cortex-a520`、三个 `cortex-a720`、一个 `cortex-x4`；17 线的 ADT 和另一块 27 板
（uke）都是**所有核同一个 `compatible`**。已改成统一 `arm,cortex-a720`（E/P 仍由
`unixnu,cluster-type` 表达），等设备回到 fastboot 再刷。

### 6.31 CPU 拓扑：AppleARMCPU 的校验清单已从二进制里挖出来（2026-11-05 21:2x）

统一 `compatible`、去掉 `cluster-type` 都没用，panic 一模一样。于是直接去二进制里找它的校验代码——
`AppleARMCPU::_validateConfiguration` 的报错字符串就排在 panic 字符串前面：

```
reg-private   l2-access-penalty   cluster-type   l2-cache-id   l3-cache-id
l2-cache-size l3-cache-size       no-aic-ipi-required
_max_cpus? /cpus   cpu-id   interrupts
function-cpu_idle  function-platform_cache  function-ipi_dispatch
function-ipi_dispatch_other  function-enable_core  function-error_handler
```

以及它每条失败分支的原文：

| 原文 | 含义 |
| --- | --- |
| `fromPath failed to find /cpus in device tree` | 找不到 `/cpus` |
| `Invalid cpus/%s property length: %d` | `/cpus` 上某属性长度不对（`max_cpus`） |
| `Max CPU count is zero` / `Actual CPU count (%d) greater than max (%d)` | CPU 数量与 `max_cpus` 不符 |
| `CPU doesn't use AIC IPI's but function-ipi_dispatch defined` | AIC IPI 与 `function-*` 矛盾 |
| `Length for CPU ordinal %d '%s' property is %d should be 0x%lx` | 某属性长度不对（`cpu-id`） |
| `CPU id %d ... out of range [0..%d]` / `Duplicate CPU entry for id %d` | `cpu-id` 越界/重复 |
| `CPU %d interrupts[2] not equal to interrupts[0]+1` | `interrupts` 三元组不成对 |
| `Missing '%s' property for CPU %d` | 缺属性 |
| `%s for CPU %d (%d) must match interrupts[0] (%d)` | `cpu-id`/`function-*` 与 `interrupts[0]` 不一致 |

我们的 `dts/muyu.dts` 一个都没有（生成器只发 `reg/state/device_type/compatible/timebase-frequency`，
schema 2 再加 `cluster-type`），而 17 线的 ADT 也没有——**所以 17 线能起、靠的不是满足它，而是让这个驱动不启动**
（`ArmCpu` 的 `IOProbeScore = 2000` 先拿走 provider）。我们这份 27 里有 `ArmCpu`（核心里 38 个符号），
但苹果那个还是起来了。

**下一步二选一**（都需要下一步动作，不能靠猜）：

1. **解码真树**：`fw/ipsw/24A446/DeviceTree.j820ap.im4p` 的 payload 是 `bvx2` 容器（不是裸 FDT，
   里面找不到 `d00dfeed`），解出来后照抄 `/cpus` 节点的属性集，把 Apple 的校验满足掉；
2. **让苹果驱动不启动**：加一条内核补丁，让 `AppleARMCPU` 在 `_validateConfiguration` 之前就
   `return false`（或让 `AppleARMPlatform` 不发布 /cpus 的 platform nub），把 CPU 枚举完全交给
   `ArmCpu`——这也更接近 17 线实际生效的路径。

我倾向 2：我们本来就有自己的 `ArmCpu`，让苹果那套 Qualcomm 上不存在的 AIC/PMGR 校验跑起来再去满足它，
是把复杂度往错的方向加。

另外修了一个**设备仓库脚本遮蔽核心**的坑：`patch-xnu-entry-probe.py` 的 `kernel_symbols()` 解析
`llvm-nm` 输出时不校验首列是十六进制，fileset 内核会在每个成员前打 `Symbols:` 横幅，于是
`int('Symbols', 16)` 抛异常。改的是 `UniXNU-muyu/scripts/` 那份（树里实际用的是它），核心保持原样。

### 6.32 走通第 2 条：改 personality，不碰内核（2026-11-05 21:3x）

选了 6.31 的第 2 条，但**答案不在内核补丁里，而在 personality plist 里**——而且姊妹 27 端口
（`UniXNU-27/docs/devices/sirius.md` §"the next stop was AppleARMPlatform's CPU class"、
`rmx1971.md` §Configuration）早就把这条路走通了，我们只是没查。

**机制**（sirius 的实测结论，我们的二进制与它一致）：

- 苹果的匹配是 `IODTPlatformExpert` **按前缀**比 `IONameMatch`，所以 `cpu` 会把 `cpu0..cpu7` 全认下来；
- `AppleARMCPU` 的 personality 就一条：`IOClass = AppleARMCPU`、`IOProviderClass = IOPlatformDevice`、
  `IONameMatch = cpu`、`IOProbeScore = 100`；
- 我们的 `ArmCpu` 是 `IOProbeScore = 2000`，**但分数根本救不了**：sirius 加到 40000 也没赢；
- 缺的 `/cpus/max_cpus`、`cpu-id`、`interrupts`、`function-*` 是苹果设备树的东西，Qualcomm 板上没有，
  满足它等于给一块没有 AIC 的板子伪造 AIC 语义——方向错。

**做法**：把 `__PRELINK_INFO,__info` 里那份 XML plist 中 `AppleARMCPU` 的 `IONameMatch` 从
`cpu` 改成 `xpu`。三个字符换三个字符，**镜像里一个 offset 都不动**（plist 长度不变，
`__LINKEDIT`/chained fixups/segment 布局全部原样）。

实现是两个新文件（核心仓 `UniXNU-27`）：

| 文件 | 作用 |
| --- | --- |
| `scripts/patch-prelinked-personality.py` | 定位 `__PRELINK_INFO,__info` → 解析 plist → 找到发布该 personality 的唯一 kext → 同长度改写 → 回读校验 |
| `scripts/test-patch-prelinked-personality.py` | 合成一个单段 Mach-O 做单元测试：只改一个字节、幂等、长度不等/值不符/找不到 personality/没有 `__PRELINK_INFO` 全部拒绝 |

自检比“写对了”更严格：改写后把那个值再改回旧值，整份 plist 必须与原文**逐字段相等**；
文件里变化的字节必须全部落在目标 value 的窗口内，且个数等于新旧值的字符差异数
（`cpu`→`xpu` 只有 `c`→`x` 一个字节）。

board.toml 侧只有一个开关（`[payload] native_cpu_owner = true`），payload 步骤在写完 `kernel.macho`、
跑 entry-probe 补丁**之前**调用它，报告写进 `payload.json` 的 `personality_rewrites`。门禁留在
板级而不是核心：真 Apple 树（靠 AppleARMCPU 枚举 CPU 的板子）不能开这个。

**本次产物**：

| 产物 | 值 |
| --- | --- |
| `fw/vendor_boot_27.img` | 100,462,592 B，`def7e2cb…`（布局与 6.27 完全一致：ramdisk 98,566,144 @0x1000、dtb 1,885,961 @0x5e01000） |
| payload | `8f2812ac…`，94 MiB，`personality_rewrites` 已记录 |
| 旧镜像留档 | `fw/vendor_boot_27-6.31.img`（`ad4b…`，panic 现场那一版） |

**顺手确认的两件事**：

- `payload.json` 的 `kernel_sha256`（`5797bce1…`）没变：这次没有重编内核，改的只是 payload 里的副本。
  `payload_kernel_sha256`（`a1037870…`）也没变，因为内核被切成 `.0/.1` 两半、它算的是前半，
  而改动的那一个字节在 92 MB 处、落在 `.1` 里——所以这两个字段不能当“payload 有没有变”的判据，
  要看 `payload_sha256`。
- ADT 的 `adt_sha256` 每次构建都会变，**不是**我们的改动引起的：`/chosen/random-seed` 是生成器每次
  现取的 64 字节随机数（`cmp` 两个 ADT 只有那 64 字节不同）。要拿 ADT 做逐字节对比，得先排除这个字段。

### 6.33 卡点定位：`bsd_init` 内部、`IOKitBSDInit()` 之前（2026-11-05 22:0x）

6.32 之后重启，**不再 panic、也不再冻在 CPU 校验**，但屏幕停住不走了。定位过程全部用
`patch-xnu-entry-probe.py --fb-probe-after`（在某个 `bl` 处插入“画一行字 + 停住”的代码，走 panic
渲染器，所以**方向正常、能读**），payload 重建 ≈ 1 分钟，不需要重编内核。

**先纠正一个误判，这条对以后每次拍照都重要**：那张“panic 前必现”的屏幕，上面的字
（`Exiting boot services. A green bar across the top` / `SecurityLock::lockType: SOFTWARE_LOCK … LockCapsule` /
`MBI dump table info not found, Status = (0x0)` / `App Log Flush : 0 ms`）**全是 DarwinBoot 自己打的**
（`DarwinBoot/drivers/DarwinBootPkg/Driver/DarwinBoot.c:1021` 及同文件 DEBUG 输出），不是内核、也不是 panic。
内核此时什么都还没画——所以“屏幕上只有 loader 的字”= 内核还没走到能画的地方。

另一个坑：`--fb-probe-after` **会故意关掉入口的 stage 1/2 彩条**（探针要在此后作画，彩条会被覆盖）。
所以“没有彩条”不等于“内核没起来”，别再拿彩条当判据。

| 轮次 | 探针位置 | 结果 | 结论 |
| --- | --- | --- | --- |
| probe1 | `_kernel_bootstrap_thread` 里 `_initialize_screen` 之后 | 出现 `PROBE OK: past PE_init_iokit` | IOKit 匹配全部走完（`ArmCpu::start`、GIC、所有被匹配 kext 都成功） |
| probe2 | `_bsd_init` 里 `_IOKitBSDInit` 之后（+0x680） | 无字 | 卡点在 `bsd_init` 内部、`IOKitBSDInit()` 之前（但也可能是“那时画不出来”） |
| probe3 | `_kernel_bootstrap_thread` 里 `_PE_lockdown_iokit` 之后（+0x3d0，紧邻 `bsd_init` 调用） | 出现 `PROBE OK: bsd_init next` | 渲染器在很晚阶段仍能画；**内核确实进了 `bsd_init`** |
| probe4 | 同 probe2，但关掉 `offline-apfs-load` / `offline-mobile-graphics-load` / `offline-mobile-graphics-client`（三个运行时 `OSKextLoadKextWithIdentifier` 的开关） | 无字 | **三个运行时 kext 加载无罪**，排除 |
| probe5 | `_bsd_init` 里 `_kauth_init` 之后（+0x60，`bsd_init` 最开头） | 待测 | 用来排除“`--in _bsd_init` 的补丁本身有问题 / 进 `bsd_init` 后画不出来” |

`bsd_init` 在 `_IOKitBSDInit`（+0x680）之前的可疑清单（按调用顺序，全部是核心初始化，与设备无关）：
`kauth_init`、`mac_policy_initbsd`、`kmem_suballoc`、`bsd_bufferinit`、`IOKitInitializeTime`、`vfsinit`、
`proc_uuid_policy_init`、`mbinit`、`aio_init`、`pthread_init`、`domaininit`、`skywalk_init`、`necp_init`、
`memorystatus_init`、`sysctl_mib_init`、`kminit`。

**顺带记录一个真实差异**：27 内核里 `AppleKeyStoreState` / `sheng-aks` 的出现次数是 **0**（17 线是 2），
也就是 AKS 那一支没有链进 27 内核——`sheng-aks=1` 目前是空转。`NativeUfs`（117 处）和
`native-ufs-root` 是在的，所以存储组件本身链上了。

#### 6.33.1 再往下两层：`kperf_init_early()` → `ml_wait_max_cpus()` → 没有 CPU 中断控制器（2026-11-05 22:2x）

probe6（`_vm_set_restrictions` 之后，`--in _kernel_bootstrap_thread`）**有字**，于是最后那段
`_vm_set_restrictions` / `_kperf_init_early` / `_bsd_init` 的三明治被切开：

```
+0x3d0  _PE_lockdown_iokit      probe3 在此停住（有字）
+0x3e0  _vm_set_restrictions    probe6 在此停住（有字）
+0x3e4  _kperf_init_early       ← 卡在这里
+0x3e8  _bsd_init
```

`_kperf_init_early` 只有 132 字节、四个调用，第一个就是 `ml_wait_max_cpus()`：

```c
void kperf_init_early(void) { ml_wait_max_cpus(); if (kperf boot arg) kperf_kernel_configure(...); }
```

`ml_wait_max_cpus()`（`osfmk/arm/machine_routines_common.c:1383`）等的不是“CPU 数量”，而是
`ml_set_max_cpus()` 被调用——`max_cpus_initialized` 从 `MAX_CPUS_WAIT` 变 `MAX_CPUS_SET`。
全内核只有两处调用它：

| 调用点 | 谁用 |
| --- | --- |
| `iokit/Kernel/IOCPU.cpp:632`（`IOCPUInterruptController::initCPUInterruptController`） | **我们的 `ArmCpu` 走这条** |
| `iokit/Kernel/arm/AppleARMSMP.cpp:167` | 苹果 SMP 胶水，我们没有 |

所以对这块板子，条件等价于“`ArmCpu::start()` 必须走到 `controller->initCPUInterruptController()`”。
而 `ArmCpu::start()` 开机核路径是：

```c
if ((!skipGic && !prepareGic ()) || !IOCPU::start (provider)) { return false; }   // ← prepareGic 失败就 return
...
controller = new IOCPUInterruptController;
controller->initCPUInterruptController (cpuCount, 1);     // ← ml_set_max_cpus 在这里
```

`start` 返回 false 只表示这个驱动没启动成功，**IOKit 会继续往下走**——这正好解释了
“probe1 显示 `PE_init_iokit()` 已返回” + “`bsd_init` 永远到不了”：中断控制器没建，`ml_set_max_cpus`
没人调，`kperf_init_early` 就永远睡着。

`cpus=1` 实测**无效**（关掉后画面不变），印证了这不是“CPU 数量不够”，而是“没人叫醒等待者”。

下一步用现成开关验证：`armcpu-skip-gic=1` 绕过 `prepareGic()`，再看 `_bsd_init` 开头的探针
（`PROBE OK: bsd_init entered (skip-gic)`）。有字 = 失败点就在 `prepareGic()`（`mapGic` /
`wakeRedistributor` / `initDistributor` 的 `waitClear`），没字 = 失败点更靠后
（`IOCPU::start` / 控制器分配 / `ml_processor_register`）。

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
