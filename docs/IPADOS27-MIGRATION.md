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

**阶段 1 由此可以结案**：arm64e + 4K + BCM2837 + 12377 这套在本机能编，剩下的都是"接设备层/工具链"的问题。

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
