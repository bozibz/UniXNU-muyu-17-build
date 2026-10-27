# muyu 触摸移植：板级常量（2026-10-25 核实版）

来源全部是**设备自带的东西**，不是猜的：

| 来源 | 取到的内容 |
| --- | --- |
| `fw/stock/dtb_3.dtb`（CliffsP 主 DTB，`dtc -I dtb -O dts`） | SE / TLMM / GCC 的地址、SPI 引脚组、IRQ 号 |
| `fw/dtbo.img`（overlay，见下"怎么解"） | `novatek@0` 节点：频率、IRQ GPIO、LCD-ID GPIO、CS 引脚组 |
| `MiCode/Xiaomi_Kernel_OpenSource` 分支 **`muyu-v-oss`** | `drivers/clk/qcom/gcc-cliffs.c`、`drivers/pinctrl/qcom/pinctrl-cliffs.c`、`pinctrl-msm.c` |

复现方法：`dtc -I dtb -O dts fw/stock/dtb_3.dtb -o /tmp/d3.dts`；
`dtbo.img` 是 Android DTBO（magic `d7b7ab1e`，32 字节头 + 32 字节/项），
解出条目后用 dtc 反编译：条目里 `size/offset` 是大端，偏移 64 起。

## 1. 总线

| 项 | 值 |
| --- | --- |
| SE | `qupv3_0_geni_se@ac0000` 的 `spi@a90000`（SE4），base **0x00a90000**，size 0x4000 |
| QUP wrapper | **0x00ac0000** |
| SE 中断 | DT `interrupts = <0x165 4>` → GIC SPI **0x165 = 357** |
| SPI 时钟 | `spi-max-frequency = <0x124f800>` = **19,200,000** |
| 控制器 | Novatek，`compatible = "novatek,NVT-ts-spi"`，`reg = <0>` |

## 2. 时钟（`gcc-cliffs.c`，GCC base = 0x00100000）

qupv3_0 这一组三个分支共用投票寄存器 **0x52008**（GCC 内偏移）：

| 时钟 | 投票位 | halt 寄存器 |
| --- | --- | --- |
| `gcc_qupv3_wrap0_core_2x_clk` | `BIT(18)` | 0x23018 |
| `gcc_qupv3_wrap0_core_clk` | `BIT(19)` | 0x23008 |
| `gcc_qupv3_wrap0_s4_clk` | `BIT(26)` | 0x183bc |

SE4 的 RCG：`gcc_qupv3_wrap0_s4_clk_src`，`cmd_rcgr = 0x183c8`，`mnd_width = 16`，`hid_width = 5`，
`parent_map = gcc_parent_map_0`：

| parent | 值 | 频率 |
| --- | --- | --- |
| `P_BI_TCXO` | 0 | 19.2 MHz |
| `P_GCC_GPLL0_OUT_MAIN` | 1 | 600 MHz |
| `P_GCC_GPLL0_OUT_EVEN` | 6 | 300 MHz |

它的频率表里就有 `F(19200000, P_BI_TCXO, 1, 0, 0)` —— **所以直接用 TCXO、div=1 就能得到 19.2 MHz
的 SE 时钟，不需要 GPLL0**（sheng 的板子要 GPLL0，因为它的表用 GPLL0 做源；这里可以省一整步）。
RCG 寄存器布局：`CMD`@0x183c8、`CFG`@0x183cc（bit4:0 = 半整数分频，bit10:8 = 源，bit13:12 = mode）、
`M`@0x183d0、`N`@0x183d4。源=TCXO(0)、div=1、mode=0、M=N=0 时 **CFG 写 0**，然后按 clk-rcg2 的顺序
写 `CMD` 的 root-enable 位。

## 3. 引脚（`pinctrl-cliffs.c` + `pinctrl-msm.c`）

TLMM：base **0x0f000000**，stride **0x1000**，`ctl = base + 0x1000*gpio`，`io = ctl + 4`，
`mux_bit = 2`，`drive_bit = 6`，`oe_bit = 9`，`pull_bit = 0`（本设备 SPI 用 `bias-disable`）。

**关键点：`pinctrl-msm.c` 里 mux 字段不是全局函数号**：
`mask = GENMASK(mux_bit + order_base_2(g->nfuncs) - 1, mux_bit)`，而写入的是该引脚**自己**函数表里的下标
（`PINGROUP(16, qup0_se4_l0, ...)` → 0=gpio、1=qup0_se4_l0）。所以这四根线写 **mux = 1** 就对了
（全局函数号 170–173 只在算掩码时用，不要直接写进寄存器）。

| 信号 | GPIO | 引脚函数表 | 写入 mux | drive |
| --- | --- | --- | --- | --- |
| MISO | 16 | gpio, **qup0_se4_l0**, ibi_i3c, phase_flag21 | 1 | 6 |
| MOSI | 17 | gpio, **qup0_se4_l1**, ibi_i3c, phase_flag20 | 1 | 6 |
| CLK | 18 | gpio, **qup0_se4_l2**, phase_flag19 | 1 | 6 |
| CS | 19 | gpio, **qup0_se4_l3**, phase_flag18 | 1 | 6 |
| IRQ | **54**（`0x36`，flags `0x2001`） | gpio, qup0_se0_l2, nav_gpio3 | 0（GPIO） | — |
| LCD ID | **26**（`0x1a`） | —（不用） | — | — |

即 `ctl = (1 << 2) | (6 << 6) = 0x184`，四根线一样。

TLMM 的中断是**一根共享 GIC SPI**：DT `pinctrl@f000000 { interrupts = <0 0xd0 4> }` → SPI 208 →
`armcpu_spi_attach(32 + 208 = 240, ...)`（和 sheng 的 `32+208` 完全一致）。每个引脚另有
`intr_cfg`(+8)/`intr_status`(+0xc)/`intr_target`(+8) 寄存器，`intr_target_bit = 8`。

## 4. 覆盖层里的 touch 节点（`fw/dtbo.img`，fragment@38）

```dts
&qupv3_se4_spi {                 /* 主 DTB 里 status = "disabled" */
    status = "ok"; qcom,rt;
    pinctrl-0 = <&qupv3_se4_spi_mosi_active>, <&qupv3_se4_spi_miso_active>, <&qupv3_se4_spi_clk_active>;
    novatek@0 {
        compatible = "novatek,NVT-ts-spi";
        reg = <0>;
        spi-max-frequency = <0x124f800>;              /* 19.2 MHz */
        novatek,irq-gpio = <&tlmm 0x36 0x2001>;       /* gpio 54 */
        novatek,lcd_id-gpio = <&tlmm 0x1a 0x00>;      /* gpio 26 */
        pinctrl-0 = <&mi_ts_spi_cs_active>;           /* gpio19, qup0_se4_l3 */
        pinctrl-names = "pmx_ts_active", "pmx_ts_suspend";
    };
};
```

注意：**没有 `reset-gpio`**，也没有 regulator 引用 —— 说明触控的供电/复位不在 DT 里（由 PMIC 常开或
kernel 默认），移植时先假设"上电即活"，读 chip id 来验证。

## 5. 移植计划（每一级都能单独上机验证）

1. **寄存器勘察（只读，最安全）**：`muyu-touch-probe=1` 时映射 TLMM(0xf000000)、QUP wrapper(0xac0000)、
   SE(0xa90000)，读并打印：TLMM gpio16–19/54 的 ctl/io 现值、wrapper 的 `QUPV3_HW_VER`、SE 的
   `GENI_FW_REVISION_RO`/`GENI_STATUS`。**先不要写任何寄存器** —— 如果 SE 没时钟，读 0 或读挂就能看出来。
2. **开时钟 + 配引脚**：GCC 0x52008 置 BIT(18|19|26)，等各自的 halt 寄存器清零；RCG 0x183c8 设 TCXO/1；
   TLMM 四根线写 `0x184`；再读 `GENI_FW_REVISION_RO` 应为非 0/非全 1。
3. **PIO SPI 读 chip id**：`GeniSpi.c` 是 FIFO 模式（不需要 DMA，也就绕过 SMMU SID），按 19.2 MHz 配置
   `GENI_SER_M_CLK_CFG`，发 Novatek 的 `0x81 0x01`（读版本）看回包。
4. **接 Novatek 协议**：`NvtProtocol.c` 的 `nvt_bus` 用 `GeniSpi` 的 xfer；`NvtTouchCore.c` 出触点。
5. **HID**：`NativeHidInput.cpp` 的 digitizer 源套上触点（sheng 的 `TouchProbe.cpp` 里 `publish()` 就是范例）。
6. **中断**：IRQ gpio54 → TLMM `intr_cfg/target` 配置好，`armcpu_spi_attach(240, ...)` 挂处理函数。

## 5.1 仓库里已有的 `package/TouchProbe.cpp` 是 sheng 版，不要直接指过去

`UniXNU-muyu/package/TouchProbe.cpp`（594 行，`sheng-touch=<1..4>` 分级、由
`build-native-legacy-display.py` 编进核心里）是从 sheng 搬来的，它的第 3 级依赖三件 sheng 硬件特有的事：

1. **DFS**：`configureClock()` 用 `tb_choose_clock()` 扫 RCG 的 8 个 DFS level（`TB_RCG_DFSR_CMD`/`PERF_*`），
   而 cliffs 的 SE4 RCG 我们直接走 TCXO/1 就够，没有 DFS 可扫；
2. **DMA + SMMU**：`openBus()` 在 `fifo_disabled` 时强制走 DMA，`prepareDma()` 要对 `apps-smmu` 做 bypass
   （`TB_QUP_SMMU_SID = 0xa3` 是 sheng 的 stream id）；
3. **引脚/时钟常量**：`package/TouchBoard.h` 现在还是 sheng 的 SM8550 值（wrap1、gpio44–47、IRQ gpio25、
   TLMM 0xf100000）。

所以下一步建议**另写一个很小的 muyu 专用探针**（PIO/FIFO、TCXO 19.2 MHz、无 SMMU），
分级照旧：①只读勘察 → ②开时钟+配引脚+读 SE 身份 → ③PIO 读 chip id → ④接 `NvtProtocol`/`NvtTouchCore`。
sheng 那份留着当参考（`publish()`、HID 接线、THP 帧解析都在里面）。

## 6. 还没解决/待确认

- 触控控制器是否需要**固件下载**（`NvtProtocol` 有下载路径；muyu 的 vendor 分区里是否有
  `novatek_nt36532_n81a_fw_csot.bin` 还没确认 —— 先按"不需要"试，chip id 能读到就说明固件在片内）。
- `qcom,rt`（实时优先级）和 19.2 MHz 下的 SE 分频值要在第 3 步实测。
- 主 DTB 里 `spi@a90000` 的 `status` 是 `disabled`，覆盖层才打开；我们不走 DT，所以忽略。

## 7. 上机实测（2026-10-25 10:50–11:05，探针 `package/TouchProbeMuyu.cpp`）

分级探针 `muyu-touch=<1..3>` 在 dump 时刻运行（`muyu_panic_with_log()` 里先跑探针再打 dump），
所以每一行都会先 kprintf 到 console，再随 panic 一起出现在大字屏上。

### 7.1 实测结果

```
GCC vote=487c0200 extra=00000000 halt c2x=00000400 core=00000400 s4=00000400
GCC branches running: c2x=1 core=1 s4=1
SE4 rcg cmd=80000000 cfg=01000000 m=0 n=0 src=0
QUP wrapper ver=240c1801 param=030d0000
SE status=00000000 fwrev=00000002 hwparam=20102e04 sclk=00000011 clksel=00000000
```

- **SE4 的时钟本来就是开的**：三个 halt 寄存器 bit31 都是 0（运行中）。`0x52008` 的 bit18/19 已在
  `0x487c0200` 里，但 **bit26 写不进去、也不影响 halt** —— 说明在这个 SoC 上 `0x52008` 不是 SE4 的门控，
  或者该位被 TZ 保护。**不要再花时间给 0x52008 投票。**
- **wrapper 与 SE 都可读**：`ver=0x240c1801`、`fwrev=0x2`、`hwparam=0x20102e04`、`sclk=0x11`。
  注意 `0xa90000` 单独映射会失败，必须用 **0xa00000 起 1 MiB 的 QUP 窗口**（wrapper 在 +0xc0000，
  SE 在 +0x90000）。
- **TLMM（0xf000000）从内核访问会直接复位整块板**（实测两次：一次读、一次读+写）。判定为引脚控制器的
  寄存器时钟在 bootloader 交接后是关的（或该区被 XPU 保护），访问即总线错误。
  **所以不要把 TLMM 的读写放进内核探针的早期路径**；真要 mux 引脚，得先找到它的时钟门控（或从 UEFI 侧做）。

### 7.2 因此下一步

1. 先试"不碰 TLMM"的 stage 3：如果 ABL 已经把 SPI 引脚 mux 好，`geni_spi_init` + `nvt_identify` 就能直接
   拿到 chip id（`muyu-touch=3 muyu-touch-tlmm=0`）。
2. 如果拿不到（引脚确实没 mux），再解决 TLMM 门控：查 `gcc-cliffs.c` 里 TLMM 相关的时钟分支
   （`gcc_tlmm_*`）或用 UEFI 侧配置引脚。
3. 一旦 chip id 出来，就把 `NvtProtocol`/`NvtTouchCore` 接到 `GeniSpi` 的 FIFO 传输上，再挂 IRQ gpio54。

### 7.3 重要修正：读"未上电的 SE"同样会复位

2026-10-25 11:56 的 stage 4（**纯读**，遍历 SE0–SE7）也复位了。结论修正为：

- **只读 SE4（触摸所在的那一个）是安全的**，`fwrev/hwparam/sclk` 都能读；
- **遍历 SE0–SE7 是危险的**：其中某些引擎的时钟是关的，读它们同样是总线错误；
- 因此"11:32 / 11:36 那两次写测试"其实是**先打印了整张表**，复位很可能来自表格而不是写 —— 写通路是否安全
  需要单独、干净地测（`muyu-touch=5` 且不遍历）。

### 7.4 stage 3（SPI）实测：写 GENI 也会复位

`muyu-touch=3 muyu-touch-tlmm=0`（不碰 TLMM，只 `geni_spi_read_info→init→set_clock→setup→nvt_identify`）
同样把板子复位。结合 7.1：

| 操作 | 结果 |
| --- | --- |
| 读 GCC（0x100000 窗口） | ✅ 安全 |
| 读 QUP 窗口（0xa00000+1 MiB） | ✅ 安全 |
| 读 SE 寄存器（GENI_STATUS / FW_REVISION / HW_PARAM / SER_M_CLK_CFG / CLK_SEL） | ✅ 安全，值合理 |
| 读/写 TLMM（0xf000000） | ❌ 复位 |
| 走 `geni_spi_init` 那条写路径（或任何 SE 写） | ❌ 复位 |

**结论**：只读这一段已经"够用"——引擎身份拿到了；但**要发 SPI 必须先解决 QUP 的写通路**，最可能的原因是
SE 的 AHB 时钟（wrapper 的 m_ahb/s_ahb）在交接后是关的，写进去就是总线错误。下一步应当：

