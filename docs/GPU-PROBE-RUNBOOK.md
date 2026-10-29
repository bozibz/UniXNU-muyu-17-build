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

## 8. 首次实跑结果（2026-10-29 20:30，已完成）

`muyu-gpu-power=3` 的 panic 屏（照片 `logs/2026-10-29-20-30-54-132.jpg`）读数，与清单预期对照：

| 字段 | 实测 | 与预期 |
| --- | --- | --- |
| `gcc` | `ok`，`ahb=80000001/80000001 en=0` | ✅ 固件已使能 AHB，mode 3 不需要写 |
| `cc` | **`ok`**，`ioerr=0` | ✅ 22 个 GPUCC 寄存器全部读到，板子没挂 |
| `cx` / `gx` | `0022f801` / `0022f801`（bit31=0） | 两个域都关着（预期）|
| `hwctrl` | `2800010a` | ✅ 与 Android 逐位一致 |
| `ccahb` | `00000000`（CLK_OFF=0） | GPUCC 的 AHB 没被门控 —— 这就是读得通的原因 |
| `pll1` | `00000000/00000000 l=00000000` | ⚠ **PLL1 完全没配**（Android 是 440 MHz）|
| `gmucfg` / `hubcfg` | `00000000` / `00000000` | ⚠ RCG 没配（Android 是 `src=3 div=2` / `src=5 div=3`）|
| `gpll0` | `0065257f`（bit15/16=0） | ⚠ GPLL0 的 GPU 投票没投 |
| `smmu` / `gpu` | `skip` | ✅ 正确门控（CX 没开，不去读）|

**结论**：只读通道打通；cliffs 的 GPUCC 在 XNU 下"可读但未配置"。
下一步是**写**：PLL0/PLL1 → GPLL0 投票 → hub/gmu RCG → 分支 → CX GDSC（轮询 0x90a0 的 PWR_ON），
每步一次 panic 摘要。**注意：`muyu_pstore=1` 会让启动停在苹果 logo，已从 board.toml 注释掉**（§11.4）。

## 9. mode 4：只配时钟、不碰电源域（下一枪）

`muyu-gpu-power=4` = mode 1 的只读快照 **＋** 时钟 bring-up（照 Linux 的顺序）**＋** panic 摘要：

1. **GPLL0 的 GPU 投票**（GCC `0x52000` bit15/16，Linux 投完等 10 µs）；
2. **PLL0 配置**（`l=0x1a, α=0x9000`，510 MHz；已经在跑就不重配）；
3. **PLL1 配置 + 使能**（`l=0x16, α=0xeaaa`，440 MHz）：写配置 → 清 OUTCTRL → OPMODE=0 →
   MODE|=RESET_N → 等 `LOCK_DET`（最多 1500 次）→ USER_CTL|=0x7 → MODE|=OUTCTRL；
4. **hub 时钟源**：`0x919c`，`src=5 (GPLL0)`、`div=3`（200 MHz）；
5. **gmu 时钟源**：`0x9168`，`src=3 (PLL1)`、`div=2`（220 MHz）；
6. **分支**：`gpu_cc_ahb`、`gpu_cc_cxo`、`gpu_cc_hub_cx_int`、`gpu_cc_cx_gmu`、`gpu_cc_memnoc_gfx`、
   GCC `gcc_gpu_memnoc_gfx`、`gcc_ddrss_gpu_axi`，最后轮询 `cx_gmu` 的 `CLK_OFF` 清掉。

**不写 GDSC**（CX/GX 仍是关的）。新增两行输出：

```
MUYU GPU POWER clk step=… failed=… lock_polls=… hub_polls=… gmu_polls=… branch_polls=…
MUYU GPU POWER clk pll1=…/… gpll0=… gmucfg=… hubcfg=… cx_gmu=… cx=…
```

判定：

