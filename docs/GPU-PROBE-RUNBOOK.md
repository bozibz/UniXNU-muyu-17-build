# muyu 只读 GPU 探针 · 首次上机观察清单

> 目标：一次上机拿到 `muyu-gpu-power` 的读数，确认 XNU 下能不能安全读到 cliffs 的 GPUCC / SMMU，
> 以及 PLL/RCG 的现场值。**全程只有一次写**（mode 2/3 里的 `gcc_gpu_cfg_ahb` 使能位），
> 不碰电源域、不碰 SMMU 配置、不启动 GMU。

## 0. 前提：这一轮必然要动 userdata

探针挂在显示驱动的 late probe 里（`package/LegacyDisplayProbe.cpp` 的
`native_legacy_display_late_probe()`），也就是**要等 iPadOS 根挂上、用户态开始跑之后**才会执行。
设备现在（2026-10-29 晚）的 `userdata` 是 Android 的，所以：

```
必须先 fastboot flash userdata fw/userdata-backup/userdata-root.sparse   （8.9 GB，~3.5 分钟）
```

**刷完 Android 就没了**（要回来只能重刷原厂 ROM）。这是这一轮的既定代价，不是意外。

## 1. 已有镜像（都在 `fw/`，都已构建完成）

| 文件 | sha256 | boot-arg | 行为 |
| --- | --- | --- | --- |
| `vendor_boot_gpupower1.img` | `f4fb62e745370900bf11baf80718d55669edabd69abfa08592b5db36b809d825` | `muyu-gpu-power=1` | **只读**：GCC → （AHB 已使能才）GPUCC → （CX 上电且投票时钟在跑才）SMMU → （CX+GX 都上电才）GPU |
| `vendor_boot_gpupower3.img` | `5a751e6c9a53382b7b629249cba85f61c90e6ae5dca75e9728c30c0b127c57dc` | `muyu-gpu-power=3` | 先按 mode 2 跑（多一次 AHB 使能写），然后把整份摘要 **panic 到屏上**（这台机器没有持久内核日志）|

> 两个镜像都编自同一个内核（`kernel.macho` = `5dff60e765ee3f93d860d5fc9e6b8f69b457be6df88e3b4ea6cb93ac9c3cc23c`），
> 唯一差别是 boot-arg。
>
> 命令行的坑：内核命令行已经 1008 B，而上限 1000 B（AGENTS §3）。所以 panic 开关没有再单独加参数，
> 而是折进 mode 编号（3 = 2 + panic）。

## 2. 刷之前（3 分钟）

```bash
tools/muyu-flash-mode.sh status        # 设备、槽位、电量、现有镜像
tools/muyu-flash-mode.sh preflight     # 修 slot-unbootable 之类
shasum -a 256 fw/vendor_boot_gpupower1.img fw/vendor_boot_gpupower3.img   # 必须等于 §1 的值
```

- 电量 > 60%，**插墙充**（AGENTS §5 第 5 条）。
- 确认 `fw/userdata-backup/userdata-root.sparse` 的 sha 是 `4c90d7a7…`（唯一副本，别删）。
- 记下当前时间，之后每步都进 `logs/flash-ledger.tsv`（脚本自动写）。

## 3. 刷（两条命令，顺序不要换）

```bash
# ① iPadOS 根（破坏性；约 3.5 分钟）
fastboot flash userdata fw/userdata-backup/userdata-root.sparse

# ② 探针镜像并启动（脚本负责 preflight / 白名单 / set-active / 台账）
tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpupower1.img
```

`xnu` 子命令会：刷 `vendor_boot` → 用 UEFI `boot.img` 启动 → 从 fastboot 出来进 XNU。

## 4. 盯什么：7 行 `MUYU GPU POWER`

探针一共打 7 行（`IOLog`），都是 16 进制、等宽，方便拍照：

```
MUYU GPU POWER m1 mapped gcc=1 gccvote=1 gpucc=1 smmu=1 gpu=1
MUYU GPU POWER gcc ok ahb=XXXXXXXX/XXXXXXXX enabled=0 memnoc=... dvm=... ddrss=... gpll0=...
MUYU GPU POWER cc ok cx=XXXXXXXX gx=XXXXXXXX hwctrl=XXXXXXXX ahb=... crc_ahb=... smmu=... gmu=...
MUYU GPU POWER cc cxo=... hubcx=... accu=... sleep=... memnoc=... cx_ff=... hub_aon=...
MUYU GPU POWER pll1 mode=... opmode=... l=... user=... gmu_rcgr=... gmu_cfg=... hub_rcgr=... hub_cfg=...
MUYU GPU POWER gmu src=3 div=2  hub src=5 div=3  cx_on=Y gx_on=N
MUYU GPU POWER smmu ok scr0=... id0=... id1=... id2=... gfsr=... smr=24 cb=22
MUYU GPU POWER gpu skip rbbm=00000000 ioerr=0
```

