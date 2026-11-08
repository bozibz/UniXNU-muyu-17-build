# uke 完全体：从零到桌面的构建与启动链路

对 `UniXNU-uke` 全仓走查的结果。回答一个问题：**同一套硬件、同一套框架，uke 到底是怎么"造出一个完整系统"的**。

核心结论只有两句：

1. 官方提供了统一驱动 `unixnu`，`./unixnu release` 一条命令跑完六步（deps → preflight → patches → build → pack → verify），加上可选的 `./unixnu build userdata`，就是一个完全体。
2. 我们（muyu）从头到尾只跑了这六步里的**两步半**：`build kernel`、`build payload`，`pack` 用手写脚本代替。`userdata` 一次都没跑过。

---

## 1. 一键脚本在哪里

| 层 | 文件 | 作用 |
| --- | --- | --- |
| 最外层 | `UniXNU-27/scripts/package.sh` | 文档所述入口（`docs/porting/packaging.md:39`），`--os` / `--no-deps` / `--check` |
| 实际实现 | `UniXNU-27/scripts/unixnu/cli.py:273` `cmd_release()` | 六步固定顺序 |
| 设备封装 | `UniXNU-uke/scripts/build-uke-macos.sh`（91 行） | 检查工具链、填环境变量、串起 `unixnu` 与 UEFI 构建 |

六步的定义在 `docs/porting/packaging.md:28-36`，代码里对应：

```python
STEPS = ('kernel', 'package', 'firmware', 'payload')      # cli.py:12
OPTIONAL_STEPS = ('userdata', 'runtime')                  # cli.py:13
```

`cmd_release()` 的实际流程（`cli.py:273-312`）：

```
step deps       → fetch 模块 + ipsw 解包
step preflight  → defconfig + feeds.resolve
step patches    → 按依赖排序列出 patch series
step build      → kernel → package → firmware → payload
step pack       → bootimg（firmware volume + dtb + payload）
step verify     → 写 manifest 并逐项校验
```

**注意 `verify` 这一步。** `pipeline.write_manifest()` 会产出一份 `package.manifest.json`，记录每个输入的 hash。uke 的"完全体"不是一个镜像，是**镜像加可复现清单**。

---

## 2. 逐步展开

### 2.1 deps

读 `feeds.toml`、`.gitmodules`、`[ipsw]`。做两件事：

- 拉模块子模块与 git 源，写 `unixnu.lock` 锁定版本。
- 从 IPSW 里只取需要的成员。uke 的 `[ipsw] fetch` 只要四个：`KernelCache`、`DeviceTree`、`StaticTrustCache`、`Cryptex1,SystemTrustCache`。

uke 的 `feeds.toml:20-54` 把组件分成两类：

```toml
# 本仓库有源码
local = "drivers/NativeNovatekTouch"     # feeds.toml:56-59
# feed 外部拉取，本仓库只有 pin
Adreno740 / GopFramebuffer / ShengUfs / ShengHidInput
```

**这一点很关键**：uke 的仓库不是"驱动全在里面"，而是"驱动在 feed 里，只有 pin 和板级适配在里面"。所以它的 `board.toml` 能做得很薄。

### 2.2 preflight

选 defconfig 写配置。uke 用 `uke_ipados27_defconfig`，并且靠 `config_fragments` 叠两层：

```toml
config_fragments = ["arch:configs/common.config", "os:configs/ipados.config"]   # board.toml:47
```

### 2.3 patches

四层 series，顺序固定（`board.toml:48`）：

```toml
patch_series = [
  "arch:patches/xnu/series",        # 共享架构
  "soc:patches/xnu-12377/series",   # SoC 平台
  "os:patches/xnu/series",          # OS 层
  "device:patches/xnu-12377/series",# 板级
]
```

`patches.plan()` 会按依赖解析出**实际应用顺序**并逐条打印（`cli.py:293-295`：`patch {number:02} {path}`）。这是"一补丁一假设"的落点——每个补丁都能单独回滚。

