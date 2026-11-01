# AdrenoFirmware 补丁（本板的 gen7 启动修正）

## 为什么是补丁而不是 submodule 提交

`kits/AdrenoFirmware` 的远端是官方的 `UniXNU/AdrenoFirmware`，本工作区没有它的推送权限，
所以 `UniXNU` 仓库里那个 submodule 指针（`b888963`）**别人克隆时取不到**。

按 AGENTS §2.3「改动落在补丁/组件层」，本板的改动以补丁形式留在这里：

```
0001-muyu-bring-up.patch    基于官方 20ad3d7，改 4 个文件、+90 行
```

应用方式（在 submodule 里）：

```bash
cd UniXNU/kits/AdrenoFirmware
git checkout 20ad3d7
git am /path/to/UniXNU-muyu/patches/adreno-firmware/0001-muyu-bring-up.patch
```

---

## 补丁内容

### 1. `AdrenoHfiSendOneWay()`（`AdrenoHfi.c/.h`）

新增一个不跟踪应答的发送路径。gen7 固件对某些配置消息（HFI `0x0d` set-value）**不按自己的
序列号回 ACK**，用 `AdrenoHfiSend()` 等它就会把启动序列卡死（实测 `proto=8`
`AdrenoHfiStartupTransportError`）。

### 2. `perf_table_v2` 性能表（`AdrenoHfiStartup.c/.h`）

gen7 固件 ≥ `0x20060001` 只认 **msg `0x0f`** 的性能表，忽略旧的 `0x04`。两个格式：

| | 旧（< 0x20060001）| 新（≥ 0x20060001）|
| --- | --- | --- |
| 消息号 | `0x04` | **`0x0f`** |
| 头部 | `[0]=count, [1]=gmu_count` | **`[0]=0, [1]=1, [2]=count, [3]=3`** |
| 电平 | 从 `p[2]` 起，3 字/档 | **从 `p[4]` 起，3 字/档** |
| GMU 档 | `p[50+2i]` | `p[3*count+6+2i]` |
| 总字数 | 固定 58 | `3*count + 2*gmu + 6` |

**载荷是用 kprobe 从真机抓下来的**（A735，12 档 GPU + 3 档 GMU = 49 字），不是推测：

```
w0=0x310f (0x0f | 49<<8)  w1=0  w2=1  w3=12  w4=3
w5..w40  = 12 档 {vote, 0xffffffff, freq_kHz}
w41=3  w42=2  w43..w48 = 3 档 {vote, freq_kHz}
```

### 3. SET_VALUE 与 FEATURE_CTRL 阶段（`AdrenoHfiStartup.c/.h`）

`perf_table_v2` 为真时，在 CoreStart 之前插入厂商的 3 条 set-value 与 8 条 feature-control：

```
set-value:    {111,0,3}  {112,44,0}  {117,1,160015}
feature-ctrl: {1,0} {2,13} {9,5760} {11,1} {12,0} {16,0} {17,0} {27,0}
```

数值同样来自 kprobe 实测。

---

## 效果

真机验证：加了这些之后，启动序列从 `proto=6`（超时）变为 **`proto=0`**（完整通过），
`fw=00000100`（init done）、`hfi=1`（ready）都正常。

**但 GX 仍未上电**——见 `HANDOFF-2026-10-31.md` §25：GX 由固件在收到频率命令后自行上电，
不是靠 OOB 或投票。补丁只解决了「启动序列能被固件接受」，没有解决「GX 何时上电」。

---

## 上游提 PR 的建议

这个补丁的前两项（one-way 发送、v2 性能表）对任何 gen7 设备都有用，值得提 PR。
第三项（set-value / feature-ctrl 的具体数值）是 A735 的板级数据，建议做成可配置项。