| 现象 | 含义 / 下一步 |
| --- | --- |
| `step=7 failed=0`，`gmu src=3 div=2`、`hub src=5 div=3`、`gmu_running=Y` | 时钟树完全起来，可以进下一步（CX GDSC） |
| `step=3 failed=1`，`lock_polls=1500` | PLL1 不锁定：查 PLL0/PLL1 的配置值或父时钟（CXO）是否在跑 |
| `step=4/5 failed=1` | RCG 的 `ROOT_OFF` 不清：`src` 选到了没跑的父时钟 |
| `step=6 failed=1` | 分支使能后 `cx_gmu` 的 `CLK_OFF` 不清：父时钟/门控问题 |
| `failed=2` | I/O 失败（`ioerr`），先看是哪一行 |

**风险**：这是第一次真的"写" GPUCC 的时钟。若挂板，回滚 `fw/vendor_boot_btn2.img`，
并把 `step` 号照片留下（panic 摘要里有）。

## 10. mode 9：第一次**写** GPU SMMU（GMU 的 stage-1）

`muyu-gpu-power=9` = mode 8（CX 上电 + 时钟树 + SMMU 可读 + 五块 GMU 内存 + 固件 staging）
**＋ GMU 的 stage-1 SMMU 建起来**。GMU 本身仍然不启动。

代码：`package/MuyuGmuSmmu.c`（板级计划）+ `package/ShengGpuSmmu.c`（bank/stream 生命周期）
+ `kits/ArmSmmu4K`（native4K 表/上下文/流）。主机校验：
`python3 UniXNU-muyu/scripts/check-muyu-gmu-smmu.py`（常量对表 + 库生命周期 + 本板计划 + freestanding aarch64）。

### 10.1 它到底写什么

| 步骤 | 写/读 | 说明 |
| --- | --- | --- |
| 1 | 读 | IDR0/IDR1/IDR2/IDR7、sCR0、GFSR、SMR0/S2CR0、`0x22000`（CB2 在 0x20000+bank*0x1000）与 `0x4000`（若 GR0 只有 2 页）两个候选 CB 基址 |
| 2 | 写 S2CR[slot] | 为 SID 5 租一个**无效 SMR**（跳过所有 valid 的槽位），把它的 S2CR 置成 FAULT（`0x20000`）。库要求"目标流已经是 FAULT" |
| 3 | 写 sCR0 **4 位** | `(scr0 & ~0x407) | 0x406`：CLIENTPD=0、GFRE=1、GFIE=1、保留 USFCFG 与其余所有位。库的全局前置要求 `(scr0&0x407)==0x406` |
| 4 | 分配 4 MiB | 表池：`[0x4000, 0x60407000)` 这段口径需要 **774 页**（1 根 + 2 个 level-2 + 771 个 level-3），叶子描述符放在这些表里 |
| 5 | 写 CB2 | `ShengGpuSmmuStartLive(GMU, bank=2, slot, TTBR0=arena)`：先擦 image、写 TCR2/TCR/TTBR0/MAIR、ASID 同步、回读校验、最后置 SCTLR |
| 6 | 写 CB2 | 五块按 `ArmSmmu4KLiveUpdate` 逐个映射（`[icache]→[dummy]→[log]→[hfi]→[debug]`，每次把已映射集合作为 `before` 重新证明）|
| 7 | 读 | `ShengGpuSmmuObserveLive`：context+stream+五个 extent 再验一遍 |

缓存：表池与五块内存在 walk 之前按**物理地址** `flush_dcache64(..., TRUE)` + `dsb sy`
（`smmu_*` 的 barrier 不是 cache clean）。上下文 `coherent_walk=1`、`continue_after_fault=1`、
icache 段带 X 权限，其余 RW+privileged，`writeback=0`、`shareability=2`（与 a6xx_gmu.c 的 GMU 映射一致）。

### 10.2 期望值

```
gmu smmu done ph=5 prep=0 start=0 map=0 obs=0 tbl=774 leaf=4111 cap=1024 ctx=00022000
gmu io id0=00000018 id1=... id2=... scr0=00000406>00000406 ...
```

