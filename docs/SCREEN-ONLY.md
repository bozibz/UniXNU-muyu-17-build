# 只读屏（screen-only）：决定、现状、回植方案

**决定（2026-11-08 17:30，维护者）：** 放弃 USB CDC ACM 调试口；放弃 Linux 侧取日志
（`init_boot-reader` v1–v4、pstore/ramoops 读取）。**唯一输出通道 = 面板。**

**更正（2026-11-08 17:50，维护者指出）：** 本文初版把「版本标签、自动翻页、开机
panic 首屏」列为待设计缺口——**这是错的**。这三件（以及更多）早在 iOS 17 线
（21H461）就实现了，27 线不是没实现，是**换到 uke/sheng 基座重建时在链接与配置层
弄丢了**。本文已按回植框架重写（§3–§4）。

USB 与 Linux 两条线的历史结论保留在 `HANDOFF-2026-11-07.md` §47–§60 与
`HANDOFF-2026-11-08.md` §4，作为「已放弃」记录，不再迭代。**2026-11-08 晚已按本文把两条线的
代码、脚本、补丁与启动参数全部拆除**，可继承的测量值与删除清单见 `ABANDONED-CHANNELS.md`。

---

## 1. 为什么这个决定成立

| 通道 | 轮次 | 结果 |
| --- | --- | --- |
| USB CDC ACM | u128–u159，约 29 轮 | 从未枚举成功；最后卡 `DEPCFG` 入队不执行；期间 12 轮测的是被自己复位挂死的核心 |
| Linux 侧（init_boot + pstore） | 3 个镜像（log-reader、v3、v4） | 从未刷过；v1 的头被 ABL 拒绝并回落 recovery |
| **面板** | u143 起 | **每一轮都有结论**；u143 换观测方式后，三轮就走到「只剩 DEPCFG」 |

读屏的命中率是 100%，另两条是 0%。放弃不是妥协，是把资源从 0% 挪到 100%。

---

## 2. 现有资产

读屏引擎不在设备仓里，而在构建树的 `osfmk/console/video_console.c`——**575 行未提交改动**，
今天已抢救进本仓（`drivers/MuyuDebug/` 与 `patches/xnu/wip/muyu-screen-wip-20261108.patch`）。

| 能力 | 符号 | 位置 | 状态 |
| --- | --- | --- | --- |
| 画块（纯色矩形） | `muyu_vc_block` | video_console.c:1314 | ✅ 已用 |
| 画 8×8 字形 | `muyu_vc_glyph_paint` / `muyu_vc_dump_glyph` | :1360 / :1471 | ✅ |
| 大字（panic 用） | `muyu_vc_bigchar` / `muyu_vc_bigrow` | :1652 / :1669 | ✅ |
| 状态行（数字钉住不滚） | `muyu_vc_status_paint` / `muyu_vc_status_repaint` | :1377 / :1448 | ✅ |
| 整屏日志页 | `muyu_vc_logdump` | :1494 | ✅ |
| 任意文本页 | `muyu_vc_textdump` | :1572 | ✅ |
| pram 大字页 | `muyu_vc_pram_bigpage` | :1682 | ✅ |
| 进度条 | `muyu_vc_bar` | :1769 | ✅ |
| 文本绕开保护矩形 | `muyu_vc_char_protected` | :1752 | ✅ |
| 有界锁（防控制台卡死整机） | `muyu_vc_trylock` + `muyu_vc_lock_giveups` | :196 / :190 | ✅ u127 教训 |
| 控制台环形镜像 128 KiB | `muyu_debug_append/copy` | `drivers/MuyuDebug/muyu_debug.c` | ✅ 与 USB 无关，保留 |
| 上次 panic 的 pram 摘要 | `muyu_pram_info[9]` / `muyu_prev_text` | `pexpert/arm/pe_init.c` | ✅ u157 |

---

## 3. 更正：这些 17 线早就实现了（27 线丢了，不是没做过）

维护者指正后的物证核对（2026-11-08 17:35–17:50）。结论：**下表每一项在 17 线都有
成形的、带校验脚本的实现，全部在 `package/`（即 27 线构建树的 `kits/muyu/`，
两处文件逐字节相同）里**；27 线内核只剩残根。

