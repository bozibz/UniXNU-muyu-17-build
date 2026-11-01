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

## 11. mode 11：GMU 冷启动（2026-10-30，**已成功**）

> **一句话**：mode 11 在 mode 10 之后把板级 OPP/BCM 表从 AOP cmd-db 重建，然后跑上游
> `ShengGmuColdStart()`。**2026-10-30 18:46 第一次成功**：`cold=0 ph=7 proto=0 fw=00000100
> hfi=00000001`，即固件加载、TCM 回读、RSC/PDC、CM3 释放、`FW_INIT` 与**七条 HFI 命令全部收到真 ACK**。
>
> 但第一版（`gpupower11`）在 `FW_INIT_RESULT` 超时（`cold=6 fw=00000901`），根因是**固件 chip id 用错**。
> 两轮的过程写在 `HANDOFF-2026-10-29.md` §19；**这一节只留可复现的做法与判定**。

### 11.1 mode 11 的输入（全部已有，缺一不可）

| 输入 | 来源 | 生成/校验 |
| --- | --- | --- |
| 五块 MMIO 窗口 | `fw/stock/dtb_3.dtb` | `scripts/gen-muyu-gmu-source.py` → `ShengGmuSource.h` |
| 37 个 GMU 寄存器偏移 + 19 条 RSC/PDC + 固件 sha | 同上 + `a6xx_gmu.c`/`a6xx.xml` | 同上 |
| **固件 chip id `0x070b0000`** | **CM3 镜像自带的 chip-id 比较字面量**（`0x10004104` 后跟 `0x070b0000`）| 同上（见 §11.4）|
| 五块 GMU 内存 + IOVA | mode 7 分配、mode 9 映射 | `MuyuGmuSmmu.c` / `ShengGmu.c` 的尺寸表 |
| **板级 OPP/BCM 策略** | 本机 DT 的 `qcom,gpu-pwrlevels` / `qcom,gmu-freq-table` + 主线的 A7xx 公共 BCM | `scripts/gen-muyu-gmu-votes.py` → `ShengGmuVotesSource.h` |
| AOP cmd-db | 本机 DT 的 `aop_cmd_db_region@81c60000`（+0x20000）| `ShengGmuCommandDbSource.h`（base/bytes/version=1） |
| stage-1 SMMU | mode 9（`start=0 mapped=5`）| `MuyuGmuSmmu.c` |

生成器与校验（**改任何 GPU 代码前先跑，全绿才动手**）：

```bash
python3 UniXNU-muyu/scripts/gen-muyu-gmu-votes.py     # OPP/BCM/cmd-db 头
python3 UniXNU-muyu/scripts/check-muyu-gmu-votes.py   # 生成物对 DT、主机用例、aarch64 编译
python3 UniXNU-muyu/scripts/check-muyu-gmu-smmu.py
python3 UniXNU-muyu/scripts/check-muyu-gpu-mmio.py
python3 UniXNU-muyu/scripts/check-muyu-gpu-power-host.py
```

### 11.2 它到底写什么

`ShengGmuColdStart()` = `ShengGmuLoadAndStartHfi()` + `AdrenoHfiStartupRun()`：

1. 复核 CM3 在复位、TCM retention 清、RSC 空闲、HFI 无旧 ready；
2. 19 条 RSC/PDC 写 + FALNEXT/FAL + `TCM_RET_CNTL=1`；
3. 清零 icache/dummy/debug/log、初始化 HFI 队列、按 15 个 block 把 80,680 B 固件写进 ITCM/ICACHE/DTCM；
4. **逐字读回** TCM 与 icache 内容比对（CPU 侧）；
5. 写 `FW_INIT_RESULT=0`、`BOOT_CONFIG=2`、`HFI_QTBL_ADDR/INFO`、`AHB_FENCE_RANGE_0`、`CM3_CFG=0x4052`、
   `GENERAL_10=chip id`、`GENERAL_8=log iova|3`、GBIF QSB side、`SYS_BUS_CONFIG/ICACHE/DCACHE=1`；
6. 读 DTCM+0xff8 选 mask（本机 `dtcm=0x3c0ab2a5 > 0x20010004` ⇒ mask `0x1ff`、ready `0x100`）；
7. `SYSRESET=0` 释放 CM3，轮询 `FW_INIT_RESULT`（上限 10 ms，见 §11.5）；
8. 写 `HFI_CTRL_INIT=1`，轮询 `HFI_CTRL_STATUS&1`；
9. 七条 HFI 消息（performance / bandwidth / core start / start 等），每条等真 ACK。