### 2.4 build: kernel

这是 uke 最厚的一步。`[kernel] archives`（`board.toml:54-77`）定义了构建顺序，而这个顺序**就是依赖顺序**：

```
corecrypto ×2 → ArmDTPlatform → ArmCpu → libpthread
→ AppleKeyStoreState → SoftwareAES(+27)
→ VirtioMmioBlock → ShengUfs → RamBlock → ShengHidInput
→ prepare-uke-gmu → Adreno740 → adreno740-kits → adreno740-accelerator
→ build-native-legacy-display-27      ← 最后，把所有探针 .a 成 libNativeLegacyDisplay.late.a
```

最后那个 `build-native-legacy-display-27.py:543-549` 把 20 多个探针对象链成一个 late archive。这解释了为什么 uke 的探针"改参数就能换行为"——它们全在同一个链接单元里，靠 boot-arg 分支。

prelink 用 iPadOS 24A446 的原厂 kernelcache 做参照，`[kernel.prelink] roots` 列了 31 个 kext（`board.toml:151`），`--allow-missing` 容忍缺失。

### 2.5 build: package

产 `package.img`，即内核之外的用户态组件包。

### 2.6 build: firmware

UEFI 固件。uke 用 Mu-Silicium：`dsc = "Platforms/Xiaomi/ukePkg/uke.dsc"`，产物 `Build/ukePkg/{target}_CLANGPDB/FV/SILICIUM_UEFI.fd`（`board.toml:159-161`）。

`preflight = ["scripts/check-uke.py"]` 会在构建前跑设备自检。**我们这一步是空的**（`preflight = []`）。

### 2.7 build: payload

把内核 + device tree + trust cache + 可选 Data seed 打成 FAT 卷，卷名 `UKEESP`，`size_mib = 160`，`loader = "firmware"`（DarwinBoot 在固件卷里）。

uke 在这里打开 `panic_screen = true`（`board.toml:201`），注释写得很实在：

> the panic screen draws a panic that happens before the video console exists. Every panic before this was invisible on uke (no UART)

### 2.8 pack

`bootimg`：firmware volume + device tree + payload → `boot.img`。header v4，`pagesize = 4096`。

uke 这里有个**必须加的节点**（`board.toml:302-310`）：

```toml
[[bootimg.dtb_node]]
path = "/reserved-memory/splash_region"
# The stock ABL powers the panel off when the device tree has no splash region, so add it.
```

没有 splash region，ABL 直接关屏。这是硬件行为，不是配置洁癖。

### 2.9 verify

写 `package.manifest.json`，逐项比对输出与输入 hash。

### 2.10 userdata（可选步，但必需）

`./unixnu build userdata`，实现在 `UniXNU-27/scripts/unixnu/userdata.py`。

uke 也有一个早期独立版本 `scripts/build-userdata-root-image.py`（198 行），逻辑相同，可以对照读，因为它把注释写得更清楚。

---

## 3. userdata 的真相：不是 f2fs，是 SHNGROOT

这是整个体系里最反直觉的一环，也是我们理解错最多的一环。

**userdata 分区里装的不是 Android 的文件系统，而是一个自定义容器，里面是 IPSW 的 OS/SystemOS 磁盘镜像原封不动。**

布局（`userdata.py:1-18`）：

```
0x000000  manifest block（4096 字节），零填充到 1 MiB
0x100000  window "OS"        完整的 OS 磁盘镜像（GPT + APFS），1 MiB 对齐
....      window "SystemOS"  完整的 SystemOS 磁盘镜像，1 MiB 对齐
....      data objects       --object NAME=FILE[:OFFSET]，各自 1 MiB 对齐
```

manifest 格式（小端）：

```
"SHNGROOT" | u32 version | u32 count | u64 image_bytes
then count × 128 字节条目：
  { char name[32], u64 offset, u64 size, u32 block_size, u32 flags, u8 sha256[32], 40 reserved }
最后 4064..4096：前 4064 字节的 SHA-256
```

