# muyu：USB CDC 调试串口移植方案

> 目标：让 XNU 在真机上通过 USB CDC ACM 暴露一个控制台，能从 Mac 读到内核日志（含早期 kprintf）。
> 这是 §13 得出的结论之后的**唯一**可靠日志通道：面板在 UEFI→内核交接后不再扫描输出，屏幕取日志不可行。

参考实现（已 clone 到本仓库旁边，只读参考，不要直接改）：

```
/Users/diannao/pad7pro-xnu/ios-vermeer-lab/
  USB-DEBUG.md                       # 行为说明、开机参数、主机端用法
  xnu/vermeer/vermeer_usb_debug.c    # 456 行，DWC3 + CDC ACM，自包含
  xnu/vermeer/vermeer_debug.c        # RAM 日志环（append/copy）
  xnu/vermeer/vermeer_usb_protocol.h # 设备/配置描述符、DMA 约束
  xnu/vermeer/0024-vermeer-usb-debug-hook.patch
```

同样的驱动也被 SM8650 的 OnePlus Ace5 移植采用（`/Users/diannao/pad7pro-xnu/UniXNU-ace5`，
见 `docs/USB_TERMINAL.md`，VID:PID `1209:0001`，开机参数 `vermeer_usb=1`）。
所以“同一份驱动跨 SoC 适配”是这条路线已验证过的做法。

## 关键前提（决定了移植量）

驱动开头写着：

> `Reuses ABL's PHY/clocks; never changes GCC, SMMU, PMIC or storage.`

也就是说**它不初始化 PHY、不配时钟、不碰 SMMU**，只做三件事：

1. 校验 GCC 的 USB 电源域/时钟和 SMMU 的 USB 身份路由（不满足就**拒绝**，不动控制器）；
2. 软复位 DWC3、重建事件环和 EP0/EP1 描述符、打开 RUN；
3. 用 RAM 日志环喂 CDC IN，解析 CDC OUT 的命令。

`fastboot boot` 之后 ABL 已经把 USB 留在 device 模式，这正是它能工作的原因。
muyu 上的可行性要在第一次真机测试时确认（见“验证步骤”）。

## 集成方式（4 个补丁点）

参考 `0024-vermeer-usb-debug-hook.patch`：

| 文件 | 改动 |
| --- | --- |
| `bsd/conf/files` | 加 `muyu_debug.c`、`muyu_usb_debug.c`（`standard`） |
| `bsd/kern/bsd_init.c` | 在准备根卷之后调 `muyu_usb_debug_start()` |
| `osfmk/console/serial_console.c` `_cnputs()` | 把 console 文本追加进 RAM 环 |
| `pexpert/arm/pe_kprintf.c` `serial_putc_crlf()` | 早期 kprintf 逐字符追加（PE 阶段也有日志） |

在 muyu 里这四处都放进 `UniXNU-muyu/patches/xnu/` 的一个新 patch（文件名排在最前，
例如 `zzzzzzzzzzz-muyu-usb-debug.patch`，因为其它 zzz 补丁会改同一批文件）。
新增源文件放在 `UniXNU-muyu/package/` 或按现有的 `drivers/<Name>/` 约定放，
并用 `CONFIG_XNU_<NAME>` 开关（参考 `build-arm-driver.py` 的 archive 机制）。

## 硬件参数对照（SM8635 = cliffs / muyu）