### 11.3 期望值与判定（本机实测）

```
gmu cold guard=1 map=0 db=0 votes=0 cold=0 ph=7 proto=0 fw=00000100 hfi=00000001
gmu cold hash=1 reset=0 ready=1/1 rw=259/176 polls=17 ao=00000000 cm3=00000001 cfg=12/3/7/0
```

| 字段 | 成功值 | 含义 |
| --- | --- | --- |
| `guard` | 1 | 前置全过（mode 9 `mapped=5` + 六个窗口 + 五块内存）|
| `map` | 0 | 窗口物理/长度与生成的 `sheng_gmu_window_*` 逐字节一致 |
| `db` / `votes` | 0 / 0 | cmd-db 解析成功（version=1）/ 板级 OPP/BCM 构造成功 |
| `cold` / `ph` | 0 / 7 | `ShengGmuSuccess`，最后阶段 = HFI ready |
| `proto` | 0 | `AdrenoHfiStartupSuccess`：**七条命令都有 ACK** |
| `fw` / `hfi` | `00000100` / `00000001` | 固件 init 完成 / HFI 队列就绪 |
| `hash` / `reset` | 1 / 0 | 固件整份 sha 校验过 / CM3 已不在复位 |
| `rw` / `polls` | 259 / 176 / 17 | I/O 次数（本板基线）|
| `ao` / `cm3` | 0 / 1 | 无致命中断；`cm3=1` 是 HFI 的 ack 位，不是故障（fatal mask `0x800000`）|
| `cfg` | 12/3/7/0 | GPU OPP 12（含 off）/ GMU OPP 3 / 带宽 7（含 off）/ ACD 0 |
| `dtcm` | `0x3c0ab2a5` | 决定 FW_INIT 的 mask/ready；**冷启动后可能略有变化**（10-29 记的是 `0x3c0aa2f5`）|

**失败判定**：

| 读数 | 含义 | 处置 |
| --- | --- | --- |
| `cold=6 ph=5 fw=00000901` | `FW_INIT_RESULT` 超时；**先怀疑固件 chip id** | 见 §11.4，不要先怀疑 SMMU |
| `cold=5 ph=4` | 配置寄存器读回不一致（`FW_INIT_RESULT/BOOT_CONFIG/QTBL/CM3_CFG/GENERAL_10…`）| 对照 §11.2 第 5 步 |
| `cold=3` | GMU 不在复位 / RSC 忙 / HFI 残留 ready | mode 10 的 `gmu win` 行 |
| `cold=2` | 固件 sha 或 15-block 计划不符 | `gmu memory digest` 的 sha 前 4 字节应为 `435c7bac` |
| `cold=1` | 参数/窗口/内存尺寸不符 | `map` 与 `guard` |
| `cold=9` | AO/CM3 致命位置位 | 看 `ao`/`cm3` |

### 11.4 ★ 固件 chip id：`0x070b0000`，不是公式算的 `0x4303b000`

第一版用 Linux 的"catalog 没有 `.gmu_chipid` 时把 GPU chip id 打包"公式，得到 `0x4303b000`，
结果 `FW_INIT_RESULT=0x901`、10 ms 超时。用 `llvm-mc --triple=thumbv7m` 反汇编 CM3 镜像发现它自己带
chip-id 检查：

```
ldr r0, =0x10004104   ; GMU chip-id 镜像
ldr r0, [r0]
ldr r1, =0x070b0000   ; 本 GPU 期望值
cmp r0, r1
```

`0x070b0000` = gen7 / minor `0x0b`(=11) / sub 0，与 DT `qcom,adreno-gpu-gen7-11-0` 完全对应。
`gen-muyu-gmu-source.py` 现在**从镜像里**取这个值：扫 `0x10004104` 后跟的字面量，按
gen/minor/sub 过滤，要求**恰好一个**匹配，否则构建失败。写死的旧结论已作废。

### 11.5 已知限制（不是本轮的问题，但要知道）

- `ShengGmu.c` 的 `poll()` 把**单次轮询上限写死成 10 ms**（Linux 对 `FW_INIT` 给 100 ms）。
  本机固件在 <10 ms 内完成，所以没挡；若以后换固件出现"总是差一点"，先看这里，不要改板级代码。