两个版本：

| version | block_size | flags | 用途 |
| --- | --- | --- | --- |
| 1 | 512 | 1 | 窗口，发布成只读块设备 |
| 2 | 4096 | 3 | data object，内核自己读，不发布成块设备（策略哈希表、固件） |

### 内核侧怎么读

`drivers/NativeUfs/RootLayout.h`，219 行，全部是校验：

```
1. 读 GPT，找 userdata 分区（entry[56+2c] 匹配 "userdata" UTF-16）
2. 读分区第一块，判 magic0==0x474e4853 && magic1==0x544f4f52   // "SHNGROOT"
3. version==1 && 1<=count<=4
4. SHA256(bytes 0..4063) == bytes[4064..4095]
5. 每个窗口：offset 4KiB 对齐、size 512 对齐、不越界、block_size==512、flags==1
6. 名字可打印、唯一、窗口不重叠
```

`manifestStatus` 是诊断码（`RootLayout.h:150-209`）：

| 值 | 含义 |
| --- | --- |
| 0 | 合法 |
| 1 | 没有 SHNGROOT magic |
| 2 | version/count 不合法 |
| 3 | manifest 自校验失败 |
| 4 | 越界 |
| 5 | 名字非法 |
| 6 | 窗口重叠 |
| 7 | 属性非法 |

`NativeUfs.cpp:2176-2200` 的 `startRoot()` 三步：host bring-up → `findRootLayout()` → `manifestStatus != 0` 直接拒。

然后 `prepare()` 给每个窗口造一个 `NativeUfsBlock`，`configure(name, userdataFirst*4096 + window.offset, size)`，再交给 VerifiedRoot 认证。

### 为什么值得这样做

1. **不用造 root image。** OS 卷就是 IPSW 里的那份，字节不变，签名自然有效。
2. **可复现。** manifest 里的 sha256 就是 IPSW 成员的 sha256，改不了。
3. **可诊断。** 7 个拒绝码直接把"哪一步不对"变成数字，不用猜。

代价是：**这个分区一旦写入，Android 就再也读不了它**。这就是为什么我们的设备在刷过 userdata 之后，Android 起不来，必须进 recovery 重建 f2fs。

---

## 4. 内核内部：从入口到桌面

### 4.1 总装顺序写死在一个函数里

`package/LegacyDisplayProbe.cpp:754-765`：

```c
native_keystore_probe();          // AppleKeyStoreState：回答锁状态查询
sheng_cpufreq_probe();
sheng_gpu_power_probe();          // Adreno740 KMD：GMU 冷启动
native_dpu_probe();               // 显示
native_touch_probe();             // Novatek 触摸
native_hid_input_probe();         // 进 IOHIDFamily
native_virtio_input_probe();
native_shared_window_probe();
native_qemu_metal_publish();      // AdrenoMetal：发布 Metal 插件
```

这个顺序就是依赖顺序，不能换。

### 4.2 存储链

`package/SourceStorage.cpp:134-179`，只在 `native-source-storage=1` 时执行：

```
load IOStorageFamily → 校验 IOBlockStorageDevice getClassSize()
→ native_source_root_policies_load()   // 策略包，失败即 panic
→ load apfs
→ preModLoad → ufs_init / ram_init → postModLoad
→ start_image4_dependencies()
→ native_ufs_source_create() + init/attach/start
→ start_data_disk()
```

`SoftwareAES` 在 APFS 之前必须过双 KAT（`SourceStorage.cpp:52-71`：algorithm + interface，任一失败 panic 并打印 `kat/interface/registered`）。

### 4.3 到桌面必须的五个前提