| 字段 | 期望 | 偏离含义 |
| --- | --- | --- |
| `prep` | 0 | 1=计划被拒（对齐/尺寸/重叠/池太小），2..5 = `ArmSmmu4K` 的 BadRange..Busy |
| `start` | 0 | **6 = Unsupported**：`ArmSmmu4KEncodeContext` 的 ID 门（IDR0 bit30、IDR2 bit12=4K、IDR2[11:8]/[7:4] 的 VA/OAS）或 `ArmSmmu4KProgram` 的 `IDR1[30:28]` 页数门（见 §10.3）|
| `map` | 0 | 1=Argument、3=Overlap、7=StateMismatch：先看 `mapped`（已装上的数量）|
| `obs` | 0 | 7 = 观察到的 context/流/映射与写入不一致 |
| `tbl` | 774 | 不是 774 说明口径或 pool 变了，先改 `MuyuGmuSmmuTablePages` |
| `leaf` | 4111 | 4092(icache)+4+7+4+4 个 4K 叶子 |
| `ctx` | `00022000` | `(32+bank)*0x1000`（IDR1[30:28]=4 时）|

### 10.3 ★ 第一次上机的实测结果（2026-10-29 21:17）：0x4000 **不是** context bank

`fw/vendor_boot_gpupower9.img` 刷入后 **panic 在 data abort**（照片
`logs/2026-10-29-21-17-49-960.jpg`）：

```
Kernel data abort, at pc 0xfffffff0079f3094, lr 0xfffffff0079f2844
x18: 0x00000000c435c7bac        ← mode 8 的固件 sha 摘要 ⇒ 模式 8 已经跑完，挂在 mode 9
esr: 0x95000010                 ← EC=0x25 同 EL data abort，DFSC=0x10 同步外部中止
```

反汇编（`llvm-objdump` 同一份 `kernel.macho`）把 PC 精确对到两条相邻的读：

```
79f307c: add x11, x8, #0x22, lsl #12   ; SMMU 窗口 + 0x22000
79f3080: ldr w11, [x11]                ; ✅ 读通
79f3090: add x11, x8, #0x4, lsl #12    ; SMMU 窗口 + 0x4000
79f3094: ldr w11, [x11]                ; ❌ 外部中止（就是这里）
```

判读：**+0x4000 不是 context bank**。读通的 `+0x22000` 才是 —— 它正是
`0x20000 + bank*0x1000`（bank 2）。DT 的节点是 `qcom,qsmmu-v500`，`reg = <0x3da0000 0x40000>`，
22 个 bank 正好占 `0x20000..0x36000`；而厂商 `arm-smmu-qcom.c` 里的 `ARM_SMMU_IMPL_DEF4` 页
（micro-MMU halt / `CLK_ON_STATUS` 那套 QSMMU 扩展）就落在 `0x2000..0x6fff` 这一段，属于
**不该按 bank 去碰**的区域。

按 `ArmSmmu4K` 的模型，`IDR1[30:28]=0 ⇒ pages=2 ⇒ bank2 = (2+2)*0x1000 = 0x4000`，
**正好撞在 IMPL_DEF 页上**。所以这不是"我们猜错了 CB 基址"，而是 **这块 QSMMU 的 IDR1 高位读出来是 0**。

### 10.3.1 这台 QSMMU 的 ID 寄存器只报低字节

mode 6/9 的现场值：`id0=0x18`、`id1=0x16`、`id2=0x02`。低字节与 Android dmesg 的
"24 register groups / 22 context banks" 完全对得上，但**能力位全为 0**：
IDR0 没有 stage-1/AArch64 位、IDR0 的 SID 宽度字段为 0、IDR1 的 bank 页数字段为 0、
IDR2 没有 4 KiB 页表位也没有 VA/OAS 宽度。`ArmSmmu4K` 会因此拒绝（Unsupported），
而它的 2 页推导又会去碰 0x4000。