| 能力 | 17 线实现 | 证据 | 27 线内核现状 |
| --- | --- | --- | --- |
| **版本标签单一来源** | `BootProgress.c:194,509`：`PE_parse_boot_argn("sheng-build-tag")` → `build_tag[32]` → 页头 `[tag] title`，一处定义 | 源码在仓；`sheng-build-tag` 机制注释明确「a photograph says which image it is」 | ❌ 无 `_boot_progress_stage`，只剩弱桩 |
| **自动翻页 + 页码标签** | `BootUiTraceProbe.c:110–122`：`page=(seconds/8)%pages`，标签印 `STACK64 %u/%u`；14 行一组轮转，主线程页单独编组 | **17 线实刷镜像（vendor_boot_nofs1/usb9/consoletail1）strings 命中 `STACK64`**；27 线内核 0 命中 | ❌ 无 `boot_ui_trace_*` 符号 |
| **开机首屏回放上次 panic** | `BravearkLastLog.c`（664 行）：pram 低半区 FUNK 记录「the next XNU boot prints」+ 高半区 Linux ramoops 兼容环（`DBGC`），500 ms 周期写 | 源码在仓；`BootProgress.c` 另有 `boot_progress_panic_text[256]` 冻结 panic 文本供后续照片 | ⚠️ 27 线手写的 `muyu_prev_*`/`muyu_pram_*` 只覆盖摘要，没有 FUNK 打印机制 |
| **用户态 crash 上屏** | `MuyuCrashLog.c`（292 行）：从 Data 卷读最新 ReportCrash 报告，经 panic 屏渲染（`muyu-crash-dir/-bytes/-find/-offset` 四个参数） | **17 线实刷镜像 strings 命中 `muyu-crash-dir`**；27 线 0 命中 | ❌ |
| **卡死自恢复（防「死等拍照」）** | `BootResetCore.h` + `BootWatchdogPolicy.c` + `BootLog.c` 的 autoreset：`sheng-autoreset=<s>` 标记 PSCI cell + 武装 hypervisor watchdog，60 s 无 ping 咬死重启进 fastboot | 源码在仓，注释含 int-229 的教训 | ⚠️ 参数在（`sheng-autoreset`），整链路未接 |
| **诊断整页（48 行）** | `BootProgress.c`（990 行）+ `BootProgressDisplay.h` 差分行重绘（只刷变化的字符格）+ `bp_poll` 冻结/连续模式 + `BootProgressTests.c` | 源码在仓；HANDOFF-2026-11-02「诊断页在 CPU 行下多打一行 THERM」 | ❌ |
| **主机侧校验器** | `check-boot-{diag-resume,fault-probe,lifecycle,monitor-isolation,process-probe,progress-code,progress-evidence,progress-host,pulse,ui-layout,ui-trace,ui-trace-reference,watchdog-policy}.py`、`check-xnu-panic-screen.py`、`decode-boot-ui-trace.py`、`debug_screen.py`（主机离线屏模拟） | 两线 tree/scripts 里脚本齐全（27 线还多一个 build-boot-runtime.py），但从未在 27 线跑过 | ❌ 未启用 |

**27 线内核的定量对比**（`nm kernel.macho` + `strings`）：

- 17 线：`_boot_progress_{stage,tag,prepare_scanout,cancel_scanout,pulse_anchor,ext,ext_wide}`
  全在；`STACK64` 在；68 个 `muyu-*/sheng-*` 启动参数（含整套 `muyu-touch-*`、`muyu-crash-*`、`muyu-dump-*`）。
- 27 线：只剩 `_boot_progress_poll`、`_boot_progress_tag`；`STACK64` 无；25 个参数，
  `muyu-*` 仅剩 `muyu-source-storage`。
- 27 线改由 `bsd/dev/arm/muyu_usb_debug.c`（1336 行，未提交）+ `video_console.c`（575 行，
  未提交）**重写**了一小套——即今天抢救的那两个文件。这一步是把 uke 侧的 96 补丁基座
  搬过来时顺手做的，代价是把 17 线成套的东西排除在链接之外。

**一处必须诚实记录的细节**：17 线实刷的构建里，BootProgress 整层也是关着的——
`LegacyDisplayStubs.c` 头注释明写 "when the first-console configuration disables the
BootProgress layer"，所有 `CONFIG_XNU_BOOT_PROGRESS` 在 muyu 各 defconfig 里均
`is not set`；唯一开启它的 `muyu-full-plus_defconfig` 头部自注 "does not build from a
clean tree yet"。所以 17 线面板上实际是 **XNU 控制台 + STACK64 段 + `MUYU ...` 探测行**
（11-03 照片：userspace panic + 寄存器行），不是 48 行整页。**层在、代码在、校验器在，
只差一个能编过的配置**——这正是回植的工作量所在。

---

## 4. 回植方案（替代初版的「新模块设计」）

不做新设计；把 17 线的层接回 27 线的链接。按依赖序：