| 前提 | 组件 | 缺了会怎样 |
| --- | --- | --- |
| 锁状态有应答 | AppleKeyStoreState（`sheng-aks=1`） | keybagd 卡住，用户态停摆 |
| Image4 依赖可用 | SoftwareAES + AppleImage4 | APFS 挂不上 |
| 内核 crypto | corecrypto（prelink root） | AMFI/沙箱失败 |
| IOSurface 就绪 | load IOSurface，等 `IOCoreSurfaceRoot` 5 s | `layout_buffers` 在 `0xfffffff00578d110` 崩 |
| HID 可被 backboardd 接受 | PrimaryUsagePage=0x0d, PrimaryUsage=4 | digitizer 事件被静默丢弃 |

第五条有专门的坑记录（`HidInputProbe.cpp:121-126`）：组合设备的键盘 collection 在前，服务会发布成 (1,6)，backboardd 当成键盘，触摸全丢。所以 `sheng-touch-split=1` 要单独发一个 Digitizer 设备。

### 4.4 阶段编号：没有全局 enum

想找 "stage 0..N" 会失望。实际是：

- **全局**：`docs/PROGRESS.md:44-50` 的 1..7 表（UEFI / UniXNU / 负载 / DarwinBoot / 内核入口 / 存储与根卷 / 用户态），只以配置文件名形式进代码。
- **运行时**：`UkeBootStages.cpp:58-79` 的 6 位序号，只是"第 n 条日志"。
- **子系统各自编号**：`sheng-touch=1..4`、`sheng-gpu-power=1..9`、`offline-legacy-phase=1..4`、UFS root 的 `native_ufs_root_step=1..3`。

---

## 5. muyu 断在哪

把两条流水线并排放：

| 步骤 | uke（完全体） | muyu（实际跑的） |
| --- | --- | --- |
| deps | `feeds.toml` + `[ipsw]` | 手工准备，`fw/trust-24A446/` 等目录 |
| preflight | `uke_ipados27_defconfig` + 两层 fragment | 同名机制，但配置在 `/tmp/spike27/board.toml`（**不在仓库**） |
| patches | 四层 series，`patches.plan()` 排序 | 同样四层（arch/soc/os/device），设备层 `patches/xnu-27/series`；2026-11-08 之前只有手写的一层（`series-spike27`） |
| build kernel | ✅ | ✅ |
| build package | ✅ | ⚠️ EDK2/UEFI 产物是 2026-11-05 编好后一直复用的（`UniXNU/out/muyu/boot/boot.img`，2.1 MB），此后每轮没重编 |
| build firmware | Mu-Silicium + `check-uke.py` 预检 | `preflight = []`，同上，直接吃那个预编译 `boot.img` |
| build payload | ✅ `size_mib=160` | ✅（`--data-ramdisk` 只是首启种子，见下） |
| **pack** | `bootimg` 从 firmware + dtb + payload | **手写 `patch-vendor-boot.py` 替换 ramdisk** |
| verify | 写 manifest 逐项校验 | ❌ 无 |
| **userdata** | `unixnu build userdata` | ✅ 做了，但用的是自写脚本（烧过 6 次） |

> **更正（2026-11-08 16:40）**：本节初稿写"从来没烧过 userdata"，是错的。我只看命令行上的
> `--data-ramdisk` 就下了结论，没查 `logs/flash-ledger.tsv`。实际记录：

| 时间 | 镜像 | 说明 |
|---|---|---|
| 2026-11-03 11:48 / 12:09 | `fw/userdata-root-preboot.sparse` | 首次，9.38 GB sparse |
| 2026-11-03 12:18 | `fw/userdata-root-preboot2.sparse` | 重建后重烧（sha 变了） |
| 2026-11-03 13:49 | `fw/userdata-backup/userdata-root.sparse` | 换回备份版 |
| 2026-11-05 12:43 | `/tmp/userdata-root-27.sparse` | **27 线**，15.2 GB |
| 2026-11-06 15:38 | `/tmp/userdata-preboot.sparse` | 14.7 GB |

解开 `fw/userdata-root-preboot.sparse` 的 sparse 头，逻辑容量 8.74 GiB，裸数据前 8 字节是
`53 48 4e 47 52 4f 4f 54` = `SHNGROOT`，版本 1，窗口数 2 —— 正是
`drivers/ShengUfs/RootLayout.h` 要解析的那个容器，OS 与 SystemOS 两个窗口都在。