### 期望值与偏离解释

| 字段 | 期望 | 说明 / 偏离含义 |
| --- | --- | --- |
| `mapped` | 五个都 1 | 0 表示该窗口映射失败（`nomap`），先查地址/长度 |
| `gcc ahb` | bit0=**1**，bit31 可能=1 | bit0 是使能位；bit31 是硬件门控的 CLK_OFF，**hw-gated 分支空闲时会读 1**，不要当故障 |
| `enabled` | mode 1 → **0**；mode 3 → 1（若 bit0 原本为 0）| mode 1 不写任何东西。**若 ahb bit0=0，则 `cc` 一定是 `skip`，这是正确行为，不是失败** |
| `cc` | `ok` | `skip` = AHB 没使能（见上）；`nomap` = 窗口没映射；`ioerr=1` = 读失败 |
| `cx` | bit31=1（上电）、bit0 可能=1 | `cx_on=Y` 需要 bit31；bit0（SW_COLLAPSE/投票位）在 Android 空闲时也是 1，别被它误导 |
| `gx` | 空闲时 bit31 可能=0 | GPU 空闲时 GX 关着是正常的（Android 实测 255 MHz 空闲态） |
| `hwctrl` | 非零 | `gds_hw_ctrl`（0x90a0），`gdsc.c` 用它轮询 PWR_ON |
| `pll1` | `mode` bit31=1（锁定）、`opmode` bit0=1、`l=0x16`、`user=5` | 440 MHz 配置；`l` 不是 0x16 说明 PLL1 没按 cliffs 的值配 |
| `gmu src/div` | `src=3 div=2` | 220 MHz = PLL1 440 ÷ 2；`div` 已是「域值+1」 |
| `hub src/div` | `src=5 div=3` | 200 MHz = GPLL0 ÷ 3 |
| `smmu id0/id1` | 低 8 位 **0x18 / 0x16**（=24 / 22）| 与 Android dmesg 的 `24 register groups / 22 context banks` 对齐 |
| `smmu id2` | 记录即可 | 页大小/VA 宽度编码，对照 dmesg `page sizes 0x61311000` |
| `gpu` | `skip`（GX 关）或 `ok` | `ok` 时记录 `rbbm`；`skip` 不代表失败 |
| `ioerr` | **0** | 1 = 有读操作失败，按行定位 |

## 5. 判定树

1. **没有任何 `MUYU GPU POWER` 行** → 探针没跑到：先确认到底有没有走到用户态（屏幕上是否有
   `Failed to mount root device` 之类的更早 panic）。这是 boot 问题，不是探针问题。
2. **`cc skip` + mode 1** → 正常（XNU 里没人开 AHB）。改用 `vendor_boot_gpupower3.img`（mode 3）。
3. **mode 3 下 `cc ok`** → 拿到 GPUCC 现场值，把 7 行拍照，进入 §6 的对照。
4. **mode 3 下 `cc nomap` / `ioerr=1` / 挂板复位** → 停止实验，记录现象（时间、最后一行、屏幕内容），
   回滚到 `fw/vendor_boot_btn2.img` 再来。
5. **`smmu ok` 且 id0/id1 与 Android 一致** → 只读通道完全打通，可以开始计划 CX 上电序列（下一阶段）。

## 6. 记录规范

- 每行都拍照（照片放 `logs/2026-10-29-gpu/` 或当天目录），**先 `transpose` 回正再读**（相册照片常常旋转 90°）。
- 把 7 行原文抄进当天 `HANDOFF-2026-10-3x.md`，连同：镜像 sha、boot-arg、设备启动到出行的耗时、
  是否复位过。
- `tools/muyu-flash-mode.sh ledger` 会自动记刷写；**不要手敲 fastboot**（AGENTS §5 第 9 条）。

## 7. 构建记录（可复现）

```bash
# 内核（含探针；改了 package/MuyuGpuPower* 就要重来，约 5–12 分钟）
cd UniXNU && python3.12 unixnu build kernel -D ../UniXNU-muyu \
   --kernelcache out/ipsw/21H461/kernelcache.decompressed

# payload（mode 1）
python3.12 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
   --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
   --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr --kernel-arg=muyu-gpu-power=1
python3.12 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
   --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_gpupower1.img

# payload（mode 3 = mode 2 + panic 摘要）：同上，把 --kernel-arg 换成 muyu-gpu-power=3，
# 输出换成 fw/vendor_boot_gpupower3.img
```

> `--data-ramdisk` 必须是 `fw/data-seed/muyu-data-200g.dbr`：这是从能进系统的
> `fw/vendor_boot_btn2.img` 里按 `DBRDISK1` 魔数逐字节比对出来的（HANDOFF-2026-10-29 §10.3），
> 不是猜的。漏掉它 → `offline scratch Data mount failed`，永远到不了系统。
