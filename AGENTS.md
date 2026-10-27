# AGENTS.md — 接手本工作区的强制规则

本文件是给**任何接手本工作区的 AI / 协作者**的入口约束。开始工作前必须读完本文，
并按 §1 的顺序读对应文档。**违反 §2 / §5 的行为视为错误，不是风格问题。**

---

## 0. 这是什么工作区

在**小米平板 7 Pro（代号 `muyu`，SM8635，骁龙 8s Gen 3）**上运行 **UniXNU + iPadOS 17.7.11（21H461）**。

| 项 | 值 |
| --- | --- |
| 工作目录 | `/Users/diannao/pad7pro-xnu`（记作 `$W`） |
| 设备 serial | `a20248f7`（bootloader 已解锁，`verifiedbootstate=orange`） |
| 当前状态 | **能启动到真正的 iPadOS 桌面**（2026-10-27）|
| 保底镜像 | `fw/vendor_boot_lite.img` = `c58ad307…`；git tag `muyu-desktop-works` |
| iPad 根镜像 | `fw/userdata-backup/userdata-root.sparse`（`4c90d7a7…`，8.9 GB）|

---

## 1. 先读什么（按顺序）

1. `NEXT-PROMPT.md` —— 现状 + 下一步（顶部有官方优先规则和一键粘贴的精简版）
2. `HANDOFF-2026-10-27.md` —— 最新完整交接（§A–§J 是最后一个有效工作日的流水）
3. `PLAN.md` §0 —— **官方优先原则（完整版）**；其余章节是历史阶段记录
4. `HANDOFF.md` —— 早期完整细节（§0.2 列了哪些历史结论已作废，别照抄旧结论）

---

## 2. 官方优先（最高优先级，完整版见 `PLAN.md` §0）

**官方网站 <https://unixnu.com> 与官方仓库 <https://github.com/UniXNU/UniXNU> 是主体；
本工作区的实现是补充，不是替代。**

1. **先查后写** —— 实现任何驱动/机制前，先在网站与官方仓库搜索是否已有实现（含
   `/reference/components` 的规范 ID）。**有 → 接入或移植**；没有 → 才自己写，并在文档里写明
   "查过什么、为什么不复用"。
2. **用规范名** —— `ShengUfs` / `GopFramebuffer` / `AppleKeyStoreState` / `Adreno740` / `AdrenoMetal` /
   `ShengNovatekTouch` / `QcomGeniSpi`。`NativeUfs` / `NativeMobileDisplay` / `NativeKeyStoreEvents`
   只是历史别名。对照表：网站 `/porting/migration`。
3. **改动落在补丁/组件层** —— 不整目录复制上游文件后原地改；优先 `patches/xnu/series`、组件 pin、
   板级 `package/`。
4. **开工先对表** ——
   ```bash
   git -C $W/UniXNU fetch
   git -C $W/UniXNU rev-list --left-right --count origin/main...HEAD   # 落后 / 领先
   ```
   把这个数字写进当天交接。**落后 > 50 个提交就安排一次对齐。**
   （2026-10-27 审计：当时落后 **153** 个提交，设备仓库还是**格式 1**，另有 **49 个文件未提交**。）
5. **冲突以官方为准** —— 本地行为与官方文档/实现冲突时，先在 `PLAN.md` 记录差异与理由再决定偏离，
   **不允许静默偏离**。

### 2.1 已知"我们造过、上游已有"的东西 —— 不要再重造

| 本工作区的实现 | 官方已有 |
| --- | --- |
| 屏幕 dump 通道（panic → 冻屏 → 拍照）| `package/BootLog.c` + `BootReset.c` + `package/linux-log/` + `scripts/read-sheng-log.py`（全自动，网站 `/porting/log-capture`）|
| CPU 逐帧扫描拷贝（~1.4 fps）| 显示 `GopFramebuffer`；加速 `Adreno740` + `AdrenoMetal`（网站 `/porting/gpu`）|
| 自研 AppleKeyStore（`NativeKeyStoreEvents`）| `AppleKeyStoreState` |
| 自己写的 SoC 参数 | 共用 SoC 支持 `UniXNU-sm8550-common` |

---

## 3. 构建与上机（当前最简路径）