**Data 卷也不是临时的。** `packed_data_ramdisk = true` + `persistent_data = true` +
`sheng-data-persist=2`，`board.toml:664` 自己写着："With sheng-data-persist=2 the Data disk keeps
its writes in a window at the end of userdata, so it is not ephemeral storage"。`data-seed.img`
（1.04 MiB，`DBRDISK1`）只是首次开机的种子，不是 Data 卷本身。

所以真正剩下的差异只有两条，都比原来判断的轻：

**（1）`[bootimg.image]` 段缺失，但功能没缺。** muyu 的 `board.toml` 确实没有 `[bootimg]`（`grep`
命中 0），但同一份配置的注释写明"Image parameters come from the Mu-Silicium device config"。
实际路径是：`boot` 分区刷 Mu-Silicium 编出的 UEFI fd（2.1 MB），`vendor_boot` 用原厂模板
加 `patch-vendor-boot.py` 替换 ramdisk。**是另一套管线，不是少了能力**——`vendor_boot` 已经
成功刷了 316 次，链路是通的。初稿说"pack 跑不了所以致命"，夸大了。

**（2）配置不在仓库里。** 实际生效的是 `/tmp/spike27/board.toml`，仓库里那份
`UniXNU-muyu/board.toml` 还是 iPad7,1 / j120ap 模板，差 183 行。`/tmp` 一清，行为就变。这条仍然成立。

---

## 6. 如果要重来一遍

顺序应该是这样，一步都不跳：

```sh
# 1. 把配置落进仓库（现在是 /tmp，随时会没）
cp /tmp/spike27/board.toml <device-repo>/board.toml

# 2. 一条命令跑完 kernel → package → firmware → payload
./unixnu release

# 3. 造 userdata（自写脚本和官方脚本产物同构，都产出 SHNGROOT 容器）
./unixnu build userdata        # 或 scripts/build-userdata-root-image.py
fastboot flash userdata <work>/userdata/userdata-root.img

# 4. 按 verify 的 manifest 核对，再上机
```

`[bootimg]` **不是必须补的**：muyu 的 `boot` 分区只放 Mu-Silicium 编出的 UEFI fd，用
`fastboot boot` 从 RAM 起；内核 payload 走 `vendor_boot`，由 `patch-vendor-boot.py` 套原厂模板。
这是另一套（已验证 316 次的）管线，不是缺能力。真正该补的是**把 UEFI 的 EDK2 构建纳回
`release` 流程**——现在那个 2.1 MB 的 `boot.img` 是 11-05 的旧产物，`UniXNU/out` 一删就没了。

关键判断：**在第 3 步之前不要碰硬件。** uke 能在第 4 天出画面，是因为前 3 天全在 Mac 上把这几步跑通、把 18 个 `Offline*.mm` 在主机上验完。上机只验"寄存器通不通"，不验"逻辑对不对"。

---

## 附：一页速查

| 想知道 | 看哪里 |
| --- | --- |
| 一键脚本做了什么 | `UniXNU-27/scripts/unixnu/cli.py:273` |
| 六步的定义 | `UniXNU-27/docs/porting/packaging.md:28-36` |
| userdata 格式 | `UniXNU-27/scripts/unixnu/userdata.py:1-18` |
| userdata 内核侧解析 | `UniXNU-uke/drivers/NativeUfs/RootLayout.h:141-209` |
| 组件构建顺序 | `UniXNU-uke/board.toml:54-77` |
| 内核内总装 | `UniXNU-uke/package/LegacyDisplayProbe.cpp:754-765` |
| 存储链 | `UniXNU-uke/package/SourceStorage.cpp:134-179` |
| 阶段表 | `UniXNU-uke/docs/PROGRESS.md:44-50` |
| 移植领域状态 | `UniXNU-uke/porting.toml`（12 领域，9 device-verified） |