| 项 | vermeer (SM8550) | muyu (SM8635) | 依据 |
| --- | --- | --- | --- |
| DWC3 core | `0xa600000` | `0xa600000` | DT `soc/ssusb@a600000/dwc3@a600000` reg |
| DWC3 大小 | `0xe000` 映射 | `0xd93c` | 同上 |
| QSCRATCH | `0xa6f8800` | 待确认（同偏移先试） | `qcom,dwc-usb3-msm` 惯例 |
| GCC base | `0x139000` | **`0x100000`** | DT `soc/clock-controller@100000` = `qcom,cliffs-gcc` |
| GCC USB 电源域/时钟寄存器偏移 | `0x4/0x18/0x88/0x8c` | **需按 cliffs 重新取值** | Linux `drivers/clk/qcom/gcc-cliffs.c`、`gcc-pineapple.c` |
| APPS SMMU | `0x15000000` | `0x15000000` | DT `soc/apps-smmu@15000000` |
| SMMU 身份寄存器 | `0x20/0x24/0x0/0x28` 的期望值 | **必须在真机读一次再填** | 驱动 `identity()` |
| USB 的 SID/stream | 105 个 stream 里唯一匹配 | 从 DT `dwc3.iommus` 取；数量待确认 | DT、驱动 `identity()` |
| DMA 约束 | `0x8c0000000..0x940000000`，`>=36` 位 | **按 muyu 的 12 GB @ `0x80000000` 重算** | DT memory、`vusb_dma_allowed()` |
| 设备字符串 | `K70DBG004` | 改成 `MUYUDBG001` | `vermeer_usb_protocol.h` |
| 开机参数 | `vermeer_usb=1` | `muyu_usb=1`，加进 `board.toml` 的 `kernel_args` |  |

> GCC 和 SMMU 的期望值不能靠猜：驱动的设计就是“值不符 → 拒绝并打印”，
> 第一次上机即使拒绝也是**有用的信息**（能用 `boot_progress`/panic screen 之外的
> 方式看到吗？——看不到，所以第一版要尽量把“读到的值”也通过 USB 发出去，
> 或者退一步：先只做 DWC3，不做守卫，确认能枚举）。

## 建议的推进顺序（每一步都可以单独上机验证）

1. **最小枚举实验**：只做 DWC3 软复位 + EP 配置 + RUN，不接任何守卫、
   不接日志环，设备字符串固定。目标：Mac 的 `ioreg`/`system_profiler` 里出现
   `1209:0001`。出现了就说明“ABL 留下的 PHY/时钟在 muyu 上同样可用”，
   后面才有意义；没出现再查 PHY/时钟。
2. **接日志环**：把 `_cnputs()`/`serial_putc_crlf()` 的追加接上，
   主机端 `UniXNU/scripts/debug_terminal.py` 的 `replay` 能拿到缓冲日志。
3. **补守卫**：读一次 GCC/SMMU 的真实值，填进 `identity()` 和 DMA 约束，
   恢复“不满足就拒绝”的安全属性。
4. **接命令协议**：`ping/status/help` 起步，再按需要加。

主机端现成工具（`UniXNU/scripts/`）：

- `debug_terminal.py`：命令/日志/采集（`--port`，自动找 VID:PID）
- `debug_screen.py`：GSC1 帧协议解码 + 截屏（要内核端实现帧传输，晚一点再说）

## 验证步骤（Mac 侧）

```bash
# 1) 开机参数带 muyu_usb=1，payload/内核照常
fastboot flash vendor_boot fw/vendor_boot_patched.img
fastboot boot UniXNU/out/muyu/boot/boot.img

# 2) 看 CDC 有没有枚举（大约 20–40 秒后）
ioreg -p IOUSB -w0 -l | grep -i "MUYUDBG\|1209"
ls /dev/cu.usbmodem*

# 3) 连上去看日志
/opt/homebrew/bin/python3.12 UniXNU/scripts/debug_terminal.py --port /dev/cu.usbmodemXXXX
#   然后 status / replay
```

## 风险

- **枚举失败无法自救**：一旦 DWC3 配错，设备可能从 USB 上消失，只能强制重启进 fastboot。
  所以第一版保持最小、可回滚，并且**不要改 PHY/GCC 的写操作**（只读校验）。
- 驱动自己说明：配置后若发生断线/总线复位，控制器会停机，通常要重启手机才能重连。
- USB 与 `fastboot` 共用同一个控制器：`fastboot boot` 之后 ABL 状态是否保留，
  是这次移植的第一个未知数。