**处理**（`package/MuyuGmuSmmu.c` 的 `MuyuGmuSmmuIdView`）：只在这几个 ID 寄存器上，
给库一个**能力视图**——`IDR0 |= stage-1|AArch64|10-bit SID`（DT 的 `qcom,stream-id-range` 是
`0x00..0x400`）、`IDR1[30:28]=4`（32 个 bank 页 ⇒ CB0=0x20000）、`IDR2 |= 4 KiB|VA40|PA40`。
其余位一个不改；IDR 是只读寄存器，这个视图不会落到任何寄存器上，而且读值是确定性的，
库自己的"前后重读一致"检查仍然成立。**原始的 `id0/id1/id2` 由探针直接读、原样打印**，
所以记录里两种值都在。主机测试（`check-muyu-gmu-smmu.py`）现在就用
`0x18/0x16/0x02` 跑，并断言 context 落在 `0x22000`、且**任何一次访问都不许碰 0x4000**。

### 10.3.2 旧的预判（保留作对照）

mode 6 读到的现场值是 `id0=00000018 id1=00000016 id2=00000002 scr0=00220465`。按 `ArmSmmu4K` 的模型：

- `scr0=0x00220465`：CLIENTPD(bit0)=1、GFRE(bit1)=0 ⇒ `ArmSmmu4KStreams` 的全局预检**必然拒绝**
  （它要 `0x406`）。这就是 mode 9 必须先写那 4 位的原因。
- `id1=0x00000016`：库把 `IDR1[30:28]` 当"上下文页数指数"，`pages = 1<<(((id1>>28)&7)+1)` = **2**，
  而 `banks = id1&0xff` = 22 ⇒ `banks > pages` ⇒ **Unsupported**。
- `id2=0x00000002`：`bit12`（4 KiB 页表支持）为 0，`[11:8]`/`[7:4]` 的 VA/OAS 编码为 0 ⇒
  `ArmSmmu4KEncodeContext` 也会 **Unsupported**。（实测见 §10.3：真实的根因是"只报低字节"，
  而且 0x4000 是 IMPL_DEF 页，不是 bank。）

这三个值都是**裸的低位**，与 Android dmesg 的 "24 register groups / 22 context banks" 只对上了低字节。
所以 mode 9 的 panic 摘要同时打印 **两个候选 CB 基址**（`cbA=0x22000`、`cbB=0x4000`）与 `id0..id7`：
一次上机就能判定"上下文 bank 到底从 0x2000 还是 0x20000 开始"，以及 IDR 是不是真的没写高位。

判定：
1. `start=6` 且 `cbA`/`cbB` 里有一个看起来像 CB 寄存器（非 0、非全 1）⇒ 按那个基址确定 `pages`，
   在 `ShengGpuSmmu`/`ArmSmmu4K` 层记录偏离（**不许静默改上游**，按 AGENTS §2 第 5 条写进 `PLAN.md`）。
2. `start=6` 且两个候选都是 0 ⇒ 寄存器窗口/偏移的假设错了，先只读扫 0x3da0000+0x0..0x20000。
3. `start=0` 而 `map!=0` ⇒ 先看 `mapped` 和 `leaf`，表池/权限问题。
4. 挂板复位 ⇒ 就是写 SMMU 引起的，回滚 `btn2`，把最后一行照片留下。

### 10.4 镜像与复现

```bash
# 内核（含 SMMU 板级代码；改了 package/MuyuGmuSmmu* 或 ShengGpuSmmu* 就要重来）
cd UniXNU && python3.12 unixnu build kernel -D ../UniXNU-muyu \
   --kernelcache out/ipsw/21H461/kernelcache.decompressed
# payload + vendor_boot
python3.12 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
   --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
   --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr --kernel-arg=muyu-gpu-power=9
python3.12 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
   --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_gpupower9.img
```

`fw/vendor_boot_gpupower9.img` = `6e3f28accc025aae40e4e9e24a8738f784da9d82560952bfe379daf80abecd9b`
（内核 `32d619a1c4d823f58278dfdd2737b3c17bf564a506ba439e45583b2ad2dab84d`，2026-10-29 21:15 上机）。