1. 在 `gcc-cliffs.c` 里找 **qupv3_0 的 AHB 时钟**（wrap0 的 m_ahb/s_ahb 分支，或 AOSS 侧的 `qup_ahb` 时钟）并先投票；
2. 或者**把这一步挪到 UEFI**：DXE 阶段 QUP 的时钟/引脚通常已被固件打开，在那里发一次 SPI 读 chip id 更安全，
   拿到结论后再决定内核侧怎么接；
3. `geni_spi_read_info()` 本身可能已含写操作（清中断/选 FIFO 模式），如果只需只读探测，应直接读寄存器而不要调它。

### 7.5 结论与下一步（2026-10-25 12:00）

**已证实**（都是单变量、可复现）：

| 操作 | 结果 |
| --- | --- |
| 读 GCC（`0x100000` 起 2 MiB） | ✅ 安全 |
| 读 QUP 窗口（`0xa00000` 起 1 MiB）、读 SE4 的身份寄存器 | ✅ 安全（`fwrev=0x2 hwparam=0x20102e04 sclk=0x11`）|
| 读**其它** SE（SE0–SE3/SE6/SE7） | ❌ 复位（未上电的引擎读也挂总线）|
| 写 SE4 一条良性寄存器（`0x618` IRQ clear，值为 0） | ❌ 复位 |
| 读/写 TLMM（`0xf000000`） | ❌ 复位 |
| `geni_spi_init` 全流程（多条写） | ❌ 复位（与单条写一致，不意外）|

**结论**：cliffs 上 AP→QUP 的**写通路**在 XNU 接管后不可用；读能过、写就挂。最可能的原因是 Linux 的
`qcom-geni-se` 在 probe 时做的**互连（NoC）带宽投票**（DT 里 `interconnects = <&clk_virt …>`、`qup-core`/
`qup-config`/`qup-memory` 三条）在 XNU 里没有对应驱动；其次是 AOSS 侧的 QUP 时钟门控。

**因此核内路线目前有三条可选**：

1. **补 NoC/RPMh 投票**（最根本、工作量最大）：需要 RPMh（`apps_rsc`）通道和 BIMC/NoC 的带宽投票，
   uke/sheng 也还没做；做成后 QUP/UFS/显示都能受益。
2. **把 SPI 挪到 UEFI（DXE）**：DXE 阶段 QUP 的时钟/互连是固件配好的，读 chip id 甚至整帧轮询都可行；
   但 XNU 侧用户态要用还得回到内核，只能算"先验证控制器活着"。
3. **换总线思路**：muyu 的触摸只走 SPI，没有 I²C 备选，所以不适合走 umi 那条（ST FTS over I²C）的路。

在上表任何一条打通之前，**不要把 `muyu-touch` 设成 ≥2**，否则每次启动都会复位（`muyu-touch=0` 或 1 是安全的）。

### 7.6 UEFI/DXE 探针（2026-10-25 12:30–12:52）：能加载，但观测不到输出

做了 `Mu-Silicium/Platforms/Xiaomi/muyuPkg/Drivers/MuyuTouchProbe/`（DXE 驱动，复用 `GeniSpi.c` +
`NvtProtocol.c`，加了一个 `CompatString.c` 提供 `memset/memcpy/memcmp`，因为 DXE 没有 C 库）。

**已经打通的部分**（下次直接复用）：

- 编进固件：`muyu.dsc` 的 `[Components]` + `muyu.fdf` 的 `INF` 两处；`.inf` 必须带
  `UefiDriverEntryPoint`（否则 `lld-link: undefined symbol _ModuleEntryPoint`）；
- 构建：`unixnu build firmware -D ../UniXNU-muyu --uefi ../Mu-Silicium` → `SILICIUM_UEFI.fd`，
  再 `unixnu bootimg …` 生成 boot.img（结尾 `boot image header does not match` 是已知误报）；
- 加 `DxeServicesTableLib` 后用 `gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, …, EFI_MEMORY_UC)` +
  `SetMemorySpaceAttributes` 先把 GCC/TLMM/QUP 三个窗口映射进来（DXE 默认不映射设备地址）。

**卡在观测**：三次上机（12:35 / 12:41 / 12:50）屏幕上都只有 BDS 的
`Loading driver at 0x… EntryPoint=0x… MuyuTouchProbe.efi`，随后约 20 秒复位，**看不到任何 `Print` 输出**
（包括加在入口、且在碰硬件前停 20 秒的那一版）。最可能的原因：驱动在 DXE dispatch 阶段运行，
**那时 `ConOut` 还没连上**，`Print` 静默失败；接着访问未映射/无时钟的 MMIO → 异常 → 看门狗复位。

**下次的正确入口**：不要做成 DXE 驱动，而是做成 **`UniXNUPkg/Application` 里的一个菜单项**
（UniXNU 菜单运行时 console 已经可用、还能按键选择），或者用协议通知等 `ConOut` 出现后再跑。
探针代码（时钟、引脚、GENI 初始化、`nvt_identify`）已经写好，搬到 app 里即可。

固件已恢复为不含探针的版本（`muyu.fdf` 里那行 INF 被注释掉），`boot.img` 也已重新生成。

## 8. UEFI 侧分级探针（2026-10-25 13:42 第一次上机）

§7.6 的结论是"要做成菜单 app，等 console 可用"。实际做法换了个更方便的入口：**BDS 的
`PlatformBootManagerAfterConsole`**。驱动 `muyuPkg/Drivers/MuyuTouchProbe` 的入口只
`InstallProtocolInterface`（什么都不碰），BDS 拿到协议后调 `Run()`。这个位置：

- 所有 DXE 驱动都 dispatch 完了；
- GOP `Blt` 可用（BDS 自己就在这里画白条和四个色块），所以**不用文本 console**：
  自带 5×12 字体（`DiagFont.h`，和 DarwinBoot 的大字同一份）整屏画表；
- 每步开始/结束都把进度写进 **NV 变量 `MuyuTouchTrace`**（`Tag/Boots/Early/State[10]/Text[10][96]`）。
  某一步把板子写复位了，下次启动看到 `state=RUN` 就把它改成 `RSET` 并**跳过**，
  所以多次启动能凑齐完整表格；连续三次"还没开始就复位"也会放弃，避免死循环。

十步：`gcc / wrap / se4 / geni`（只读）→ `gccw`（写 GCC 投票）→ `geniw`（第一批 QUP 写）→
`clk`（时钟+setup）→ `loop`（SE 自带回环）→ `nvt`（nvt_identify）→ `snap`（失败现场寄存器）。

### 8.1 实测（boot 1，屏幕照片）

| 步骤 | 结果 |
| --- | --- |
| S1 `gcc` 读 | ✅ `vote=487c0200 rcg=80000000/00100000 m=0 n=0 halt=80004000/4000/4000` |
| S2 `wrap` | ✅ `ver=240c1801` |
| S3 `se4` 读 | ✅ `fwrev=00000002 hw=20102e04 sclk=00000011 st=0` |
| S4 `geni` 读描述符 | ✅ `proto=1 fifo=16/32 qup=30000000 txfs=0 rxfs=0` |
| S5 `gccw` 写投票 | ✅ **bit26 写进去了**：`before=487c0200 after=4c7c0200 diff=04000000`，同时 S4 的 CBCR 从 `80004000` 变 `4000`（bit31 清零） |
| S6 `geniw` GENI 初始化 | ✅ **rc=0**（十几条 QUP 写全部成功，没有复位）|
| S7 `clk` 时钟+setup | ✅ `rc=0 setup=0 hz=1000000` |
| S8 `nvt` | ❌ `rc=-5`（NVT_ERR_BUS），id 全 0 |
| S9 `tlmmr` 读引脚 | ❌ **复位整板** |

### 8.2 结论（这一条改写了 §7.5 的推断）

1. **UEFI 里 QUP 写是通的**：`geni_spi_init` 那十几条写返回 0，板子没复位。
   而 XNU 里同样的写会复位 —— **不是 QUP 本身有写保护，是 XNU 接管之后的环境差异**。
2. **S4 分支在 BDS 时是关的**：`0x52008` 的 bit26 = 0、CBCR `0x183bc` bit31 = 1（CLK_OFF）。
   写 bit26 之后 CBCR 的 bit31 清零 —— 即 **bit26 就是 S4 的使能投票**。
   注意 XNU 侧的探针**从头到尾只读、没写过 GCC**（`TouchProbeMuyu.cpp` 里唯一的写是 SE 寄存器），
   所以"XNU 写 QUP 就复位"最可能的解释是 **分支时钟没开就写**，而不是 XPU。
3. **TLMM 从 UEFI 里读也会复位**（不经过 XNU）—— 这是 bootloader/TZ 级别的封锁（或者该 block 的
   时钟/XPU 没开），**AP 侧目前没有任何引脚 mux 的通路**。这一条和固件无关，也和 XNU 无关。
4. `nvt_identify` 在 UEFI 里同样失败，但 `rc=-5` 只说明第一次 SPI 传输没成功，
   还缺"GENI 到底是超时还是 IO 错误"以及失败现场，所以下一版加了 `loop`（SE 回环，不依赖引脚）
   和 `snap`（`M_IRQ_STATUS`/`GENI_STATUS`/TX/RX FIFO 状态）。

### 8.3 因此的下一步

XNU 侧新增 stage 6–9（`package/TouchProbeMuyu.cpp`），每级只多一条写，复位也能定位：

| stage | 做什么 |
| --- | --- |
| 6 | 写 `0x52008 |= bit26`，回读投票与三条 CBCR（**只写 GCC，不碰 QUP**）|
| 7 | 6 + 把 SE4 `0x28`（CGC_CTRL）原值写回（一条良性 QUP 写）|
| 8 | 7 + 写 SE4 `0x618`（IRQ clear，之前复位的那一条）|
| 9 | 8 + GENI info/init/clock/setup + `nvt_identify` + 失败快照 |

判读：stage 6 复位 ⇒ XNU 连 GCC 都写不了；stage 6 能过、7 复位 ⇒ QUP 写通路仍不通（且不是时钟）；
6/7 都能过 ⇒ **触摸的 QUP 通路在 XNU 里是可用的前提被打破，接着把 `nvt_identify` 的 ID 读出来**。

### 8.4 RCG 位定义（vendor 内核源码核对，2026-10-25 14:05）

`drivers/clk/qcom/clk-rcg2.c`（MiCode `muyu-v-oss`）：

| 寄存器 | 位 | 含义 |
| --- | --- | --- |
| CMD = `cmd_rcgr + 0x0` | BIT(0) | `CMD_UPDATE`（**自清零**：写完等它变 0，表示配置已生效）|
| | **BIT(1)** | `CMD_ROOT_EN`（**root 使能**）|
| | BIT(31) | `CMD_ROOT_OFF`（状态：root 关着）|
| CFG = `+0x4` | [4:0] | hid 分频（写 0 → 硬件分频 = 1）|
| | [10:8] | 源选择（`gcc_parent_map_0`：0 = P_BI_TCXO）|
| | [13:12] | mode（0 = 单边）|
| | BIT(20) | `CFG_HW_CLK_CTRL`（硬件时钟控制）|

`gcc_qupv3_wrap0_s4_clk_src`（`gcc-cliffs.c`）的 flags 是 **`HW_CLK_CTRL_MODE` + `enable_safe_config = true`、没有
`FORCE_ENABLE_RCG`**，因此 vendor 的 `clk_rcg2_enable()` 流程是：

1. `set_force_enable()` → **写 CMD_ROOT_EN(BIT1)**，轮询到 `ROOT_OFF(BIT31)` 清零；
2. `configure()` → 写 CFG/M/N，再写 `CMD_UPDATE(BIT0)` 并等它自清；
3. `clear_force_enable()` → 清掉 BIT1。

第 3 步是因为**硬件时钟控制器**（AOSS/AOP 那条路）接管了 root 的开关；我们这边没有那个驱动，所以**必须把
`CMD_ROOT_EN` 留着不放**（强制使能），root 才会跑起来。

**踩过的坑**：第一版探针把 `CMD_ROOT_EN` 当成 BIT(0)（实际上是 `CMD_UPDATE`），写进去只触发了一次 latch，
root 仍然是 `0x80000000`（off），所以 `loop` 照样超时。v5 固件与 stage 7 起改用 BIT(1) 并保留。

## 9. 修正 §7.5：XNU 侧 QUP 写是通的（2026-10-25 14:18 实测）

### 9.1 实测（stage 9）

`muyu-touch=9`、`muyu-dump-at=60`，同一启动里 UEFI 探针先跑过（rd 了投票 + RCG），XNU 在 65 s 打 dump：

```
MUYU TOUCH GCC vote=4c7c0200 extra=00000000 halt c2x=00004000 core=00004000 s4=00004000
MUYU TOUCH SE4 rcg cmd=00000000 cfg=00100000 m=00000000 n=00000000 src=0
MUYU TOUCH vote before=4c7c0200 cbcrs=00004000/00004000/00004000
MUYU TOUCH vote after=4c7c0200 wanted=4c7c0200 s4 halt=00004000
MUYU TOUCH rcg cmd 00000000 -> 00000000 cfg 00100000 -> 00100000 s4 halt=00004000
MUYU TOUCH SE4 0x28 write back 0000005f -> 0000005f          ← QUP 写，成功
MUYU TOUCH SE4 0x618 =0 irq=00000000 status=00000000         ← 以前"一写就复位"的那一条，这次没事
MUYU TOUCH geni info rc=0 fw=00000002 proto=00000001 fifo_dis=0
MUYU TOUCH geni init rc=0 oversampling=1
MUYU TOUCH geni clock rc=0 hz=19200000
MUYU TOUCH geni setup rc=0
MUYU TOUCH nvt_identify rc=-19 id=00 00 00 00 00 00          ← 传输**成功**，只是 ID 不匹配
```

**板子没有复位。** `nvt_identify` 返回 `-19`（`NVT_ERR_CHIP`，5 次读到的 ID 都不匹配），
而不是 `-5`（`NVT_ERR_BUS`）——说明**每一帧 SPI 传输都完整跑完了**。

### 9.2 结论

1. **§7.5 的"XNU 里 QUP 写通路不可用"是错的。** 真正的条件：**先给 SE 投票（`0x52008` bit26）
   并把 root divider latch 上（`0x183c8`）**；在**时钟没开**的时候写 SE 寄存器，才会挂总线 → 整板复位。
   一旦时钟通了，`0x28`、`0x618`、整个 `geni_spi_init` 都能写。
2. 因此**触摸在 XNU 里是可做的**，剩下的问题不是"写不进去"，而是**引脚 mux**（TLMM 仍是死路）
   和 **CS 没有拉**。