0. **先接 panic 停留**（2026-11-08 审计新发现）：`braveark-hold-reboot=1` 在 27 线是死参数——
   解析它的两个补丁 `zzzzzzzzz-braveark-hold-{reboot,restart}.patch` 停在 `patches/xnu/backlog/`，
   从未进过 series。后果是 27 线 panic 后自动重启、屏上文本丢失，而读屏唯一怕的就是这个。
   这两个补丁是一对，接的时候一起进 series 并按需验证 apply。审计脚本会因此报 0 fail。

1. **配置**：27 线 defconfig 增加 `CONFIG_XNU_BOOT_PROGRESS=y` +
   `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y`（uku 基座已有这两个开关，17 线的
   `muyu-full-plus_defconfig` 是现成模板；它欠的三样——storage metadata 快照、
   verified OS policy bundle、Adreno 固件值——GPU 两样在 27 线已解决）。
2. **链接**：`build-native-legacy-display.py` / `build-boot-log.py` 照 17 线产物
   （`build/native-legacy-display/libNativeLegacyDisplay{,.late}.a`、
   `build/braveark-last-log/{BootLog,MuyuCrashLog}.o`）把
   BootProgress / BootUiTraceProbe / BravearkLastLog / MuyuCrashLog 接回 fileset。
3. **标签**：vendor_boot cmdline 加 `sheng-build-tag=s<mmddHHMM>`；页头由
   `build_tag[]` 单点生成。**废除在源码 printf 里写 `u1xx` 的做法**（今天已经因此
   错判过一次，见 `HANDOFF-2026-11-08.md` §3.5）。
4. **摘 USB**（✅ 2026-11-08 已完成）：`bsd/conf/files.arm64` 删 `muyu_usb_debug.c` 行；驱动源文件、
   协议头、打包脚本、`zzzzzzzzzzz-muyu-debug.patch`、`-DXNU_MUYU_USB=1` 与三组 `muyu_usb*` 启动
   参数一并删除（清单见 `ABANDONED-CHANNELS.md` §4）。`muyu_debug.c` 环形镜像保留。
   `muyu_prev_*`/pram 摘要保留（它是 BravearkLastLog 的 27 线对应物，等第 2 步接上后由后者接管）。
   **遗留一处待办**：`muyu_vc_status_paint()` 里还有 `muyu_usb_stage_value/last_id/ring_code`
   三个弱引用，驱动删除后 resolve 到 0、该行整条不画；下次真刷面板时把这三格换成内核自己的
   boot stage 数字（改绘制布局 `6*gw + 11*gap` 必须与照片一起验证，不能盲改）。
5. **跑校验器**：第 2 步链接完成后，先在主机跑全套 `check-boot-*.py`
   （17 线的发布门槛），再刷机。

§5 验收清单、§6 代价、§7 与 uke 的对照，维持原文。

---

## 5. 验收清单（全部是照片级，不需要任何工具链）

| # | 场景 | 通过标准 |
| --- | --- | --- |
| 1 | 正常启动 | 开机 20 s 内屏幕出现状态页，第一行含本次 tag |
| 2 | 翻页 | 80 s 内 4 页各出现一次；第 4 页可见进度条 |
| 3 | 上次 panic | 人为触发一次 panic 重启，下次开机的第一屏是 panic 页且含上次文本 |
| 4 | 版本一致 | 同一镜像内所有页面的 tag 相同（grep 源码无硬编码 u1xx） |
| 5 | 控制台不死 | 连续滚动日志时 `giveups` 计数可见且不增长到卡死 |
| 6 | 无 USB 残留 | `strings kernel.macho \| grep 'MUYU USB'` 为空 |

---

## 6. 代价（说清楚，不是没有成本）

- **信息量下降**：一页 ~230×40 字符，四页约 3.6 万字符上限，且必须人拍照。
- **不能脚本化**：没有机器可读的日志，回归比对只能靠看图。
- **时间精度**：心跳 20 s 粒度，事件先后顺序靠页面内容里的 tick 判断。
- **单点**：屏一黑就一无所有——所以 pram panic 页是这条路的**生命线**，不是可选项。

---

## 7. 与 uke 的对照（为什么 uke 不需要这条路）

uke 有 `panic_screen = true`（board.toml:204）+ 预留 pram 回读 + 日志落盘，
它的读屏只做兜底；muyu 现在把读屏当**唯一**通道，所以要比 uke 多做两件事：
自动翻页、开机首屏画 panic。这两件 uke 都不需要——但 muyu 在 17 线已经都有了。