- ACD / IFPC 都**关着**（`cfg=…/0`）：ACD 需要 AOSS/ACD 前置，IFPC 需要 power-collapse 路径。
  开它们之前要单独取证。
- 成功只表示 **GMU 固件与 HFI 起来了**，**不表示** SQE/ZAP 已加载、GPU 已初始化、能提交命令。

### 11.6 镜像与复现

```bash
# 内核（改了 package/ 里任何东西都要重来）
cd UniXNU && python3.12 unixnu build kernel -D ../UniXNU-muyu \
   --kernelcache out/ipsw/21H461/kernelcache.decompressed
# payload + vendor_boot（mode 11）
python3.12 unixnu build payload -D ../UniXNU-muyu --kernel out/muyu/kernel/kernel.macho \
   --unique-chip-id 0000000000000001 --owner-trust-cache /tmp/muyu-static-system.bundle \
   --data-ramdisk ../fw/data-seed/muyu-data-200g.dbr --kernel-arg=muyu-gpu-power=11
python3.12 ../UniXNU-muyu/scripts/patch-vendor-boot.py --base ../fw/vendor_boot_lite.img \
   --payload out/muyu/payload/payload.img --output ../fw/vendor_boot_gpupower11b.img
```

| 镜像 | sha256 | 结果 |
| --- | --- | --- |
| `fw/vendor_boot_gpupower11.img` | `fa1e6755a966076a4200589dd61e645c23a205b10ba1d29f4a040a6f56064b52` | 第一版（chip id 公式值）→ `cold=6 fw=0x901` |
| **`fw/vendor_boot_gpupower11b.img`** | **`f6c85a99ec958c33628dd52f8c14e5f532c2f30b86048881d2039bcca15846f3`** | **成功**（内核 `c4eaebd5…`）|
| 照片 | `logs/2026-10-30-18-46-34-306.jpg` | 成功摘要 |
| 失败对照 | `logs/2026-10-30-18-39-31-476.jpg` | `cold=6 ph=5 fw=00000901` |

回滚：`tools/muyu-flash-mode.sh xnu fw/vendor_boot_btn2.img`（能进 iOS 的保底镜像）。

## 12. mode 12–16：GX 上电链路（2026-10-30，**卡在 RPMh 电压投票**）

> **一句话**：GMU 之后的第一件事是让 GX 电源域起来。五轮上机把寄存器写法、OOB 位、
> 轮询时长、gx_clkctl GDSC 全部排除，最后定位到：**`gpu_cc_gx_gdsc` 的 parent-supply 是
> RPMh ARC regulator `pm_v6c_s6_gfx_voter_level`，没有 CPU 侧 RSC 驱动去使能它，
> 所以 `CFG_GDSCR.POWER_UP_COMPLETE` 永远不置位。** 完整证据在
> `HANDOFF-2026-10-29.md` §20。

### 12.1 mode 一览

| mode | 做什么 | 结果 |
| --- | --- | --- |
| 12 | mode 11 + OOB GPU_SET（非 legacy 30/31，20 ms） | `ack=0`，`st 3>2`（GDSC 的 power-off 位清了） |
| 13 | mode 12 + 1 s 轮询 + GMU log 32 字 | 仍 `ack=0`；log 是 `0x21` 记录 + `deadba55` |
| 14 | mode 13 + `gx_clkctl_gx_gdsc`（0x3d68024）上电 | `0>0`：父电源没上，写不进去 |
| 15 | mode 14 + 先抬主 GX GDSC（0x9034） | `power=2`（超时），`gdscr=6822f800`，rail `0>0` |
| 16 | mode 15 + legacy OOB（16/24）+ gx_clkctl 块 dump | `cfg=050e0000`（bit16=0）、legacy `ack=0`、`blk=0/0/0/0` |

### 12.2 关键寄存器语义（已修正）

- `gpu_cc_gx_gdsc`（0x9034）是 **`qcom,support-cfg-gdscr`** 的 GDSC：
  - 使能 = 清 `GDSCR.SW_COLLAPSE(bit0)`；
  - **在位** = `CFG_GDSCR(0x9038).POWER_UP_COMPLETE(bit16)`，**不是** `GDSCR.PWR_ON(bit31)`；
  - retention 位（bit14/13）写 **GDSCR**，且必须用**上电后重读**的值（用上电前的值会把域压回去）。