3. `nvt_identify` 读到全 0 的最可能原因：**SPI 从来没有把 CS 拉低**。共享驱动
   `drivers/NativeGeniSpi/GeniSpi.c` 明确不清 `GENI_CS_TOGGLE`、也不发 CS 命令，而 vendor 的
   `spi-geni-qcom.c` 是**每次传输前用 `SPI_CS_ASSERT`（opcode 8）、之后 `SPI_CS_DEASSERT`（9）**
   单独发一条命令。Novatek 在 CS 没拉低时根本不会驱动 MISO → 读回全 0。

### 9.3 下一步（stage 11）

`package/TouchProbeMuyu.cpp` 增加 **stage 11**：同样的 init + 19.2 MHz，但每次传输前后
分别发 `SPI_CS_ASSERT` / `SPI_CS_DEASSERT`（轮询 `M_CMD_DONE`），然后 `nvt_identify`。

- 若 ID 变成 `.. .. .. 32 65 03` ⇒ **SPI 通了，触摸控制器活着**，剩下只有 IRQ/gpio54 + 引脚 mux；
- 若仍是全 0 但 `cs assert rc=0` ⇒ CS 通了但引脚没 mux（MISO 悬空）→ 回去解决 TLMM；
- 若 `cs assert rc=-1` ⇒ 连 CS 命令都不完成，说明 SE 仍缺东西（再查 AOP/NoC 侧）。

## 10. FIFO 接口是关的；DMA 也读不到数据 → 卡在引脚 mux（2026-10-25 14:44）

### 10.1 stage 11/12 实测

```
stage 11（FIFO + 手动 CS）
  SE4 if_disable … fifo_dis=1                       ← 关键
  cs assert rc=0 / cs deassert rc=0                 ← CS 命令能跑
  cs nvt_identify rc=-5 id=00 00 00 00 00 00       ← 数据命令不完成
  cs snapshot irq=00000000 status=00000004

stage 12（DMA + apps-SMMU pass-through）
  SE4 if_disable=00000001 hwparam0=20102e04          ← FIFO_IF_DISABLE 确认，hwparam 正常
  smmu slot=… bank=… reuse=0 ready=1 err=none
  smmu configure ok                                  ← pass-through 装上（SID 0xa3）
  dma init rc=0 fifo_dis=1
  dma clock/setup rc=0 hz=19200000
  dma nvt rc=-19 id=00 00 00 00 00 00                ← 传输"完成"，但数据全 0
  dma snap t=0 r=0 ti=0 ri=0 irq=0 st=0 rxfs=0       ← 没有超时、没有 AHB 错误
```

### 10.2 解读

1. **`GENI_IF_DISABLE_RO` 的 bit0 = 1**：这个 SE 的 FIFO 接口是关的（vendor 的 `spi-geni-qcom.c`
   遇到它会优先用 GPI DMA，取不到 DMA 通道才回退 FIFO）。所以 FIFO 路线的失败不是时钟问题。
2. **DMA 路线能装起来**：apps-SMMU（`0x15000000`，SID `0xa3`，与 DT `qupv3_0_geni_se@ac0000`
   的 `iommus` 一致）的 pass-through 银行配置成功（`smmu configure ok`），SE 的 DMA 传输
   **不超时、不报 AHB 错误**，`nvt_identify` 只是"ID 不匹配"（-19）。
3. **但读回的数据全是 0** —— 传输"完成"却没有 MISO 数据。最一致的解释：**四根 SPI 引脚没有
   mux 成 SPI 功能**（CS/CLK 没接到控制器，控制器不驱动 MISO），而不是总线或时钟问题。
4. TLMM 目前是唯一挡路的东西：任何**读**都会复位整板（UEFI 与 XNU 都一样，见 §8、§9）。

### 10.3 下一步：盲写引脚 mux

从没试过"**只写不读**"TLMM。固件探针 v6（`run6`）新增两步：

- **S12 `tlmmw`**：向 gpio16–19 的 ctl 写 `0x184`（mux=1 `qup0_se4_lN`、drive 6 mA），**不读回**；
- **S13 `nvt2`**：如果板子没复位，重新 init + 19.2 MHz + setup + `nvt_identify`
  —— 这一步是真正的判据：
  - ID 出现 `32 65 03` ⇒ **盲写有效、触摸通了**；
  - 仍是全 0 ⇒ 写虽然没复位但引脚仍不通（或控制器没供电）；
  - S12 就复位 ⇒ 连盲写都会挂总线，AP 侧彻底没有 TLMM 通路，只能考虑
    TrustZone 的 SCM IO 调用（`qcom_scm_io_write`）或 AOP 侧代劳。

## 11. 盲写 TLMM 也复位；TZ 的 SCM IO 服务不可用（2026-10-25 14:50 / 15:03）

### 11.1 盲写（固件探针 v6 的 S12）

`tlmmw` 只写 gpio16–19 的 ctl（`0x184`）、**不读回**，写下去之后立刻复位。所以 TLMM 对 AP 是
**读、写都不通**，UEFI 与 XNU 表现一致 —— 这不是系统侧的问题。

### 11.2 请 TrustZone 代劳（XNU 探针 stage 13）

`Mu-Silicium/Silicon/Qualcomm/QcomPkg/Include/Protocol/scm_sip_interface.h` 里有现成的 SIP 定义：

| 项 | 值 |
| --- | --- |
| `TZ_OWNER_SIP` | 2 |
| `TZ_SVC_IO_ACCESS` | 5 |
| `TZ_IO_ACCESS_READ_ID` | `TZ_SYSCALL_CREATE_SMC_ID(2, 5, 0x01)` = **0x02000501** |
| `TZ_IO_ACCESS_WRITE_ID` | **0x02000502** |
| 文档 | "Allows reading/writing of memory areas that fall under Memory/Area/Register Protection Unit (xPU) protection but do not need to be secured" |

两种调用约定都试了（只读，安全）：

- **A**（Linux `qcom_scm` 风格）：`a0 = 0x0501`、`a1 = 地址`、`a5 = arginfo(1)`
- **B**（固件 param-id 风格）：`a0 = 0x02000501`、`a1 = 1`、`a2 = 地址`、`a3 = 输出物理指针`

四个调用（两种约定 × gpio16/gpio54）**全部返回 `0xffffffffffffffff`**（SMCCC `SMC_UNK`，未实现）：

```
MUYU TOUCH scm A rd0  rc=ffffffffffffffff val=0
MUYU TOUCH scm A rd54 rc=ffffffffffffffff val=0
MUYU TOUCH scm B rd0  rc=ffffffffffffffff out=00000000
MUYU TOUCH scm B rd54 rc=ffffffffffffffff out=00000000
MUYU TOUCH scm: no working convention, pins stay unmuxed
```

结论：**这颗 SoC 的 TZ 没有把这个 IO 服务放出来**（vendor 的 `pinctrl-msm.c` 用
`qcom_scm_io_readl/writel` 只针对部分平台的 intr-target 寄存器，cliffs 不在其中）。

### 11.3 还没试过的组合：DMA + CS

回头看 stage 11/12 的差别：stage 11（FIFO）拉了 CS，但 FIFO 接口是关的；stage 12（DMA）能传，
但**从来没拉过 CS**，Novatek 在 CS 高电平时不会驱动 MISO。两者从没一起用过，所以：

**stage 14 = DMA + 每条传输前后 `SPI_CS_ASSERT`/`SPI_CS_DEASSERT`**（`muyu-touch=14`）。
如果 ID 还是全 0，基本就可以确定是"引脚没 mux + AP 没有 TLMM 通路"，那触摸就只剩
AOP 代劳或 bootloader 侧配置这两条路。

### 11.4 stage 14（DMA + CS）实测：仍然全 0（2026-10-25 15:10）

```
SE4 if_disable=00000001 hwparam0=20102e04 fifo_depth=0 width=0
dma mem phys=0xc8c1400000 seg=131072
smmu slot=… reuse=0 ready=1 err=none
smmu configure ok
dma init rc=0 fifo_dis=1
dma+cs assert rc=0
dma+cs deassert rc=0
dma clock/setup rc=0 hz=19200000
dma nvt rc=-19 id=00 00 00 00 00 00        ← 传输完成、CS 也拉了、还是没数据
dma+cs last deassert rc=0
dma snap t=0 r=0 ti=0 ri=0 irq=0 st=0 rxfs=0
```

**结论**：时钟、投票、RCG、DMA、SMMU pass-through、CS 全都工作，**MISO 依然一个 bit 都没有**。
SPI 主机侧已经没有任何未验证的环节；唯一剩下的解释是**四根引脚没有 mux 成 SPI 功能**
（CLK/MOSI 送不到控制器，MISO 自然也不会被驱动），而 AP 侧对 TLMM 读、写都会复位整板，
TrustZone 的 `TZ_SVC_IO_ACCESS` 又不开放。

**所以触摸移植在本场就到"引脚 mux"这堵墙为止**：XNU 侧的 QUP 通路已经证明可用，
一旦引脚问题解决（AOP 代劳 / bootloader 配置 / 其他途径），把它接到
`NativeNovatekTouch` + 轮询（不依赖 gpio54 中断，因为中断配置同样要写 TLMM）即可。

## 12. TLMM 不是"整块死":只有共享给 trusted VM 的那几个 GPIO 页被锁（2026-10-25 15:24）

### 12.1 判别实验

一次只读一个 TLMM 地址（`muyu-touch-rd=<偏移>`，stage 15），**gpio0** 的 ctl（`0xf000000`）：

```
MUYU TOUCH tlmm read +0 = 01000001 (survived)
```

**读成功了，板子没复位。** 所以：

- TLMM **整块是可以访问的**（之前"TLMM 死路"的说法要修正）；
- 复位只发生在 **gpio16–19 / gpio54**（我们那四根 SPI + 触摸 IRQ）这几个地址上。

### 12.2 为什么偏偏是这几个

`fw/stock/dtb_*.dtb` 里有一个节点，五个 DTB 全都有：

```dts
tlmm-vm-mem-access {
    compatible = "qcom,tlmm-vm-mem-access";
    qcom,master;
    tlmm-vm-gpio-list = <... >;      /* 解出来是 16,17,18,19,44,45,46,47,128,129,131,29,177,54,77,78 */
};
```

对应驱动 `drivers/soc/qcom/gh_tlmm_vm_mem_access.c`（MiCode `muyu-v-oss`）：

- 它按 `tlmm-vm-gpio-list` 把**这些 GPIO 的寄存器页**通过 Gunyah 的
  `ghd_rm_mem_share(GH_RM_MEM_TYPE_IO, …, GH_TLMM_MEM_LABEL=8, …)` 共享给
  `GH_TRUSTED_VM`（ACL：trusted VM 只读，primary VM 读写）；
- 也就是 Qualcomm 的 "TLMM VM" 机制：**引脚寄存器页是可以在 VM 之间划转的**，
  触摸那几根线正是被划给安全前端（TUI）用的那几个。

**所以复位不是 XPU 乱封，也不是没时钟** —— 是这几页在我们这个 VM 里被摘走/锁住了，
访问它们就是非法访问，Hypervisor/TZ 直接把整板复位。gpio0 不在这张名单里，所以正常。

### 12.3 这条线索把问题缩小到"要回那几页"

剩下两条可能：

1. **Gunyah RM**：让 AP 重新 `reclaim`/取消共享这几页（需要实现 gh_dbl + RM 消息，
   驱动源码可参考 `drivers/soc/qcom/gh_tlmm_vm_mem_access.c` + `gh_rm_drv.h`）；
2. **本来就不需要 mux**：如果 XBL/安全侧已经把 gpio16–19 mux 成 SPI（它们正是 TUI 的触摸线），
   那我们只要解决"控制器不应答"（供电/复位/模式）就能读 ID —— 但目前的实测是全 0。

### 12.4 还没做的便宜实验（下一场从这里开始）

| 实验 | 目的 |
| --- | --- |
| `muyu-touch-rd=0x14000`（gpio20，不在名单） | 确认"名单 = 复位集合" |
| `muyu-touch-rd=0x2c000`（gpio44，**在**名单） | 同上，反向验证 |
| 盲写 gpio20（不在名单）后再读回 | 确认非名单 GPIO **可以写**（那 TLMM 就是完整可用，只差那几页）|
| 读 TLMM 的 **io 寄存器**（`gpio16*0x1000 + 4`） | 也许只有 ctl 页被锁 |
| 试 TLMM 高偏移（`0x100000`/`0x200000`…）| 找 "TLMM MPU / VM" 配置寄存器，看能否自己把页要回来 |

## 13. 重大修正：TLMM 的 GPIO 寄存器在 `0xf100000 + 0x1000*gpio`（2026-10-25 15:44）

§11/§12 的"TLMM 一碰就复位""只有名单里的 GPIO 页被锁"**都是错的**，根因是我们自己的地址算错了：

```c
/* drivers/pinctrl/qcom/pinctrl-cliffs.c */
#define REG_BASE 0x100000
#define REG_SIZE 0x1000
	.ctl_reg = REG_BASE + REG_SIZE * id,      /* = 0xf000000 + 0x100000 + 0x1000*gpio */
```

| 引脚 | 我们之前踩的地址 | 真正地址 |
| --- | --- | --- |
| gpio16 (MISO) | `0xf010000` ❌ | **`0xf110000`** |
| gpio17 (MOSI) | `0xf011000` ❌ | `0xf111000` |
| gpio18 (CLK) | `0xf012000` ❌ | `0xf112000` |
| gpio19 (CS) | `0xf013000` ❌ | `0xf113000` |
| gpio54 (IRQ) | `0xf036000` ❌ | `0xf136000` |
| gpio0 | `0xf000000`（恰好不是 GPIO 区）| `0xf100000` |

所以：

- 所有"TLMM 读写 = 复位整板"的观测，都是踩到 GPIO 区**之前**那块未映射/受保护的地址；
- `gpio0` 那次能读出 `01000001`，是因为 `0xf000000` 本身是 TLMM 窗口里的另一个寄存器；
- 换成正确基址（`TouchBoardMuyu.h` 的 `TM_TLMM_BASE` 已改为 `0x0f100000`）后，**读写引脚全部正常、无复位**；
- **四根 SPI 引脚本来就已经是 mux=1（`qup0_se4_lN`）、drive 6、ctl=`0x184`** —— XBL/ABL 早就配好了，
  `mi_ts_spi_*` 的 pinctrl 只是重复一遍；
- gpio54（IRQ）ctl=`0x00000000`（GPIO 输入，无上拉），gpio26（LCD-ID）ctl=`0x00000000`。

### 13.1 那触摸还差什么

改对地址之后，主机侧**每一个环节都验证过了**：

| 环节 | 状态 |
| --- | --- |
| 四根引脚 mux | ✅ 已是 SPI 功能（`0x184`）|
| QUP 分支投票 + RCG root | ✅ `on=1` |
| GENI init/clock/setup | ✅ `rc=0` |
| FIFO | ❌ 这个 SE 的 `FIFO_IF_DISABLE=1` |
| SE 自带 DMA + apps-SMMU pass-through | ✅ 配置成功、传输返回 0 |
| CS 命令（ASSERT/DEASSERT） | ✅ `rc=0` |
| 引擎自报搬运字节 `SE_DMA_*_LEN_IN` | ❌ 读到 **0** |