```bash
W=/Users/diannao/pad7pro-xnu; P=/opt/homebrew/bin/python3.12
export PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH

# 内核（只有改了 package/ drivers/ patches/ 才需要，约 5–12 分钟）
cd $W/UniXNU && $P unixnu build kernel -D ../UniXNU-muyu \
   --kernelcache out/ipsw/21H461/kernelcache.decompressed

# payload + vendor_boot（只改 board.toml 时只需这两步，约 1 分钟）
$P unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
   --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
   --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr
$P ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/stock/vendor_boot.img \
   --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_<名字>.img

# 上机
cd $W && fastboot --set-active=a
fastboot flash vendor_boot fw/vendor_boot_<名字>.img
fastboot boot UniXNU/out/muyu/boot/boot.img
```

`board.toml` 的 boot-args 有**约 1000 字节上限**，加参数前先数长度。
产物一律记录 `sha256` 并写清"哪个配置最好"。

---

## 4. 记录义务

- **每次改动写进当天的交接文档**（`HANDOFF-<日期>.md`，按"一节一轮"追加，编号越大越新）。
- **不要把工作留在未提交状态。** 每个可启动的配置至少一次 commit，必要时打 tag。
- 产物命名：`fw/vendor_boot_<用途>.img`；日志放 `logs/`（照片、构建日志、刷写日志）。
- 结论要带证据：数字（sha256、秒数、计数器）和复现命令，不写"应该/可能"。

---

## 5. 硬性禁令（每条都有过事故）

1. **`fastboot getvar` 不要接 `head` / `| grep -m1`** —— SIGPIPE 会让 fastboot 半路退出，**设备端
   fastboot 服务随之卡死**，`getvar`/`flash` 全部挂起，拔插 USB 也救不回，只能强制关机重进。
   正确做法：`fastboot getvar all > /tmp/fb.txt 2>&1` 之后再 grep。
2. **不要在设备 UI 里点「抹掉」/ 还原** —— `com.apple.mobile.obliteration` 已被 `sheng-lite=1` 停掉，
   点了大概率卡死；要干净重刷用
   `fastboot flash userdata fw/userdata-backup/userdata-root.sparse`。
3. **不要砍 `offline-legacy-*` / `offline-mobile-graphics-*`** —— 砍了连 logo 都到不了（实测两次）。
4. **不要锁回 bootloader**（`fastboot flashing lock`）。
5. **充电必须用墙充（5V/2A+）** —— 电脑 USB 500 mA 会让设备陷入"充电图标↔尝试启动"死循环。
6. **注意发热** —— 本 port **没有热管理**（无 thermal zone、无 DVFS、`wdt=-1`），CPU 逐帧拷贝
   常驻约 40% 一个核。长时间测试要间歇、且**不要无人值守地挂着**。
7. **长时间 panic/dump 之前先确认通道可用** —— panic 路径会抓 stackshot，历史上会被断言顶掉；
   详见 `HANDOFF-2026-10-27.md` §C / §G。
8. **清理空间时不要删 `UniXNU/out/ipsw`** —— 它是全工作区**唯一需要联网才能恢复**的缓存（IPSW 解包，
   kernelcache 与根镜像都在里面）。要清就清 `fw/` 下被取代的镜像和 `logs/` 里的历史录像。
9. **删任何东西之前先确认它不是唯一副本** —— 已知唯一副本：`fw/userdata-backup/userdata-root.sparse`、
   `fw/vendor_boot_lite.img`、`UniXNU` 仓库里那 193 个未提交改动、全部 `*.md`。

---

## 6. 当前的下一步（按顺序）

1. **提交遗留工作**（已完成 2026-10-27：`f701029` + tag `muyu-desktop-works`）；
2. 列"本地补丁 vs 上游对应物"（如 `braveark-hold-reboot`），标出可直接删除的；
3. **迁格式 2**（`schema_version = 2` + 共用 SoC/OS profile + 规范组件）：分支上做，
   `vendor_boot_lite.img` 当保底，每步验证"还能进桌面"；
4. 之后才做 **USB 调试串口 / 日志闭环 → 热与电源 → GPU（`Adreno740`+`AdrenoMetal`）→ 音频**；
5. Wi-Fi / 蓝牙是硬墙（固件 + iOS 网络栈期望 Apple 自家芯片），**不要优先啃**。