- `gx_clkctl_gx_gdsc`（0x3d68024）：DT `status = "disabled"`，Linux 不启用；父电源没上时整块读 0。
- OOB：非 legacy（set 30 / ack 31 / clear 31），legacy（set 16 / ack 24 / clear 24）；
  ack 在 `GMU_GMU2HOST_INTR_INFO`，set/clear 写 `GMU_HOST2GMU_INTR_SET`，ack 清 `GMU_GMU2HOST_INTR_CLR`。

### 12.3 判定树

| 读数 | 含义 | 下一步 |
| --- | --- | --- |
| `gx power=2 cfg=...`（bit16=0） | GDSC 没完成上电 | 先查 parent-supply（RPMh voter） |
| `rail ... 0>0` | gx_clkctl 块不可访问 | 同上，父电源 |
| `ack=0`（两套位、1 s） | GMU 不会在 GX 不可用时 ack | 先解决供电，再谈 OOB |
| `on=1` 且 `st=0` | GX 上电成功 | 读 `RBBM_STATUS`/GPU ID，进 SQE/ZAP |

### 12.4 校验

```bash
python3 UniXNU-muyu/scripts/check-muyu-gx-host.py      # 寄存器/位 + 主机用例 + aarch64
python3 UniXNU-muyu/scripts/check-muyu-gpu-power-host.py   # 19 个 power 用例（含 GX）
python3 UniXNU-muyu/scripts/check-muyu-gpu-mmio.py         # 新增 CGF_GDSCR 常量
```

---

## 13. mode 27：补发 HFI `0x1e`（GMU DCVS set）——2026-11-01

### 13.1 为什么是 `0x1e`

厂商 resume 序列是 `… 0xa → 0x14×13 → 0x1e×N → 0x83 …`，而本移植**从来没发过 `0x1e`**。
`0x14` 是固件→主机的通知（**没有 ACK**，每个 resume 块恒为 13 个），**不要照发它**。
`0x1e` 才是主机发往 GMU 的频率设置命令，**254/254 都有正常 ACK**。

### 13.2 载荷（离线反汇编定案，不需要抓包）

对 `msm_kgsl.ko` 反汇编（`gen7_gmu_clock_set` `0x7b04c` → `gen7_gmu_dcvs_set` `0x7c6e4`）：

```
gen7_gmu_clock_set 是 36 字节薄封装：
  mov w2, #0xff          ; 写死
  mov w3, #0xffff        ; 写死
  bl  gen7_gmu_dcvs_set  ; 只透传 w1 (level)
```

⇒ `w3=0xffff` 必走 `0xFFFF0000 | w9`，该分支 `w9` 恒为 `0xff`
⇒ **线上 4 字：`{header, 1, gmu_count-1-level, 0xffff00ff}`**（最后一字是常数）。

`gen7_bus_ab_quantize`（频率比值编码器）只从 bus/AB 路径进入，**这条路不走它**。

### 13.3 mode 27 做了什么

在 mode 26 的基础上（`perf_table_v2=1`）额外：

```
gmuStartup.gmu_dcvs_enable = 1;
gmuStartup.gmu_dcvs_level  = 1;   // gmu[] = {off, 220MHz, 550MHz} -> 220MHz
```

即发出 `{1, 1, 0xffff00ff}` 三个数据字（wire count=4）。

### 13.4 期望值与判定

| 读数 | 含义 |
| --- | --- |
| `v2=1 f=1` | 新阶段确实挂上了（`gPerfV2Used` / `gGmuFreqSent`）|
| `proto=0` | 启动序列仍然完整（`0x1e` 被固件 ACK）|
| `proto=8` | 传输错误 —— `0x1e` 没被 ACK，需回看载荷 |
| `gmu gx power=… cfg=…` 里 `CFG_GDSCR` bit16 置位 | **GX 上电成功**（这是本轮要验证的唯一目标）|
| `proto=0` 但 `CFG_GDSCR` 仍不置位 | `0x1e` 不足以打开 GX，回到 §12 的 RPMh 结论 |

```bash
./tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpupower27.img
# 屏幕读 `gmu cold … v2=1 f=1 proto=?` 与 `gmu gx power=…`
```

### 13.5 校验

```bash
python3 UniXNU-muyu/scripts/check-muyu-gpu-power-host.py
python3 UniXNU-muyu/scripts/check-muyu-gmu-votes.py
```