`nvt_identify` 仍返回 `-19`（ID 不匹配）、id 全 0，`M_IRQ_STATUS/GENI_STATUS/FIFO 状态` 全 0。
下一步用**引擎内部回环**（`SE_SPI_LOOPBACK=1`，不经引脚、不经控制器）判定：
回环能把发出去的字节原样收回来 ⇒ 引擎正常、问题在控制器（供电/复位）；
回环也拿不到 ⇒ 引擎在"假传"，必须换 GPI/GSI 或强制 FIFO。

## 14. 内部回环通过：GENI 引擎是好的，问题在控制器（2026-10-25 16:11，stage 19）

`muyu-touch=19`：同样的 DMA 初始化，打开 `SE_SPI_LOOPBACK`，发 8 字节已知图案，不经引脚、不经控制器。

```
MUYU TOUCH dma clock/setup rc=0 hz=19200000
MUYU TOUCH loop setup rc=0
MUYU TOUCH loop rc=0 txin=8 rxin=8
MUYU TOUCH loop rx=a5 5a 01 23 45 67 89 fe      ← 就是我们发出去的那 8 个字节
```

**结论：XNU 侧这条 SPI 链路已经完全正确**——时钟、GENI 初始化、DMA、RX 通路、缓存维护、
apps-SMMU pass-through 全部工作，引擎确实在移位数据。

而 `nvt_identify` 用同一条链路读控制器时得到全 0 ⇒ **控制器没有应答**。

### 14.1 还剩的假设（下一场按这个顺序试）

| # | 假设 | 怎么验（都很便宜） |
| --- | --- | --- |
| 1 | SPI 模式/速率不匹配 | 加参数试 CPOL/CPHA = 0..3、100 kHz / 1 MHz / 19.2 MHz |
| 2 | CS 拉低后需要等待（Novatek 唤醒延迟）| 断言 CS 后 `IODelay(10ms)` 再传输；或先发一个 0xFF 唤醒 |
| 3 | 控制器芯片型号/协议不同（DTBO 是 `novatek,NVT-ts-spi`，我们沿用的是 sheng 的 NT36532E 序列）| 用 vendor 的 `nvt_identify` 变体试不同 `chip_ver_trim` 地址；先只做"读 6 字节 ID"不做引擎复位 |
| 4 | 控制器没供电 / 在复位 | DT 里既没有 `reset-gpio` 也没有 regulator ⇒ 供电是常开或与面板共用；要确认必须读 PMIC（`pmk8550`/`pm8550` 的 LDO 状态），需要在 XNU 里加一个最小 SPMI 驱动 |
| 5 | LCD-ID（gpio26）能提示面板/触摸变体 | 把它设成 GPIO 输入读一次（现在读到的 ctl/io 都是 0）|

### 14.2 已经可以确定不用再查的

- 引脚 mux（本来就是 `0x184`）；
- QUP 时钟/投票/RCG；
- FIFO 接口（`FIFO_IF_DISABLE=1`，走 DMA 即可）；
- SE 自带 DMA + apps-SMMU（回环证明可用）；
- CS 命令执行（`rc=0`）。

## 15. 主机侧全部排除；引脚电平"读不出"本身就是线索（2026-10-25 16:22）

### 15.1 模式/速率矩阵（stage 20）

12 种组合（mode 0–3 × 100 kHz / 1 MHz / 19.2 MHz，每条传输都包 CS）：

```
mx smmu err=none / mx init rc=0 fifo_dis=1
mx mode0 100khz rc=-19 id=00 00 00 00 00 00
…（mode0/1/2/3 × 三档速率，全部 rc=-19、id 全 0）
```

**结论**：SPI 模式、速率、CS 都不是原因。

### 15.2 焊盘电平（stage 21）

把 MISO/MOSI/CS/IRQ/LCD-ID 切成 GPIO 输入、分别加上拉与下拉，各读一次 io 寄存器：

```
pad miso  gpio16 ctl0=00000184 up=0 down=0
pad mosi  gpio17 ctl0=00000184 up=0 down=0
pad cs    gpio19 ctl0=00000184 up=0 down=0
pad irq   gpio54 ctl0=00000000 up=0 down=0
pad lcdid gpio26 ctl0=00000000 up=0 down=0
```

五个焊盘无论上拉还是下拉都读到 0 —— 连"跟着上拉变 1"都没有。这说明**要么 TLMM 的 io 回读在这些引脚上不可用，
要么我们对这些引脚的 ctl 写被过滤了**（stage 17 的 `oe` 位没写进去也是同一个征兆）。

### 15.3 下一场第一个实验（关键，1 次启动）

**拿 gpio0（不在 `tlmm-vm-gpio-list` 里）做同样的操作**，和 gpio16（在名单里）对比：

| 操作 | gpio0（名单外）| gpio16（名单内）|
| --- | --- | --- |
| 写 ctl 带 `oe`（`0x380`）后回读 | ? | `0x180`（bit9 丢了）|
| 设上拉后读 io | ? | `0` |
| 设输出高/低后读 io | ? | ? |

- 如果 gpio0 的 oe/上拉/回读**全部正常** ⇒ 名单里的引脚（也就是触摸那几根）确实被"TLMM VM"机制部分过滤，
  那就必须走 Gunyah RM 或让安全侧放开——这是 hypervisor 层的工作；
- 如果 gpio0 也一样（oe 写不进、io 恒 0）⇒ 我们对 TLMM 的编程模型理解还有一处错（例如 oe/in 使能在别的寄存器），
  需要重新对照 `pinctrl-msm.c` 的 `msm_config_set` / `msm_gpio_get`。

### 15.4 之后的顺序

1. 上面这个对比实验；
2. **控制器供电**：XNU 里加最小 SPMI 驱动读 `pmk8550`/`pm8550` 的 LDO 使能状态（DT 里触摸没有 regulator/reset-gpio，
   只能靠 PMIC 侧确认）；
3. 若供电确认正常，则怀疑控制器型号/协议（DTBO 的 `novatek,NVT-ts-spi` 与 sheng 用的 NT36532E 序列可能不同）。

---

## 16. 2026-10-25 16:44 · stage 22：引脚控制器的读写是真的，§13 的"0x184 是 bootloader 配的"要划掉

startup 阶段新加的 stage 22（`padTruthStage`）在同一只 boot 里做两件事：

### 16.1 gpio0（名单外）vs gpio16（MISO，名单内）

每一步都打印 ctl **和** io 的回读，而不是只打最后电平（这正是 stage 17/18/21 判错的原因）：

```
pad g0  ctl0=00000801 io0=00000000 ctl1(out1)=00000b80 io1=00000002
pad g0  io2(out0)=00000000 ctl3(pullup)=00000983 io3=00000000 io4(pulldn)=00000000
pad g16 ctl0=00000001 io0=00000000 ctl1(out1)=00000380 io1=00000003
pad g16 io2(out0)=00000000 ctl3(pullup)=00000183 io3=00000000 io4(pulldn)=00000000
```

- **oe（bit9）、pull（bit1:0）、drive（bit8:6）在 gpio0 和 gpio16 上都写得进去**（`ctl1=0x380`、`ctl3=...0x183`），
  §15 里"名单里的引脚写被过滤"**不成立**；
- `io1=00000003`：把 gpio16 当输出拉高后，**io bit0 真的读到 1**（bit1 = 我们写的输出值）。所以
  `in_bit=0 / out_bit=1 / oe_bit=9` 的模型正确，回读通路是好的；
- 两次 `pull-up` 都读 0 是采样太早（见 §18）；
- `ctl0` 是 **0x00000001 / 0x00000801**，不是 0x184：**上电时四根 SPI 引脚并不是 mux 好的**，
  之前那个 0x184 是我们自己 stage 16 写进去后回读出来的。§13"XBL/ABL 早就配好了"**作废**。

### 16.2 CS 交给引脚控制器

把 gpio19 切成 GPIO 输出、拉低（`ctl19=00000380 io19=00000000`），16/17/18 保持 SPI 功能，
再走同一套 DMA 发了四组 8 字节图案（0x81 0x01 / 0xFF / 0x00 / 55 AA 花式）：

```
cs gpio geni rc=0 fifo_dis=1
cs gpio p0..p3 rc=0 rx=00 00 00 00 00 00 00 00
cs gpio miso=00000000 rxin=8 irq=00000080
```

`rxin=8` = 引擎确实搬了 8 字节；回读仍然全 0。

---

## 17. 2026-10-25 16:52 · stage 23：手工 bit-bang + 11 根候选引脚 + ramoops

`bbProbeStage`：完全不碰 GENI，用 MOSI/CLK/CS 三个 GPIO 输出 + MISO 一个 GPIO 输入，
按 `NvtProtocol.c` 的帧（`0xff,(a>>15),(a>>7)` → `(a&0x7f)|0x80` + 6 个 0 → 读 8 字节取 `rx[2..]`）
手工敲 ID 读，然后把 `tlmm-vm-gpio-list` 里**不是** SPI 四根也不是 IRQ 的 11 根
（44,45,46,47,29,177,128,129,131,77,78）逐根拉低 3 ms 再放高、等 120 ms、再敲一次。

```
pad g54 ctl0=00000001 up=0 dn=0
pad g26 ctl0=00000000 up=0 dn=0
pst head da 39 00 14 nzpages=4      ← ramoops(0xa7000000,4 MiB) 有内容
pst hits=0                          ← 但没有一行含 novatek/nvt_/ts_spi
bb id=000000000000 raw=0000000000000000
bb g44..g131 id=000000000000 miso=0 （11 根全一样）
```

- 手工敲也是全 0；
- 11 根候选引脚没有一根能把控制器叫醒（g77/g78/"bb all" 三行被 2401 字节的 dump 上限截掉）；
- ramoops 里没有 Android 触摸驱动的痕迹（console ring 只留 warning/error，probe 的 pr_info 不在里面）。

---

## 18. 2026-10-25 17:04 · stage 24：三个洞一次补上 —— 而且芯片其实是活的

`stage24` 同一只 boot 做三件事：

### 18.1 上拉建立时间（0.1 / 1 / 10 / 100 ms）

```
settle g16 0000     ← MISO  一直被拉低
settle g17 1111     ← MOSI  浮空
settle g18 1111     ← CLK   浮空
settle g19 1111     ← CS    浮空
settle g54 0000     ← IRQ   一直被拉低
```

**推翻 §15/§16 的"上拉读 0 = 被钳住"**：MOSI/CLK/CS 三根在 0.1 ms 就已经是 1（纯浮空，没有钳位），
而 MISO 和 IRQ 无论等多久都是 0 —— 说明**有东西在主动驱动这两根线**，也就是**控制器是上电的**，
它在驱动自己的 MISO/IRQ（空闲/reset 态下为低）。

### 18.2 引擎的 CS 命令到底有没有到焊盘

四根 mux 成 `0x184`，发 `SPI_CS_ASSERT`/`SPI_CS_DEASSERT`，一边读 gpio19 的 io bit0：

```
se cs rc=0/0 pad idle=1 lo=0 hi=1 geni=0
```

**焊盘跟着命令走**（idle 高 → assert 低 → deassert 高）。所以"主机侧完全没问题"这句这次是真的：
mux、引擎、CS 命令、引脚驱动全部实测通过。

### 18.3 四个焊盘的 24 种角色组合

```
perm 0321 id=000000d94c838
perm tried=24 hits=1
```

24 种里**只有 0321 一种回了非零**（MISO=16, MOSI=**19**, CLK=18, CS=**17**），
也就是 DT 里 `mosi`/`cs` 那两根对调了。这是这一整天第一次从控制器读到数据。

---

## 19. 2026-10-25 17:12 · stage 25：24 种组合各读三次 —— 0321 只稳定了一次

```
pg0  000000000000 ×5  000000d94c838      ← 第 6 个 = 0321
pg6  000000000000 ×6
pg12 000000000000 ×6
pg18 000000000000 ×6
pg best=5 bits=15
pf pol=0 id=000000000000 raw=0000000000000000
pf pol=1 id=000000000000 raw=0000000000000000
```

- 每种组合读 3 次取 OR：**只有 0321 的 3 次里有一次**回了 `00 00 0d 94 c8 38`，另两次全 0；
- 在 0321 上再跑**完整** `nvt_identify` 前导（engine reset `0x7fff80<-0x5a`、bootloader reset
  `0x1fb43e<-0x69`、page、读），两种 CS 极性都**全 0**；
- 所以那一次回答更像"帧刚好凑巧选中了控制器"，还不能当可靠通路用。
  下一只 boot 就是 stage 26：把 0321 的时钟相位（CPHA 0/1）、MISO 偏置（无/上拉）、
  半周期（10/40 µs）各试三遍，看哪一种能**每次都给同一个值**，同时用 DT 的 16/17/18/19 做对照。

---

## 20. 2026-10-25 17:17–17:22 · stage 26/27：MISO 的电平不是"被钳住"，而是被某个东西**开关**

### 20.1 stage 26：同一个角色组合，慢一点就全 0xFF

把 §18.3 那个唯一回数据的组合（MISO=16, MOSI=19, CLK=18, CS=17）拿来，改 CPHA / MISO 偏置 / 半周期，
每种读三次：

```
r26 c0 id=ffffffffffff/ffffffffffff/ffffffffffff
r26 c1 id=ffffffffffff/ffffffffffff/ffffffffffff
r26 c2 id=ffffffffffff/ffffffffffff/ffffffffffff
r26 c3 id=ffffffffffff/ffffffffffff/ffffffffffff
r26 c4 id=ffffffffffff/ffffffffffff/ffffffffffff   ← 半周期 40 µs
r26 dt id=000000000000/000000000000/000000000000   ← DT 的四根
r26 dt raw=0000000000000000
```

- "答得出来"的组合（MOSI=19/CS=17）在慢时钟下**稳定回 0xFF**；
- DT 的组合**稳定回 0x00**；
- 也就是说 gpio16 上看到的不是数据，是**某个驱动源的开/关**：0xFF = 高阻/被拉高，0x00 = 被拉低。

### 20.2 stage 27：把三根驱动线停在 8 种静态组合下读 MISO

```
s27 up=00000000 dn=00000000     ← MOSI/CLK/CS 的 8 种电平组合，MISO 上拉/下拉都读 0
s27 idle=0 rel=0000000000000    ← CS 拉低时，逐根放高 13 根候选线（含 54/26），MISO 一直 0
```

**静态下无论怎么摆这三根线，gpio16 都是 0**（上拉下都读 0 → 有东西在拉低）；而 stage 26 里
把 gpio17 当输出**拉高**、时钟在 gpio18 上跑起来之后，同一个脚就稳定读到 1。

### 20.3 现在能确定/不能确定的

能确定：

