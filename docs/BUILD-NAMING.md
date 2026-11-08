# 内核构建命名规范（2026-11-08 起）

`uNNN` 连续编号退役。本文是下一个内核构建之后所有内核产物的命名依据。

## 1. 为什么换

- **它不携带信息。** `u151` 到 `u159` 之间改了什么、为什么改，只能翻台账。
- **它被写进了 printf。** 同一个镜像里既有 `(u151)` 也有 `(u155)`（逐条硬编码）。2026-11-08
  复盘 u152–u159 时，就是靠这个版本串判断「源码没变」，结论是错的，见
  `HANDOFF-2026-11-08.md` §3.5。
- **它和目的解耦。** 屏幕、存储、GPU 各条线的轮次混在一个数列里，看不出这一轮在攻什么。

## 2. 格式

```
<line>-<purpose>-<MMDD><letter>
```

| 段 | 取值 | 说明 |
| --- | --- | --- |
| `line` | `27` / `17` | OS 线：27 = iPadOS 27.0.1 (24A446)，17 = iPadOS 17.7.11 (21H461) |
| `purpose` | 见下表 | 这一轮要回答的问题类别 |
| `MMDD` | 4 位 | 构建日（月日），同一目的同一天用字母递增 |
| `letter` | `a`…`z`，再 `aa`… | 同一天同一目的的第几次尝试 |

| purpose | 含义 |
| --- | --- |
| `scr` | 屏幕 / 观测链路（控制台页、翻页、panic 回放） |
| `cfg` | 配置、defconfig、链接集合 |
| `sto` | 存储、Data、cryptex、keybag |
| `gpu` | GPU、GMU、Adreno |
| `inp` | 触摸、按键、HID |
| `usr` | 用户态、launchd、dyld、共享缓存 |
| `pwr` | 电源、热控、背光 |
| `net` | 网络 |

第一个按本规范命名的内核：**`27-scr-1108a`**（27 线，屏幕回植，11-08 第 a 次）。
同一天第二次同目的构建是 `27-scr-1108b`；第二天改配置是 `27-cfg-1109a`。

## 3. 产物命名

| 产物 | 文件名 |
| --- | --- |
| vendor_boot | `vendor_boot_27-scr-1108a.img` |
| boot | `boot_27-scr-1108a.img` |
| 内核镜像 | `kernel_27-scr-1108a.macho` |
| 面板照片 | `logs/2026-11-08-18-05-12-331.jpg`（保持相机原名，台账里登记 tag） |

`scripts/build-tag.py names` 直接打印这三个名字。

**历史不动**：`fw/vendor_boot_27-u1xx.img` 那批保持原名，它们是既成事实；
台账里凡是 `u` 编号，都是 2026-11-08 之前的轮次。

## 4. 标签如何进入内核（单一来源）

启动参数不可用：`kernel_args` 已经在 979 B / ~1000 B 预算上，加 `sheng-build-tag=`
会挤掉别的参数。所以走编译期：

```
BUILD-TAG                     ← 一行，唯一来源
  └─ scripts/build-tag.py
       ├─ drivers/MuyuDebug/build_tag_generated.h   #define MUYU_BUILD_TAG "27-scr-1108a"
       └─ <tree>/.cache/xnu-work/bsd/dev/arm/build_tag*.h（--tree 时安装）
            └─ muyu_usb_debug.c: #include "build_tag.h"
                 printf("MUYU USB" MUYU_TAG ": ...")   → MUYU USB(27-scr-1108a): ...
```

`build_tag.h` 的兜底顺序：生成头 → `-DMUYU_BUILD_TAG` → `"untagged"`。
出现 `untagged` 说明构建没拿标签，`build-tag.py check` 会直接失败。

## 5. 命令

```sh
scripts/build-tag.py next --purpose scr --note "screen port, first tagged build" \
        --tree /tmp/spike27-out/muyu/ipados-27.0.1-24A446/muyu-27/tree
scripts/build-tag.py show          # 当前标签
scripts/build-tag.py names         # 三个产物名
scripts/build-tag.py cflag         # -DMUYU_BUILD_TAG='"27-scr-1108a"'
scripts/build-tag.py check <lib.a|kernel.macho>   # 扫 "untagged"
```

`next` 会把标签写进 `BUILD-TAGS.tsv`（时间 / 标签 / 目的 / 备注），这份表就是发布台账的
前半段；刷机后在同一条后面补 sha 与结果。

## 6. 规则

1. **标签不复用。** 一个标签对应一次构建，改一个字符也要新标签。
2. **源码里不再写死轮次。** 输出路径一律 `MUYU_TAG` / `MUYU_BUILD_TAG`；注释里的
   `u151:` 是历史引用，保留可检索，但不得进入任何 printf/kprintf。
3. **刷机前跑 `check`。** 拿到 `untagged` 或 `u1xx` 的镜像不许上机——那意味着标签没生效。
4. **台账以标签为主键。** 每行：`tag / 时间 / 三个 sha / 刷入结果 / 一张面板照片`。
