# HWSCHED / GX 上电取数操作单（Android 侧）

目的：拿到厂商驱动在真机上**给 GX 上电那一刻**到底写了什么。离线反汇编已经挖完
（见 `HANDOFF-2026-10-31.md` §18），剩下的答案只在 Android 运行时里。

> ⚠️ **代价**：启动 Android 会破坏 `userdata` 上的 iPadOS 根。恢复需要重刷
> `fw/userdata-backup/userdata-root.sparse`（约 3.5 分钟）。开始前确认该文件存在且
> sha256 与 `HANDOFF-2026-10-29.md` §0 记录一致。

---

## 0. 前置检查

```bash
cd ~/pad7pro-xnu
shasum -a 256 fw/userdata-backup/userdata-root.sparse     # 4c90d7a7…（唯一副本）
./tools/muyu-flash-mode.sh status                          # 设备在 fastboot、slot a
./tools/muyu-flash-mode.sh ledger | tail -3                # 记下当前台账
```

## 1. 回到 Android

```bash
./tools/muyu-flash-mode.sh stock          # 恢复原厂 vendor_boot（只充电/安卓通路）
# 设备应自行启动 HyperOS；若停在 fastboot：
fastboot reboot
```

等 Android 起来（约 1–2 分钟），确认：

```bash
adb devices
adb shell getprop ro.build.version.incremental      # OS3.0.307.0.WOYCNXM
```

## 2. 拿 root

Magisk 已装在 `init_boot`，`su` 不在 PATH，必须用绝对路径：

```bash
adb shell /debug_ramdisk/su -c id      # uid=0 context=u:r:magisk:s0
```

后面所有命令都用 `SU='/debug_ramdisk/su -c'` 包装。

## 3. 采集（按顺序，全部只读）

```bash
OUT=logs/android-harvest-$(date +%Y%m%d-%H%M)
mkdir -p "$OUT"

# 3.1 内核日志（kgsl/gmu/hfi 的启动痕迹）
adb shell dmesg > "$OUT/dmesg.txt"

# 3.2 kgsl debugfs 节点清单
adb shell /debug_ramdisk/su -c 'ls -l /sys/kernel/debug/kgsl/kgsl-3d0/' > "$OUT/kgsl-nodes.txt" 2>&1

# 3.3 GPU 状态转储（含寄存器；这是最关键的一份）
adb shell /debug_ramdisk/su -c 'cat /sys/kernel/debug/kgsl/kgsl-3d0/gpu' > "$OUT/kgsl-gpu-state.txt" 2>&1

# 3.4 时钟树（已在仓库里有一份，重新取一份确认）
adb shell /debug_ramdisk/su -c 'cat /sys/kernel/debug/clk/clk_summary' > "$OUT/clk_summary.txt" 2>&1

# 3.5 电源域与 regulator 状态
adb shell /debug_ramdisk/su -c 'cat /sys/kernel/debug/pm_genpd/pm_genpd_summary' > "$OUT/pm_genpd.txt" 2>&1
adb shell /debug_ramdisk/su -c 'cat /sys/kernel/debug/regulator/regulator_summary' > "$OUT/regulator_summary.txt" 2>&1

# 3.6 中断（看 GMU/GX 的中断是否在动）
adb shell /debug_ramdisk/su -c 'cat /proc/interrupts' > "$OUT/interrupts.txt" 2>&1

# 3.7 kgsl 的可调参数（确认是否有 debug 开关）
adb shell /debug_ramdisk/su -c 'ls /sys/module/kgsl/parameters/ && cat /sys/module/kgsl/parameters/*' > "$OUT/kgsl-params.txt" 2>&1
```

## 4. 制造一次 GPU 工作（让 GX 真正上电）

打开任意 3D 界面即可，例如设置里的动画、相机预览或游戏：

```bash
adb shell am start -a android.intent.action.VIEW -d https://example.com   # 触发合成
sleep 5
```

在**工作期间**再抓一次状态与中断：

```bash
adb shell /debug_ramdisk/su -c 'cat /sys/kernel/debug/kgsl/kgsl-3d0/gpu' > "$OUT/kgsl-gpu-state-busy.txt" 2>&1
adb shell /debug_ramdisk/su -c 'cat /proc/interrupts' > "$OUT/interrupts-busy.txt" 2>&1
adb shell dmesg | tail -200 > "$OUT/dmesg-busy.txt"
```

## 5. 关键：抓 GX/GMU 寄存器快照

kgsl 的状态转储通常包含 `gmu` 段（HFI 队列、GX 电源状态）。若 3.3 的输出里有
`gmu` / `gx` 字样，重点保存。再补一份：

```bash
adb shell /debug_ramdisk/su -c 'cat /sys/kernel/debug/kgsl/kgsl-3d0/devfreq/cur_freq' > "$OUT/kgsl-cur-freq.txt" 2>&1
adb shell /debug_ramdisk/su -c 'cat /sys/class/kgsl/kgsl-3d0/gpu_model' > "$OUT/kgsl-model.txt" 2>&1
adb shell /debug_ramdisk/su -c 'cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage' > "$OUT/kgsl-busy.txt" 2>&1
```

## 6. 取回并归档

```bash
adb pull /sys/kernel/debug/kgsl/kgsl-3d0 "$OUT/kgsl-3d0-debugfs" || true
ls -la "$OUT"
shasum -a 256 "$OUT"/* > "$OUT/SHA256SUMS"
```

把整个 `$OUT` 目录保留在 `logs/` 下（不要删），下次开工直接读。

## 7. 恢复 iPadOS 根

```bash
# 回 fastboot
adb reboot bootloader
./tools/muyu-flash-mode.sh status
# 重刷根镜像（3.5 分钟）
fastboot flash userdata fw/userdata-backup/userdata-root.sparse
# 刷回能进 iOS 的 vendor_boot
./tools/muyu-flash-mode.sh xnu fw/vendor_boot_btn2.img
```

刷新后开机会走 iOS 的首次启动流程，属正常。

---

## 判读要点（拿到数据后看什么）

| 目标 | 在哪些文件里找 |
| --- | --- |
| GX 上电时写了哪些 GMU 寄存器 | `dmesg-busy.txt`、`kgsl-gpu-state-busy.txt` |
| HFI 队列基址与命令 | `kgsl-gpu-state.txt` 里的 `gmu`/`hfi` 段 |
| GX 电源域/时钟的真实名字与状态 | `pm_genpd.txt`、`clk_summary.txt` |
| 是否存在我们没见过的 GX 时钟 | `clk_summary.txt` 里 `gpu_cc_gx*` / `gx_*` |
| GMU 中断是否活跃 | `interrupts.txt` 与 `interrupts-busy.txt` 的差 |

拿到这些之后，才能确定「GX 时钟到底由谁打开」，再回到内核侧实现。