| 结论 | 依据 |
| --- | --- |
| 引脚控制器的写/回读模型正确 | §16 |
| 引擎的 mux/驱动/CS 命令真的到焊盘 | §18.2 |
| gpio16 有外部下拉（比内部上拉强、比焊盘驱动弱）| §16/§24 的 out1/out0 + 上拉 |
| 有源器件在驱动 gpio16/gpio54 | §18.1（10–100 ms 都拉低）|
| DT 的 mosi/cs 命名与"唯一回过数据"的组合不一致 | §18.3/§19 |

还不能确定（下一场按这个顺序）：

1. **gpio17 拉高为什么让 gpio16 变高**：可能 17 是控制器的 RESET（低有效）、17 是 CS、或者 16/17 之间有耦合。
   ⇒ 一次 boot：把 17 拉高/拉低各保持 50 ms，期间每 1 ms 采一次 gpio16，打印"跳变时刻"，
   就能区分"电平跟随"（复位/CS）和"只有时钟跑起来才变"（数据）。
2. **真正的 MISO 在哪根脚上**：用已知能驱动的 SE 路径（DT 四根）发 0xFF×32，同时把
   `tlmm-vm-gpio-list` 的 16 根脚都配成输入（下拉），每发一个字节采一遍，看哪根脚出现过 1。
3. **PMIC**：如果 1/2 都指向"控制器没被正确选中/复位"，就上 SPMI（见 §21）。

---

## 21. 2026-10-25 17:31 · stage 28：静态电平不控制 MISO，一帧之内 16 根候选脚也没有任何一根有数据

`stage28` 两件事：

### 21.1 gpio17 的电平能不能带动 gpio16（不跑时钟）

```
s28 g17 hi=000 lo=000
```

MISO 加了上拉（浮空应读 1），把 gpio17 驱动拉高后 1/5/20 ms 各采一次、再拉低同样采一次，
**gpio16 始终是 0**。所以 §20.3 里"17 → 16"不是电平跟随（不是复位、也不是 CS 的直接效果），
stage 26 里的 0xFF 只跟**时钟在跑**有关。

### 21.2 真正的数据线在哪根脚上

用 DT 的四根（CS=19, CLK=18, MOSI=17）敲一帧完整的 Novatek ID 帧，
同时把 `tlmm-vm-gpio-list` 的 16 根脚都配成"输入+下拉"，每一位采一遍、统计"1"的比例
（每格 = 144 次采样里 1 的个数 /16，`9` = 一直是 1，`0` = 一直是 0）：

```
            16 17 18 19 44 45 46 47 128 129 131 29 177 54 77 78
s28 p0      0  1  9  0  9  9  0  9   0   9   9  9   9  0  0  9
s28 p1      0  0  9  1  9  9  0  9   0   9   9  9   9  0  0  9
```

- 除了我们自己驱动的 17/18/19（p0 的 17=1 是 MOSI 图案、18=9 是时钟、19=0 是 CS 拉低；
  p1 反过来），**没有任何一根脚出现过"中间值"** —— 全是 0 或全是 1；
- 也就是说这一帧里**没有一根候选脚在传数据**：控制器的数据线要么不在这些脚上，
  要么它根本没有回答。

### 21.3 现在的处境

主机侧（mux / 引擎 / CS / 焊盘驱动）已经全部实测通过（§18.2），模拟 SPI（bit-bang）在
CPHA 0/1、无偏置/上拉、两种速率下也都试过（§20/§21），控制器**不回任何东西**：

| 已排除 | 依据 |
| --- | --- |
| 主机 mux/引擎/CS/驱动 | §18.2 |
| SPI 模式/速率/CS 极性（引擎路径）| 上一场 stage 20（12 组）|
| CPHA 0/1、MISO 偏置、66 kHz–20 kHz（手工）| §20.1/§21.1 |
| MISO 在别的 tlmm-list 脚上 | §21.2 |
| tlmm-list 里的 11 根非 SPI 脚是 reset/power enable | §17（stage 23）|

**剩下只有两条路**（都不在"再敲一次 SPI"这个维度上）：

1. **PMIC/SPMI**：DT 里触摸没有 regulator、也没有 reset-gpio，而控制器的 MISO 是"一直被拉低"，
   最可能是**被 PMIC 侧的门控或复位压住**。工具已经有了：
   `UniXNU-sheng/package/BootResetCore.h` 是**在 sheng 上验证过**的 SPMI arbiter v7 实现
   （observer 读 + channel 写 + APID 按 PPID 查找），cliffs 的 arbiter 地址与 SM8550 完全一致
   （本机 DT `spmi@c42d000`）：`core 0xc400000`、`chnls 0xc500000`、`obsrvr 0xc440000`、
   `cnfg 0xc42d000`、EE 0。搬过来先做**只读枚举**：列出所有 APID 的 `PERPH_TYPE`(0x04)，
   把 GPIO（type 0x10）和 regulator 的 `EN_CTL` 打出来 —— 第一步只读，不改任何东西。
2. **让 Android 先把触摸电起来再回 XNU**：风险是 Android 的 `fs_mgr` 可能把 userdata
   （现在装的是 iPad 根镜像）格式化掉；`UniXNU/out/muyu/userdata/userdata-root.img`
   和 `/tmp/userdata-root.sparse` 都还在，重刷约 4 分钟，但 `/tmp` 会被清掉，需要重新
   `tools/img2simg.py` 转一次（约 2 分钟）。

---

## 22. 2026-10-25 17:36–17:41 · stage 29/30：**XNU 里 SPMI 只读打通了**（新能力）

### 22.1 stage 29：arbiter 身份 + 已知外设验证

照搬 `UniXNU-sheng/package/BootResetCore.h`（在 sheng/SM8550 上验证过的 arbiter v7 代码），
cliffs 的地址与 SM8550 完全一致（本机 DT `spmi@c42d000`）：
`core 0xc400000`、`chnls 0xc500000`、`obsrvr 0xc440000`、`cnfg 0xc42d000`、EE 0。
observer 通道读 = `obsrvr + 0x8000*ee + 0x20*apid`，命令字 `(op<<27)|(byte_addr<<4)`。

```
s29 ver=70010000 feat=80060380 apids=896
s29 mapped=623/896
s29 sdam apid=481 rc=0 cell=00          ← SDAM(PPID 0x071) 读成功 = 通路验证
s29 type0=50000003000011111111111100000111
s29 type1=0000000000000000000000002222222222
s29 readok=32/32
```

- `ver=0x70010000` = **arbiter v7**，896 个 APID、623 个已映射；
- **SDAM 读成功**（sheng 用同一个外设写重启原因）⇒ SPMI 读通路可用；
- 前 64 个 APID 的 `PERPH_TYPE` 高半字节：APID 12–23 是 `0x1x`（= GPIO 类型 0x10），正好 12 个。

### 22.2 stage 30：全量只读普查

```
s30 ver=70010000 apids=896 mapped=623 ok=579 bad=44 gpio=40
s30 hist=104c85d200000000
s30 g0  0b6E0 06cE0 06dE0 06eE0 06fE0 089E0 0cdE0 0ceE0
s30 g8  0f9E0 0faE0 157E0 19eE0 1e0E1 ec1e0 1ccE0 1dcE0
s30 g16 1eeE0 1d3E0 1eeE1 ec1e0 1f6E0 1f7E1 09fE0 1f9E0
s30 g24 1feE0 1f0E0 1e0E1 200E1 201E0 202E0 203E0 204E0
s30 g32 20cE0 20dE0 20eE0 20fE0 210E0 211E0 212E0 213E0
```

（每项 = `apid(3 位 hex)` + `E`/`-`（输出使能）+ `0`/`1`（输出电平）；40 个 GPIO 外设，
历史类型直方图 `hist[0..15]=1,0,4,c,8,5,d,2,0,…` 说明 type 高半字节 0x3/0x6/0x4/0x5 都有，
也就是 PM8550/PMK8550/PM7550 这一组 PMIC 里的 regulator/GPIO 都能看见。）

**这一步是纯只读的**：写 PMIC GPIO 有把面板电掉、丢 dump 的风险，所以分两步走。

### 22.3 下一步（很明确）：只对"当前没使能"的 PMIC GPIO 做驱动扫描

1. 从 §22.2 的列表里挑出**电平/使能是 `-`（输入）或 `0`（输出低）**的那些（现在一个都没被驱动）；
2. 对每一根：`MODE_CTL(0x40)=0`（数字输出）→ `DIG_OUT_SRC(0x48)=0x80`（拉高）→ `EN_CTL(0x46)=0x80`
   → 等 50 ms → **bit-bang 读 Novatek ID**（stage 23/25 的代码现成），读到非零就立刻停；
3. 反过来再试一次"拉低 5 ms 再拉高"（复位脉冲）；
4. 每根之间恢复原值；只打印"命中"的那几根 + 一行统计。

这是目前唯一还没试过、而且直接指向"控制器被 PMIC 侧压住"的实验。

---

## 23. 2026-10-25 17:55–18:03 · stage 31：PMIC GPIO 也试了（只有 3 根可写），仍然没叫醒

### 23.1 两次 data abort（都是我自己的 bug，记下来省下一轮）

1. **SPMI 写通道窗口映射太小**：`chnls + 0x1000*apid`，apid 最大 896，需要的窗口是 **0x400000**
   （DT 里 `chnls` 就是 4 MiB），我一开始只映射了 0x10000 → apid 0x0b6 落在窗口外 → 翻译错误
   （`esr=0x96000047`，far 指向 `0x...f010`）。修法：`mapRegion(TM_SPMI_CHNLS, 0x400000, …)`。
2. **写不属于本 EE 的 APID = SPMI 总线外部中止**（`esr=0x96000010`，far = `chnls+0x1000*apid+0x10`）。
   arbiter 允许"任何 EE 读、只有 owner 能写"，所以写之前必须先读 `cnfg + 4*apid` 的 bits[2:0]
   和本 EE（0）比对，不等于就跳过。**这两条以后写 PMIC 都要先做。**

### 23.2 结果

```
s31 base=000000000000 raw=00000000
s31 cand=3 tried=3 hits=0 skipped=3
s31 r0 000
```

- 40 个 PMIC GPIO 里：大多数已经是"输出使能"（`E0`/`E1`），不动的那些里只有 **3 根**
  既是"未使能"、又属于本 EE（可以写）；另外 3 根未使能但属于别的 EE（写会总线中止，已跳过）；
- 这 3 根逐根试了"拉高 30 ms"和"拉低 5 ms 再放高 50 ms"（复位形状），**没有一根能让控制器回 ID**；
- 基线 `base=000000000000`（DT 四根、手工 bit-bang）照旧全 0。

### 23.3 于是"PMIC GPIO 是门控/复位"这条也基本排除，只剩两类动作

1. **动那些"已使能"的 PMIC GPIO（`E0`/`E1`，约 30 根）**：最可能是"复位被压住"的地方
   （`E0` = 输出低 = 典型的 reset 拉低形状），但**风险明确**：其中可能有背光/面板电，
   一旦拉高把面板弄黑，这一只 boot 的 dump 就全丢了（panic 文字是在探针之后才画的）。
   要做的话建议一次只试一根，或者先把结果写到 UFS 固定 LBA 再动手；
2. **PMIC 的 regulator（LDO/SMPS）使能位**：stage 30 的 `hist` 里 type 0x3x/0x4x/0x5x/0x6x 那些
   就是 PM8550/PMK8550/PM7550 的 regulator 外设；每个 regulator 的 `EN_CTL` 在 0x46（bit7 使能）。
   同样有"把不该开的电压打开"的风险，但比 GPIO 小（多是没有负载的轨）。
3. **兜底**：让 Android 把触摸电起来再回 XNU（userdata 有被格式化的风险，镜像还在本机，
   重刷约 4 分钟 + 重新 sparse 约 2 分钟）。

### 23.4 现在这张"证据网"已经覆盖了

| 维度 | 状态 |
| --- | --- |
| TLMM mux/驱动/回读 | ✅ 正确（§16/§18.2）|
| GENI 初始化/时钟/DMA/CS | ✅ 正确（§14/§18.2）|
| SPI mode/速率/CS 极性/CPHA | ✅ 全扫（stage 20、§20.1）|
| 数据线是否在别的焊盘上 | ✅ 不是（§21.2）|
| tlmm-list 的 11 根是 reset/enable | ✅ 不是（§17）|
| 未使能的 PMIC GPIO 是 reset/enable | ✅ 不是（§23.2）|
| 已使能的 PMIC GPIO / regulator | ⬜ **没试**（有屏幕风险）|
| Android 先电起来 | ⬜ 没试（有 userdata 风险）|

### 23.5 一个必须先知道的"别碰"清单

`fw/dtbo.img` 反编译后，PMIC GPIO 只被用在两处，其中一处就是**面板电**：

```dts
display_gpio_regulator@0 {
    gpio = <&pm8550ve_gpios 0x08 0x00>;     /* PM8550VE GPIO8 = 面板 AVDD 使能 */
    enable-active-high; qcom,proxy-consumer-enable;
};
```

所以 stage 31 之后如果要去动"已使能"的 PMIC GPIO，**PM8550VE 的 GPIO8 必须跳过**
（其余 `pm7550ba` 的 GPIO 都是别的用途）。这也解释了为什么 §23.3 里那两次 abort 反而"安全"——
它们发生在写之前，屏幕没受影响。

---

## 24. 2026-10-25 18:10–18:25 · 走 Android 这条路：**从运行中的内核拿到了真 DT**，并且发现 recovery 里 adb 是通的

### 24.1 意外收获：stock recovery 上 adb 免授权可用

刷回原厂 `vendor_boot` + `fastboot boot fw/stock/boot.img` 之后落进了**原厂 recovery**，
而 recovery 的 adbd **不需要在屏幕上点授权**：

```
a20248f7  recovery  product:muyu  model:24091RPADC  device:muyu
uid=2000(shell) ... context=u:r:shell:s0      # 不是 root，但 shell 可用
```

可用的东西：`dmesg`、`/proc/device-tree`、`/sys/class/regulator/*`、`/sys/class/input/*`、
`getprop`；**不可用**：`adb root`（production build）、`/dev/block/*`（root only）、
`/sys/kernel/debug`（没挂）。这是这个项目第一次能"实时"读这台机器，以后调试都该先用它。

### 24.2 真 DT（merged，运行期）：触摸**没有 reset-gpio、没有 supply**，goodix 是死节点

见 `logs/2026-10-25-touch/device-truth/touch-nodes.txt`。关键三条：

1. `novatek@0`：`compatible="novatek,NVT-ts-spi"`、`status="ok"`、`irq-gpio=<&tlmm 54 0x2001>`、
   `lcd_id-gpio=<&tlmm 26 0>`、`pinctrl-0=<cs(gpio19)>`、19.2 MHz —— **没有 `*-supply`、没有 `reset-gpio`**；
2. `goodix-berlin@0`：只有 `name` 和 `panel`，**没有 compatible / reg** ⇒ 死节点，
   **所以本机触摸就是 Novatek**（"可能是 Goodix"这条彻底排除）；
3. 也就是说 **Android 自己的驱动也不控制触摸的供电和复位** ⇒ 这两样是硬件常开/硬件拉死的，
   于是"XNU 启动时控制器没电/被复位"这个解释**不成立**，问题更可能在协议/时序或芯片型号上。

### 24.3 顺手拿到的 regulator 基线

`logs/2026-10-25-touch/device-truth/regulators-recovery.txt`（recovery 状态下 name=state 全量）。
注意 `display_panel_avdd=disabled` 但 `display_panel_vsn/vsp=enabled`、`pmxr2230_l12/l17=enabled` 等——
以后要判断"哪条轨是触摸的"，得拿**正常 Android 启动**（触摸驱动跑过）的同一份表来 diff。

### 24.4 为什么 Android 这条路现在走不通（两条独立原因）

1. `fastboot boot <原厂 boot.img>` 与 `fastboot reboot` 都落到 **recovery**：
   `getprop ro.boot.force_normal_boot` = `unknown`、`init.svc.recovery` = `running`、
   原厂 boot.img 的 cmdline 是**空的**（header v4），`force_normal_boot` 由 ABL/bootconfig 给，
   而 ABL 在 fastboot 模式下不给 ⇒ init 走 recovery；
2. 即使进正常启动，Android 也会因为 **userdata 里是 iPad 根镜像**（recovery 屏幕提示
   "couldn't mount file system"）而回落到 recovery，`/vendor`（recovery 版）里也没有触摸模块
   （只有 6 个音频 dlkm），所以**触摸驱动根本没机会运行**。

### 24.5 若要真正执行 B（"让 Android 先把触摸电起来"）—— 唯一可行的做法

1. `fastboot flash userdata` 一个**小体积空白 ext4/f2fs**（几 MB 即可，Android 会自己 fs）；
2. 正常启动 Android（此时 `force_normal_boot` 仍要解决：需要改 `vendor_boot` 的 bootconfig
   或 ABL 参数），让 vendor_dlkm 里的触摸模块加载、probe；
3. **直接在 Android UI 上试触摸**——这是最硬的判据（能用 = 硬件没问题，问题 100% 在我们这一侧；
   不能用 = 硬件/型号问题）；
4. 若 adb 能被授权（`ro.adb.secure` 与 /data 的新 key），还能 `adb shell dmesg | grep -i novatek`
   把驱动的打印直接读出来；
5. 收工后 `fastboot flash userdata fw/userdata-backup/userdata-root.sparse` 把 iPad 根镜像写回去
   （备份已存，约 4 分钟；**注意 `/tmp/userdata-root.sparse` 会被系统清掉，用备份那份**）。

> 另外：本场执行过 `fastboot erase misc`（清 BCB）。这是安全操作（Android 会重建），
> 但记一笔以免以后排查启动问题时困惑。

---

## 25. 2026-10-25 18:25 · 执行 B 的过程记录（到一半，设备掉电）

目标：让 Android 正常启动一次，让它 vendor_dlkm 里的触摸模块 probe，然后在 Android UI 上试触摸。

### 25.1 已经做的

1. `fastboot -w`：`Writing 'userdata' OKAY (0.025 s)` + `Erasing 'metadata' OKAY`，
   但跟了一句 `File system type raw not supported.` ⇒ **userdata 只被擦了，没有被格式化**；
2. **`androidboot.force_normal_boot=1` 写进原厂 boot.img 的 cmdline**（header v4，cmdline 在偏移 44，
   原本是空的）⇒ `fw/stock/boot-normal.img`。第一次用它 `fastboot boot` 后，45 s 时 adb **完全不出现**
   （不是 `recovery` 状态）⇒ 说明这一次真的进了正常启动，而不是 recovery；
3. 但 105 s 时又变成 `a20248f7 recovery` ⇒ **正常启动中途回落到 recovery**，最可能是 `/data` 挂不上
   （我们只擦了没格式化，且 Android 的 fs_mgr 在 recovery 里没跑 format）；
4. 顺手 `fastboot set_active a` 把 A/B 槽元数据重写过（之前 `erase misc` 把 misc 里的
   `bootloader_control` 一起清零了，这也是"正常重启也进 recovery"的一个可疑原因）；
5. 原厂 vendor_boot 的 **bootconfig_size = 0**（`strings` 里也只有 cmdline 里的 `bootconfig` 关键字），
   即 `force_normal_boot` 只能靠 ABL 或 boot.img 的 cmdline 给。

### 25.2 下一步（设备已掉电，需要先充电/开机）

1. 进原厂 recovery（`fastboot flash vendor_boot fw/stock/vendor_boot.img` + `fastboot boot fw/stock/boot.img`）；
2. **在 recovery 菜单里选 "Wipe data/factory reset"**（按键操作）——这才是**真正格式化** `/data`，
   我们之前 `fastboot -w` 没有格式化；
3. `adb reboot bootloader`（recovery 里 adb 免授权）；
4. `fastboot boot fw/stock/boot-normal.img`（带 `androidboot.force_normal_boot=1`）⇒ Android 正常启动；
5. **在 Android UI 上试触摸**（最硬判据）；
6. 若要回到 XNU：`adb reboot bootloader` → `fastboot flash userdata fw/userdata-backup/userdata-root.sparse`
   → `fastboot flash vendor_boot fw/vendor_boot_patched.img` → `fastboot boot UniXNU/out/muyu/boot/boot.img`
   （stage 32 就是为这一刻准备的五行快速探针）。

> 注意：设备现在是**完全没电/掉线**状态，USB 上已经看不到；先充电再继续。

---

## 26. 2026-10-25 18:45 · **触摸应答了！** `id=0e 00 04 32 65 03`（NT36532E 签名）

```
[175.630446] MUYU TOUCH TLMM pins set 00000184, now ctl 00000184 00000184 00000184 00000184
[175.644307] MUYU TOUCH s33 init rc=0 fifo_dis=1
[175.680178] MUYU TOUCH s33 m0 400k rc=0 id=0e0004326503
```

- `rc=0` = `nvt_identify` **成功**；`id[3..5] = 32 65 03` 正是它要求的签名，
  `id[0]=0x0e` ⇒ 走的是 **`NT36532E`** 那张 mmap（`matches_e`）；
- 这一只 boot 里**四根脚先被 mux 成 `0x184`**（`setSpiPins`），然后 mode 0 / 400 kHz 一次就读到了；
  `if (r == 0) return;` 立刻退出，所以只印了一行。

### 26.1 这一枪为什么之前没中（两个 bug 叠加）

1. **引脚没 mux**：除 stage 16 外，所有走 GENI 的测试都没调用 `setSpiPins()`，
   四根脚还停在上电态（`ctl=0x1`，GPIO 输入）⇒ **信号根本没上焊盘**。
   stage 20 那个"12 组 mode/速率全 0"的结论因此**作废**。
2. **`row()` 的 24 行上限**：满了以后连 `IOLog` 都不打（8-25 的 stage 33 首跑因此丢了 9 行）。
   已改成"超限只跳过数组、永远打日志"，数组扩到 48 行。

### 26.2 还没分开的变量（下一步必须做）

这一只 boot 里**同时**满足了两件事：① 引脚 mux；② 芯片刚被 Android 的驱动带过（`adb reboot bootloader`
是热重启，没断电，`/proc/interrupts` 里 `msmgpio 54 xiaomi_tp` 当时在正常上报）。
所以还要回答：**冷启动（彻底断电）后还能不能读到？**

- 能 ⇒ 之前的失败纯粹是 `setSpiPins` 的 bug，触摸从此可用；
- 不能 ⇒ 芯片需要 Android 驱动那套"上电/复位/固件下载"里的某一步（Android 的驱动模块
  `nt36532_touch.ko` 在 `/vendor_dlkm/lib/modules/`，但 SELinux 禁止 shell 读取，拿不到源码），
  那就得继续找那个复位脚/唤醒序列。

### 26.3 顺手确认的 Android 侧事实（`logs/2026-10-25-touch/device-truth/`）

- `/proc/interrupts`：`msmgpio 54 Edge xiaomi_tp`，计数 1826/1830/2191 ⇒ 芯片在上报触摸；
- 输入设备：`NVTCapacitiveTouchScreen`、`NVTCapacitivePenP81c`、`NVTCapacitivePenM80p`；
- 驱动模块：`/vendor/lib/modules/nt36532_touch.ko` + `xiaomi_touch.ko`（**型号 NT36532**）；
- `touch_dev` sysfs 存在（`/sys/class/touch/touch_dev/`），但文件对 shell 不可读、`dmesg` 被
  `dmesg_restrict` 挡住、`adb pull` 被 SELinux 拒（`vendor_file`, permissive=0）。

### 26.4 冷启动也成功 ⇒ 根因就是"引脚没 mux"

彻底断电（长按电源 10–15 s）后再进 fastboot、再跑 stage 33：

```
[169.142016] MUYU TOUCH TLMM pins set 00000184, now ctl 00000184 00000184 00000184 00000184
[169.163442] MUYU TOUCH s33 init rc=0 fifo_dis=1
[169.198776] MUYU TOUCH s33 m0 400k rc=0 id=0e0004326503
```

**和热重启那次完全一样**（`rc=0`、`id=0e 00 04 32 65 03`）。所以：

- 芯片不需要 Android 先初始化，**上电即活、ROM 就能应答**；
- 之前那一整天的"控制器不应答"= **我们自己的 `setSpiPins` 没被调用**（走 GENI 的 stage 10/11/12/13/14/19/20
  全都没 mux 引脚），加上 `row()` 的 24 行截断把关键行吞掉；
- 主机链路（GCC 投票 + RCG + GENI init/clock/setup + SE DMA + apps-SMMU + CS 命令 + 引脚 mux）
  **全部正确**，`nvt_identify` 一次通过。

### 26.5 下一步（真正的移植工作，不是调试了）

1. `NativeNovatekTouch` 已经在仓库里（`drivers/NativeNovatekTouch/NvtProtocol.c` + `NvtTouchCore.c`），
   sheng 的 `package/TouchProbe.cpp` 第 4 级有完整的 bring-up + `publish()` 范例；
2. 要补的正是这次验证过的那几步：`setSpiPins` → GCC 投票 → RCG → GENI init/clock/setup →
   SE DMA + apps-SMMU → `nvt_identify`（**已验证**）→ 然后 `nvt_bus` 读帧；
3. 中断：gpio54（`msmgpio 54 xiaomi_tp`），TLMM 共享 GIC SPI 208 → `armcpu_spi_attach(240, …)`；
   也可以先轮询 `SE_GENI_RX_FIFOn`；
4. 数据出口：`NativeHidInput` 的 digitizer 源（sheng 的 `publish()` 就是范例）。

### 26.6 下一层的关键未知：固件

`sheng` 的 `TouchProbe.cpp` 第 4 级里，`nvt_identify` 之后是
**`nvt_download_firmware()` → `nvt_start_scanning()`** 才开始读触摸帧。
也就是说这颗 NT36532 **可能要靠主机下载固件**（ROM 只应答 ID）。

- 我们在 muyu 上验证到的只是 **ROM/引导态应答 `0x1FB104`**；
- 要读帧，得先确认：① 这颗芯片是不是自带 flash（`nvt_start_scanning` 直接成功）；
  ② 如果要下载，固件 `.bin` 在哪（Xiaomi 的 `nt36532_touch.ko` 里可能内嵌，
  或 `/vendor/firmware/` 下；但 SELinux 禁止 shell 读 vendor_dlkm，
  `/vendor/firmware_mnt/image` 目前列出来是空的，需要另找途径——比如从原厂 ROM 的
  `vendor_dlkm.img` 里解出来）。
- 所以 stage 34 的第一步应该是：`nvt_start_scanning` 能不能过；过不了就找固件。

## 27. 2026-10-25 19:05 · stage 34：**芯片只有 ROM，固件必须由主机下载**

屏幕原图 `logs/2026-10-25-19-05-35-323.jpg`，转正/放大件
`logs/2026-10-25-touch/stage34-rot.png`、`stage34-rows.png`。原始读数：

```
MUYU TOUCH TLMM pins set 00000184, now ctl 00000184 00000184 00000184 00000184
MUYU TOUCH s34 init rc=0 clk=0 setup=0
MUYU TOUCH s34 pre rc=0 irq=0 hd100=0 hd0ff=0
MUYU TOUCH s34 pre rs=163ef9003e fw=5bb7 hc=02 hd=240097e7
MUYU TOUCH s34 id rc=0 id=0e0004326503 casc=1
MUYU TOUCH s34 post rc=0 irq=0 hd100=0 c=59287 t=154
MUYU TOUCH s34 post rs=163ef9003e fw=5bb7 hd=240097e732ef4ef6
MUYU TOUCH s34 fwinfo rc=-8 v=00 ty=00 x=0 y=0 xm=0 ym=0 pid=0000 def=1
MUYU TOUCH s34 scan cmd19=-110 doze=0 cmd01=-110
MUYU TOUCH s34 poll rc=0 n=100 valid=0 fresh=0 c=4294967295 t=255
```

### 27.1 读法

| 行 | 含义 |
| --- | --- |
| `pre` | 在**我们写任何寄存器之前**（POR 后约 165 s），事件缓冲里没有合法 THP 头（`hd100=0`），IRQ 脚 gpio54 高（`irq=0` 就是"未拉低"）⇒ **芯片自己没有在跑固件** |
| `pre rs=16 3e f9 00 3e` | reset-complete 事件块不是 `0xa0..0xaf`：ROM 态下这块本来就没意义，但也说明"引导态就绪"这个握手**还没发生** |
| `pre/post` 完全相同 | `nvt_identify` 里那次 bootloader reset（`0x1FB43E` ← 0x69）**没有改变**事件缓冲的任何字节 ⇒ 之前芯片也已经在 ROM 态 |
| `casc=1` | `0x1FB12C` bit0 读到 0 ⇒ 选了 `map_e_cascade`（event_buf **0x11C400**）。注意 ROM 态下这个读回也可能是"读不到就是 0"，所以两张 map 都要留一手（两张 map 只差 event_buf） |
| `fwinfo rc=-8` | `NVT_ERR_FW`：`event+0x78` 的 `[0]+[1] != 0xff` ⇒ **没有固件填过 fw-info** |
| `cmd19=-110` / `cmd01=-110` | `NVT_ERR_TIMEOUT`：host command 写下去以后首字节**没有被清掉** ⇒ 没有固件在消费命令（`doze=0` 只是普通写成功，不说明任何事） |
| `poll ... valid=0 fresh=0` | 1 秒内 100 次 THP 头窥视，**没有一个合法头、计数器没有动** |

### 27.2 结论

**这颗 NT36532E 没有自带可运行的固件（或至少出厂 flash 里没有可跑的 MP 固件）。**
所以 sheng 的流程在这里同样成立：`nvt_identify` → **`nvt_download_firmware()`** →
`nvt_start_scanning()` → 读 THP 帧。stage 34 的 `nvt_start_scanning` 分步复现（get_fw_info →
cmd 0x19 → doze → cmd 0x01）全部在第一步就失败，和这个结论一致。

主机侧到这里**没有剩下的未知**：总线、引脚、CS、时钟、DMA 全部验证过；缺的只是**固件镜像**。

### 27.3 固件从哪来（按可行性排序）

1. **原厂 fastboot ROM 的 `vendor_dlkm.img`**（最干净）：解出 `nt36532_touch.ko`，固件大概率以
   `static const unsigned char nvt_mp_fw[]` 形式**内嵌在模块里**（Novatek Android 驱动一贯如此）；
   用 `UniXNU-muyu/scripts/gen-touch-firmware.py <bin>` 生成 `build/native-touch/NvtFirmware.c` 即可
   被 `nvt_download_firmware()` 使用。风险：要下 7–8 GB 的整包。
2. **在 XNU 里从设备自身读**：`vendor_dlkm` 是 `super` 里的**动态分区**（GPT 的 32 项里没有它），
   所以要么解析 `super` 的 liblp 元数据，要么用 `NativeUfs` 已有的 `hostRead4k()` 加一个
   `native_ufs_read_absolute()` 导出，扫 raw 扇区找固件签名。内核里直接拿到字节就能立刻下载，
   **不需要把数据传回 Mac**。工作量最大，但不依赖网络。
3. 别的机器/公开仓库里的 NT36532 固件：只当最后手段（面板 mapping 可能不匹配）。

### 27.4 下载固件时的两个开关（写代码时别再猜）

- `cascade` 决定 event_buf：`0x11C400`（cascade）vs `0x125800`（single）。
  `fw_crc_enable()` 与 `check_reset_state()` 都在这块里，**下载握手必须用对**；
  ROM 态下 `casc` 的读回不可信 ⇒ 第一次下载时两个基址都试，谁能让
  `reset_state[0]` 落进 `0xa0..0xaf` 就用谁。
- `fw->cascade_2nd_header` 来自固件头自身（`d[0x20]` bit1），它决定要不要走
  `tx_auto_copy_en=0x56` + `wait_auto_copy` 那一段，和上面的 map 选择是**两件事**。

## 28. 2026-10-25 19:47 · stage 35：**固件下载成功，触摸在 XNU 里出帧了**

```
MUYU TOUCH s35 spi ok pins=00000184
MUYU TOUCH s35 id rc=0 id=0e0004326503 casc=1 ev=11c400
MUYU TOUCH s35 image 245760 B rc=0 parts=16 casc2=1
MUYU TOUCH s35 ilm 00200@000000+16cb3 dlm 22000@100000+14177
MUYU TOUCH s35 dl rc=0 stage=0 try=1 wait=00 polls=2
MUYU TOUCH s35 dl rs=a0040032 0e
MUYU TOUCH s35 fwi rc=0 v=0e ty=01 x=60 y=40 xm=30480 ym=20320 pid=5981
MUYU TOUCH s35 scan c19=0 doze=0 c01=0
MUYU TOUCH s35 poll rc=0 n=200 valid=200 fresh=2 c=87 t=2
MUYU TOUCH s35 frame v=1 t=2 c=87 mn=9966 mx=10042 nz=2400
MUYU TOUCH s35 irq=0
```

（原图 `logs/2026-10-25-19-47-13-293.jpg`，转正件 `logs/2026-10-25-touch/stage35-crop.png`。）

| 行 | 读法 |
| --- | --- |
| `dl rc=0 stage=0` | **`nvt_download_firmware()` 一次成功**：3 字节 bootloader reset → ILM/DLM CRC 描述符 → 写 16 个分区（175 KB）→ CRC enable → boot_rdy → `check_reset_state` 第二轮就拿到 `0xa0` |
| `rs=a0 04 00 32 0e` | reset-complete 事件 `[0]=0xa0` = `RESET_STATE_INIT`；后四字节 `04 00 32 0e` 正是 id 的镜像 ⇒ **握手用对了 event_buf（0x11C400，cascade map）**，"RELOAD_FW" 路径不需要翻转 map |
| `fwi rc=0 v=0e ty=01 x=60 y=40 xm=30480 ym=20320 pid=5981` | 固件自己在跑；**60×40 节点、3048×2032 面板** —— 和 sheng 的 12.4" 3K 面板几何完全一致，所以那份公开固件能直接boot 起来 |
| `scan c19=0 doze=0 c01=0` | idle baseline update / doze delay / doze mode 三步全过 ⇒ 固件在消费 host command |
| `poll 200/200 valid fresh=2 c=87 t=2` | 每 10 ms 窥视一次，**200 次全部是合法 THP 头**，计数器在动，type=2 |
| `frame v=1 mn=9966 mx=10042 nz=2400` | 完整 5 KB 帧读回并通过头校验；2400 个节点全非零，原始互容值在 9966..10042（约 76 个码的跨度）⇒ 扫描是真的 |
| `irq=0` | gpio54 被芯片拉低（有帧待读）——中断可以后面再做，轮询已经够用 |

### 28.1 结论：**主机侧全部打通，触摸已经是"读帧"问题，不是"能不能通"的问题**

- 这颗 NT36532E 是**纯 ROM + 主机下载**（§27），下载只在 400 kHz 下跑了几秒；
- 面板几何 60×40 / 3048×2032 与 sheng 相同，`NvtTouchCore` 的坐标表（`kRowMap`/`kColumnMap`、
  `NVT_LONG_AXIS_MAX=30480`、`NVT_SHORT_AXIS_MAX=20320`）**直接可用**；
- 下一步只剩两件事：① `nvt_touch_process()` → 触点（stage 36）；② 触点 → `NativeHidInput`
  digitizer（sheng `package/TouchProbe.cpp` 的 `publish()` 是范例，坐标约定 X 直接、Y 用 `20319-y`）。

### 28.2 固件来源（重要发现，省掉 8.7 GB 的整包下载）

原厂 ROM `muyu-ota_full-OS3.0.307.0.WOYCNXM`（和设备上的 `ro.build.version.incremental` 完全一致）
的 `payload.bin` **未压缩**，所以可以用 HTTP Range 只取需要的分区。工具：
`tools/extract-rom-partition.py`（zip64 + payload protobuf + xz，纯标准库）。

从 `vendor_dlkm`（38,232,064 B，erofs）里取出 **`nt36532_touch.ko`**
（sha256 `f98360cb31b98969…`），`strings` 得到的固件文件名是：

```
novatek_nt36532_o81_fw_csot.bin     novatek_nt36532_o81_mp_csot.bin
novatek_nt36532_o81_fw_tm.bin       novatek_nt36532_o81_mp_tm.bin
novatek_ts_fw.bin                   novatek_ts_mp.bin          (fallback)
```

- 驱动用的是 **`request_firmware()`**（`nm -u` 里有 `request_firmware`/`release_firmware`），
  **不是内嵌数组** ⇒ 固件在 `/vendor/firmware/`（`vendor` 分区，1.4 GB）；
- `o81` 对应本机的 O81 面板（`Device-Binaries/muyu/RawFiles/Panel_O81_*`），
  `csot`/`tm` 是面板厂（CSOT / TIANMA），由 `novatek,lcd_id-gpio`（gpio26）决定用哪一份；
- **`/vendor/firmware` 是 `dr-xr-x--- system system`**：recovery 里 SELinux 虽然是 Permissive，
  但这是 DAC 权限，uid 2000 读不到；`/dev/block/*` 也是 `brw------- root root`。
  所以设备上没有免 root 的读法（TOUCH3 §6.1 的"SELinux 挡住"其实是 D 权限）；
- 目前 stage 35 用的是**公开的 sheng 固件**（`map220v/nt36532e_win`，
  sha256 `358d7115…`，见 `.cache/firmware/novatek/PROVENANCE.md`）。它能跑，但它属于
  n81a 面板；**最终仍应换成 `novatek_nt36532_o81_fw_{csot,tm}.bin`**。
  取法有两条：① 从 `vendor.img`（1.4 GB）里按 erofs 解析出那一个文件（bigota 只有 ~20 KB/s，
  整包要 19 小时，所以得只取需要的块）；② 在 XNU 里用 NativeUfs 读 `super`，按固件头特征
  扫出镜像（不需要把数据传回 Mac，内核里拿到就能直接下载）。

## 29. 2026-10-25 20:00–20:22 · stage 36–39：读帧、提速、**面板确实在感测**

### 29.1 stage 36（20:02）：只读到一帧，然后 abort

`frames=1 scored=0 touch=0 dmax=0`，紧接着 `Kernel instruction fetch abort at pc == fp`。

**根因（我们自己的 bug）**：`touchUp()` 里 `struct nvt_bus bus` 是**局部变量**，`chip.bus = &bus`；
`touchUp()` 返回后 stage 36 的循环还在用 `chip.bus->xfer`，于是调用的是别人栈帧里的字节
（abort 时 `pc == fp`，典型返回地址被覆盖）。修法：`bus` 与 `CsBus` 改成 `static`。
stage 35 当时只是**侥幸**（返回后栈还没被覆盖）。

### 29.2 stage 37（20:08）：19.2 MHz 可用，帧流正常

```
s37 clk 19200 ok=4
s37 poll rc=0 n=20000 v=19933 bad=67 chg=2101 c=17263 4..17263
s37 f0 c=17266 t=4 mn=9959 mx=10075 n=-1
...
s37 f4 c=17270 t=4 mn=9960 mx=10078 n=-1
s37 read n=5 hdrbad=0 dmax=0 touch=0 x=0 y=0
```

- **19.2 MHz 有效**（4/4 窥视合法）⇒ 5 KB 整帧从 400 kHz 的 ~103 ms 降到 ~2.5 ms；
- 连续 5 帧计数器 17266→17270，**头校验全过**，矩阵每帧都有小幅变化 ⇒ 帧流是真的；
- `n=-1` 是因为 `nvt_touch_process()` 在 `frame_type != 2` 时**先收集 72 帧做中值参考**，
  5 帧当然不够；顺带确认：**这颗芯片只发 type 4**（`t2=0 t4=3000`），不发 sheng 那边的 type 2。

### 29.3 stage 38（20:16）：按住的手指**不明显**，但 dmax 在慢爬

```
s38 clock 19200
s38 t1 dmax=75/68/72/85/64/82/100/109/119/143/138/143   big=0
s38 frames=3000 scored=2998 touch=0 pk=114106
s38 dmax=143 big=0 c=45769 t=4
```

- **3000 帧 / ~11 s ≈ 270 帧每秒**（比 sheng 那边的 ~110 帧还快，因为 19.2 MHz）；
- 手指从第 ~9 秒按住到结束，`dmax` 从 ~70 单调爬到 143，**没有任何一处超过 300**（`big=0`）；
- 两个可能的解释：① 静态按住产生的差分本来就小（或手指位置在边缘）；② 参考帧/对齐有问题。

### 29.4 stage 39（20:22）：**滑动的手指 = dmax ~500、10 个节点过 300** ⇒ 面板在感测

同一只 boot 里让用户**左右来回滑**，并且改成测**相邻两帧**的差（不依赖参考帧）：

```
s39 scan c19=0 doze=0(v=2) c01=0
s39 t1 dmax=497 big=8  ty=4
s39 t2 dmax=513 big=8  ty=4
s39 t3 dmax=524 big=12 ty=4
s39 t4 dmax=531 big=10 ty=4
s39 t5 dmax=515 big=8  ty=4
s39 t6 dmax=536 big=10 ty=4
s39 t7 dmax=499 big=8  ty=4
s39 t8 dmax=490 big=8  ty=4
s39 t9 dmax=511 big=12 ty=4
s39 t10 dmax=526 big=10 ty=4
s39 t11 dmax=536 big=12 ty=4
s39 frames=3000 dmax=536 t2=0 t4=3000 first=4 last=4
s39 irq=0
```

- **每 250 帧就有 8–12 个节点的帧间差 > 300**，`dmax` 稳定在 ~500 ⇒ 触摸信号真实、清晰；
- 帧类型仍然**全是 type 4**（`t2=0`）⇒ `NvtTouchCore` 里"等 type 2 建参考帧"的分支在这颗芯片上
  **永远不会触发**，参考帧必须用 type 4 自己建（中值 72 帧已经是这么做的）；
- `doze` 变体这轮用的是 sheng 的原值（`muyu-touch-doze=2`），默认没变。

### 29.5 结论与下一步（触点 → HID）

主机链路现在**没有任何未知**：下载、扫描、19.2 MHz 读帧、矩阵、帧间差分全部实测通过。
剩下的只是**软件**：

1. **触点的参考帧策略**：type 4 帧自己建中值参考（`nvt_touch_process` 已经这样做），
   但 stage 38 说明"按住不动"的差分只有 ~143 —— 先确认这是不是手指压在边缘/参考被污染，
   再决定要不要把 `NVT_NOISE/PEAK/DOWN` 阈值按这份固件重标（现在 100/220/300）；
2. **发布到 HID**：`NativeHidInput` 的消费端已经就绪（`native_touch_seq` seqlock +
   `native_touch_count/xs/ys/ids[12]` + `native_touch_set_sink()`），
   开机参数 **`sheng-touch-hid=1`**；坐标 X 直用、**Y 镜像 `20319 - y`**；
3. **中断**：gpio54 → GIC SPI 208 → `armcpu_spi_attach(32 + 208, …)`（轮询已够跑）；
4. **换 o81 固件**（§28.2 的两条路）。

## 30. 2026-10-25 20:26–21:08 · stage 41：**触摸驱动线程跑起来了**（60 s 内 1240 个触点、1335 个 HID 报告），但 UI 还不吃

### 30.1 做法

`package/TouchProbeMuyu.cpp` 新增 `touchDriverThread()` + `native_touch_muyu_start()`，
由 `native_legacy_display_late_probe()` 在开机时调用（`muyu-touch-drv=1`）：
延迟 45 s → 自己把 GCC 投票 + SE4 RCG 打开 → `setSpiPins` → SE DMA + apps-SMMU →
`nvt_identify` → 下载固件 → `nvt_get_fw_info` → 三条扫描命令 → 19.2 MHz → 无限循环：
32 字节窥视、计数器变了才读整帧、`nvt_touch_process()`、`native_touch_publish()`。
`muyu-touch=`（dump 时探针）此时**必须关掉**，否则两边会抢同一颗芯片。

### 30.2 驱动线程自己踩的两个坑（都已修）

| 现象 | 根因 |
| --- | --- |
| `drv spi prepare failed`（第一次） | 线程没做 **GCC 投票 + SE4 RCG**（探针是 `writeStages()` 做的）→ 新增 `clockOn()` |
| `spi: geni init/clock/setup rc=-22`（第二次） | 线程没给 `geni_spi` 接 **寄存器访问器**（`se.rd/wr`、`wrapper.rd`），`geni_spi_init()` 第一次读就空指针 → `spiPrepare()` 里补齐 |

### 30.3 上机结果（2026-10-25 21:05 截图，`logs/2026-10-25-touch/dump6-rows.png`）

```
[ 95.68] MUYU TOUCH drv scan c19=0 doze=0 c01=0
[ 95.68] MUYU TOUCH drv scanning at 19200 kHz
[113.77] MUYU TOUCH drv touch n=1 x=25304 y=13368 area=18 f=14
...
[125.98] MUYU TOUCH drv f=2000 s=1998 c=541  pub=541  h=587  k=1 n=1
[134.30] MUYU TOUCH drv f=3000 s=2998 c=980  pub=980  h=1062 k=1 n=1
[142.64] MUYU TOUCH drv f=4000 s=3998 c=1240 pub=1240 h=1335 k=1 n=0
[159.31] MUYU TOUCH drv tick3 pk=42518 v=42496 r=6000 e=0 f=6000
```

- **帧泵完全正常**：6000 帧 / ~64 s（~94 帧每秒，含 1 ms 睡眠），`s=5998` 帧被触点处理，
  `e=0` 没有总线错误；`tick` 行的 `pk/v/r` 说明窥视和整帧读取都在推进；
- **触点出来了**：`c=1240` 个触点、`area` 8–22、坐标 600…25304 / 676…15918 —— 跨度是整块面板；
- **HID 消费端已注册**（`k=1`），**1335 个 HID 报告真的发出去了**（`h=1335`）；
- **但 iPadOS 界面没有反应**（用户实测：滑锁屏无反应）。

### 30.4 所以现在唯一的问题是"HID 报告 → UI"

`h` 只说明 `IOHIDDevice::handleReport()` 被调用且返回成功，**不说明事件被派发**。
按可能性排序（下一次直接从第 1 条查）：

1. **digitizer 集合没被 IOHIDEventDriver 当作触摸屏消费**。`sheng-touch-split=1` 已经让
   `native_hid_touch_allocate()` 建了一个"只有 digitizer 描述符"的独立服务
   （product `Offline touch digitizer`，`Built-In`/`DisplayIntegrated`/`DigitizerCollectionDispatch` 都设了），
   这一只就是它——**仍未生效**；
2. **描述符/报告 ID 与 iPadOS 期望不符**：`touchSend()` 用 `report[0]=3` 作 Report ID，
   每指 6 字节 `{tip|valid, id, xL, xH, yL, yH}`，坐标是 0…32767 的绝对量；
3. **backboardd 需要把这个 digitizer 服务与 framebuffer 关联**（`DisplayIntegrated` 只是属性，
   可能还要 `kIOHIDDisplayIntegratedKey` 指向当前 IOFramebuffer 服务，或 `HIDDisplayIntegrated`）；
4. 也可能只是**我们的触点太"抖"**（area 8–22 的瞬时触点），被 UIKit 当噪声丢掉了 ——
   但正常滑动应该有连续轨迹，先查 1–3。

### 30.5 现场可用的开关（不用再编内核）

| 开机参数 | 作用 |
| --- | --- |
| `muyu-touch-drv=1` | 启动帧泵线程（当前带宽下必须开） |
| `muyu-touch-delay=<s>` | 线程等待秒数，默认 45 |
| `sheng-touch-hid=1` | NativeHidInput 装 digitizer 集合 |
| `sheng-touch-split=1` | 另建一个只有 digitizer 的 HID 服务（当前开） |
| `sheng-touch-poll=1` | （sheng 老参数）强制轮询 |

## 31. 2026-10-25 21:10–21:50 · HID 那一小段：`disp=0` —— 报告全部"成功"，事件驱动一次都不派发

### 31.1 这一轮加的观测（都在 `drv` 行里）

| 字段 | 含义 | 实测 |
| --- | --- | --- |
| `f/c/pub` | 整帧 / 触点 / 发布 | 6000 帧、2027 触点、2027 发布 |
| `h` | `IOHIDDevice::handleReport()` 成功次数 | 2050 |
| `k` | NativeHidInput 注册了 sink | 1 |
| `dev` | 键盘服务 / digitizer 服务 是否 started | 2/2 |
| `run` | 最长连续有触点的帧数 / 起始次数 | 383 / 23（**触点流很连续**）|
| `snd` | touchSend：发出 / 无事件驱动 / 无新序号 / 撕裂 | 2050 / 0 / 0 / 0 |
| `edge` | edge 机制：钩子装了 / 经过的数字器事件 | 1 / 0 |
| `disp` | **被钩住的 `dispatchEvent` 进入次数 / 其中 digitizer** | **0 / 0** |
| `who` | sink 归谁 / 从键盘发的次数 / 从 digitizer 发的次数 | **2 / 0 / 2050** |

### 31.2 读法

- `who=2/0/2050`：**sink 确实在 split digitizer 上**，2050 次报告全部由它发出（不是键盘设备在替它发）；
- `snd=2050/0/0/0`：`touchSend()` 每次都能找到"自己的事件驱动子节点"并成功 `handleReport()`；
- `disp=0/0`：**被我们替换掉的那个 `dispatchEvent`（slot 198）一次都没被调用**。

结论：**`IOHIDDevice::handleReport()` 成功 ≠ 事件驱动解析并派发**。要么事件驱动没有真正解析这些报告，
要么我们钩的不是这条路径所在的那个 vtable。注意 `offline-hid-auto-home` 那条"屏幕会自己亮"的现象
**不能**当作键盘 HID 注入生效的证据 —— 本板还有 `offline-key-events=1`（`NativeKeyStoreEvents`）
这条独立且已验证的按键注入路径。

### 31.3 下次的两个决定性实验（都很便宜）

1. **换成 sheng 的配置**：sheng 的 `board.toml` 里只有 `sheng-touch-hid=1`（**没有** split），
   而且他们的注释表明在 sheng 上 digitizer 事件确实到过 UIKit（edge 手势相关记录）。
   所以下一次先去掉 `sheng-touch-split=1`，看 `disp` 是否非 0；
2. **确认事件驱动的挂载与 open**：`hasEventDriverCached()` 只证明"找到了一个 IOHIDEventDriver 子节点"，
   不证明它 `open()` 成功、更不证明它的报告解析器认这份描述符。
   下次在 `touchSend()` 里对第一次调用记一份 `eventDriverFound` 的时机、以及
   `IOHIDDevice::open()` 的返回值；或者打印 IORegistry 里我们设备下面的子节点名字。
3. 若 `disp` 仍然 0，就换一条思路：**照 Apple MultitouchHID 的描述符补 In-Range/Certification 等 usage**，
   或直接研究 `NativeKeyStoreEvents` 那条已验证的注入路径是否能承载 digitizer 事件。

## 32. 2026-10-25 21:51 · **触摸进 iPadOS 了**（关键：不要 split digitizer）

```
# 能用的配置（sheng 的配置）
"sheng-touch-hid=1"          # digitizer 集合挂在键盘设备上（合并描述符）
# "sheng-touch-split=1"      # 不要开：独立的 digitizer 服务永远不派发（disp=0/0）

# 现象（用户实测）：锁屏上滑 → 弹出 “enter passcode” 和键盘 —— 手势被 SpringBoard 认了
```

### 32.1 为什么 split 不行

| 配置 | `who=sink/键盘/digitizer` | `disp=进入/其中digitizer` | UI |
| --- | --- | --- | --- |
| `sheng-touch-split=1` | 2 / 0 / 2050 | **0 / 0** | 无反应 |
| 不 split（默认） | 1 / n / 0 | 非 0（见下一场的读数） | **锁屏有反应** |

- split 设备：`IOHIDDevice::handleReport()` 2050 次全部成功，但被钩住的 `dispatchEvent` **一次都没进入**；
- 合并设备：同一份描述符、同一份 6 字节/指的报告，digitizer 集合被 `IOHIDEventDriver` 正常解析并派发。
- 结论：**digitizer 必须跟键盘在同一个 HID 设备里**（也就是 `NativeHidInput` 的默认合并路径），
  单独的"只有 digitizer"服务在这套 IOHIDFamily 上拿不到事件派发。

### 32.2 现在的状态

- ✅ 面板 → 帧 → 触点 → HID → UIKit 全链路通了（锁屏手势有反应）；
- ✅ 坐标约定沿用 sheng：X 直接、**Y 镜像 `20319 - y`**，滑动手势落点与手指一致；
- ⏳ 还要验的：多指、边缘手势（Home/切换器需要 `kIOHIDDigitizerEventFromEdgeTip`，见 `edge` 机制）、
  点击精度、以及把 dump 拍下来的方法（现在屏被 SpringBoard/密码键盘盖住了，因为触摸生效后
  SpringBoard 一直在重绘）。

### 32.3 复现命令（当前可用的这一只）

```bash
cd /Users/diannao/pad7pro-xnu
fastboot flash vendor_boot fw/vendor_boot_patched.img
fastboot boot UniXNU/out/muyu/boot/boot.img
# 约 45–95 s 后触摸上线（drv 行在 panic dump 里），之后一直有效
```

## 33. 2026-10-26 12:48–13:07 · 中断尝试失败（已回退轮询）+ HID 链路的确认读数

### 33.1 中断：装了引脚中断但一次也不触发

按 sheng 的做法给 gpio54 装了 TLMM 引脚中断（INTR_CFG: target=3/AP、**双沿**、raw status、
**INTR_STATUS 写 0 应答**，然后 `armcpu_spi_attach(32 + 208 (=240), false, touchPinIrq, …)`）：

| 版本 | 结果 |
| --- | --- |
| 强制中断（`assert_wait` 无超时） | **触摸直接死掉**：线程永远等一个不来的中断 |
| 中断可选 + 1 s 无中断自动退回轮询 + GIC 回读 | tick 行 `irq=1/0` ⇒ **armed=1、interrupts=0**（一次都没触发）；而且更糟：**面板的帧计数器停住了**（`tick49 pk=9600 … r=1 f=1`、`tick50 pk=10000 … r=1 f=1`）——装中断的那几个写操作之后，芯片只出过 1 帧 |

于是中断改成 **opt-in**（`muyu-touch-irq=1`，默认关），并保留“1 秒没中断就退回轮询”的兜底；
`board.toml` 里已注释掉。**现阶段用轮询**（≈1000 次/秒的 32 字节窥视，CPU 占用只有 1% 量级）。
GIC 回读（`rb=`/`rt=`）留在代码里：下次要查“是不是安全世界占着 SPI 240”时直接用。

### 33.2 顺带确认：HID 链路是通的（12:56 那只 dump）

```
[155.09] drv f=5000 c=1157 pub=1157 h=1217 k=1 dev=2/2
[155.09] drv run=57/60 edge=1/60/19 snd=1217/0 disp=1217/1217
[155.09] drv pk=26390 bh=16 e=0 who=1/1217/0 m=1/60
```

- `disp=1217/1217`：被包住的 `dispatchEvent` 进了 **1217 次，且 1217 次全是 digitizer 事件**
  ⇒ 报告确实被 `IOHIDEventDriver` 解析并派发了（这也是 10-25 那个 `disp=0` 问题的反面对照）；
- `who=1/1217/0`：sink 在**键盘设备**上（合并描述符），符合“不要 split”的结论；
- `edge=1/60/19`：60 个触点里 19 个起始于面板边缘，**边缘标记（`kIOHIDDigitizerEventFromEdgeTip`）注入在工作**；
- `run=57/60`：最长 57 帧连续有触点（≈0.2 s），流是连续的；
- `m=1/60`：目前实测都是单指（还没测过双指）。

## 34. 2026-10-26 13:17 · 触摸变成正式 IOService：`NativeNovatekTouch`

### 34.1 以前是什么样

到 13:07 为止，能用的触摸是**一个内核线程**：`package/TouchProbeMuyu.cpp` 里的 `touchDriverThread()`，
由 `native_legacy_display_late_probe()` 通过 `native_touch_muyu_start()` 拉起（`muyu-touch-drv=1`）。
它没有对象：映射、DMA 缓冲、NVT 状态、线程全是探针文件的全局量，`stop()` 不存在，
bring-up 也只能从探针里调。

### 34.2 现在是什么样

新增 `drivers/NativeNovatekTouch/NativeNovatekTouch.{h,cpp}`：真正的 `IOService`

```cpp
class NativeNovatekTouch : public IOService {
    bool start(IOService *provider) override;   // 映射 → 等 45 s → bring-up → 起泵线程 → registerService()
    void stop(IOService *provider) override;    // 让泵线程退出
    void free() override;                       // 释放映射/DMA/缓冲
    static NativeNovatekTouch *create();        // new + init + attach(getPlatform()) + start()
};
extern "C" IOService *native_novatek_touch_start(void);   // 入口，仍由 muyu-touch-drv=1 开
```

- bring-up 顺序与探针**逐字相同**（GCC 投票 + SE4 RCG → 四个 SPI 焊盘 `0x184` → apps-SMMU bypass +
  SE DMA → GENI init/400 kHz → `nvt_identify` → 固件下载（失败换 event-buffer map 重试）→ `nvt_start_scanning`
  → 19.2 MHz），只是搬进了类方法：`clockOn()` / `setPads()` / `armDma()` / `openBus()` / `bringUp()` / `pump()`；
- 发布仍是 `native_touch_publish()`（`NativeHidInput` 的 seqlock），配置仍是 **`sheng-touch-hid=1`、不要 split**；
- 日志前缀 `NATIVE-NVT`（`muyu-dump-keep=NATIVE-NVT,MUYU`），每 3000 帧（~30 s）打一行
  `frames=… contacts=… published=… errors=… hid=… sink=…`；
- 构建：`scripts/build-native-legacy-display.py` 把它和探针编进同一个 archive
  （都给 `-I drivers/NativeGeniSpi -I kits/muyu`，因为它俩不在同一层目录）；
- `package/LegacyDisplayProbe.cpp` 的挂钩从 `native_touch_muyu_start()` 换成
  `native_novatek_touch_start()`（weak import）。探针里那个旧线程函数留着（默认不再被调用），
  方便对照/回退。

### 34.3 为什么不是 DT 匹配自动启动

`ArmDTPlatform` 只给 **arm-io 的直接子节点**建 nub，而触摸节点在
`soc/qupv3_0_geni_se/spi@a90000/novatek@0` 里三层深，所以拿不到 provider 去匹配。
驱动因此沿用“display late probe 里创建并 start”的方式（和 `NativeHidInput` 一样），
但对象自身是完整 IOService：`attach(IOService::getPlatform())` + `registerService()`。

### 34.4 实测（13:17:35 那只）

用户实测：**触摸与之前完全一致**（上滑、点击正常）。kernel sha256 `632d8aba…`。

### 34.5 还没做的（下一场）

1. 把 `pump()` 的轮询换成中断（`armcpu_spi_attach` 那条，见 §33 为什么失败）；
2. 电源管理（`stop()` 时给面板发 doze、`start()` 复位）与 `IOService` 的 power state 挂钩；
3. 原厂 o81 固件（§28.2）；
4. 探针 `TouchProbeMuyu.cpp` 里的旧线程函数与 1–40 号诊断 stage 可以整体移出内核镜像，
   只在需要时按 `muyu-touch=<n>` 打开。
