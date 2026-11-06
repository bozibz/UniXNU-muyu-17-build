# HANDOFF — 2026-11-06 傍晚（UniXNU-muyu / iPadOS 27.0.1 24A446）

这一轮从"根策略读取触发 VM 断言"一路推进到**第一次进入用户态**，然后停在 prelink 的 stub 陷阱。
下面按"现在的状态 / 走过来的证据链 / 下次怎么接"写，所有结论都有设备照片或本地文件作依据。

---

## 1. 现在的状态（一句话）

存储栈全通：UFS 窗口 → APFS 容器 → 卷 → **根挂载成功** → exec launchd；AMFI 的 exec 拒绝
已用 24A446 trust cache 解决；当前卡在

```
fileset: original component called an unmapped kernel function from 0xfffffff00aa7bd74
```

即某个**原始 Apple kext 调用了一个本内核不存在的内核函数**（prelink 把该站点绑到了 panic stub）。

- 已刷镜像：`fw/vendor_boot_27-u38.img` = `21578bcf…`
- 内核：`8b9ce774…`（含 `stub_sites` 报告）
- 设备状态：能进 fastboot，能刷，照片走 `logs/2026-11-06-*.jpg`

## 2. 证据链（每一环的判定依据）

| 步骤 | 观察 | 结论 |
| --- | --- | --- |
| u22 | `2c copied` 之后不再有 VM 断言，`step 1…7` 全打出来 | `kmem_alloc` 五参数 ABI 判断正确，换 `IOMallocData` 生效；主控接管、类注册、服务创建都通 |
| u22 | `root step 3; root manifest rejected` | 刷入的 userdata 是 **manifest version 2**（`OS`/`SystemOS` 两个窗口 + `OS.vroot`/`SystemOS.vroot` 两个 data object） |
| u24 | `stage=owner signature and hash table` | `.vroot` 对象找到了、71.6 MiB 读完了，但当时把它当整段信封喂 `vr_open`（少 544 B 头） |
| u25 | `SourceStorage: UFS started through the public IOStorageFamily interface` | 拼装信封（握手签名头 + 对象里的哈希表）+ `vr_open` 通过，两个窗口发布成功 |
| u25 | `offline owner SystemOS mount failed: 80` | 进入 `native_owner_cache_prepare()` 的 EAUTH |
| u26 | `stage 6 mount 0 ro 1 dev 1 ver 1 vol 1` | 授权/查卷/挂载全过，EAUTH 只在收尾 |
| u27 | 无任何 `RootBinding:` 输出 | 重校验那次查询**返回 1**，EAUTH 来自别处 |
| u28 | `bm 3` | EAUTH 来自 `native_owner_publish_boot_manifest()`：`/chosen` 已有身份 |
| u29/u30 | 仍 `bm 3` | loader 的身份是**合成**的（`MuyuBringup.BootSet.27` / `MuyuBringup.OwnerBootSet.v1`），内核按窗口摘要推导，二者构造上永不相等 |
| u30 | `offline scratch Data mount failed: 2` | Data 卷（ENOENT），进入 Data provider 这条线 |
| u33 | `… 2 stage 0 dev seed 0+0` | 种子查询确实返回 0，且驱动里的 `printf` **不出现在控制台** |
| u34 | `NativeRamBlock: no IOMedia iterator for 292E72B9-…` | 查询走到了"没有匹配媒体"这一支 |
| u35 | 同上，`ready=0` 且没有 start panic | `NativeRamBlock::start()` **从未被调用**：`IOResources` 早于 step 4 注册，personality 不再重匹配 |
| u36 | `step 8 data disk` / `Data disk started` / `ready=1 bytes=214748364800` | 按类名显式实例化解决；但 UUID 是 uke 的 |
| 离线 | `fw/data-seed.img` 解包后 3 处 `APSB` 的 `+240` 全是 `58B481F8-4243-4430-BF60-DDD1638D4C6E` | 本板种子 UUID ≠ uke 的 `292E72B9-…` |
| u37 | `BSD root: disk5s1` + `apfs_vfsop_mountroot` + `unexpected SIGKILL of launchd (namespace 9 code 0x8)` | 根挂载成功、launchd exec 被 MAC 策略拒绝 |
| 离线 | `payload.json` 的 `trust_cache_sha256: null`；board 钉的是 21H461 那份，不含 24A446 launchd(`3d2ccf3d…`) | 载荷根本没带 trust cache |
| u38 | 不再有 launchd SIGKILL；改为 `fileset: … unmapped kernel function from 0xfffffff00aa7bd74` | trust cache 生效，进入下一个问题 |

## 3. 这一轮改了什么（都已提交）

- **UniXNU-muyu**（`86b3f4c`）：`package/SourceRootPolicies.cpp`（`IOMallocData`）、
  `package/SourceStorage.cpp`（step 8 显式启动 Data 盘）、`package/SourceRootBinding.cpp`（失败原因）、
  `drivers/ShengUfs/{RootLayout.h,RootDiagnostics.h,UfsXnuCompat.h,NativeUfs.cpp}`（manifest v2 + 信封拼装 +
  `IOMallocData`）、`docs/IPADOS27-MIGRATION.md` §6.51–§6.56。
- **UniXNU**（`e246e7b` 及之前）：`patches/xnu-12377/offline-owner-systemos-mount.patch`
  （缓存准备各阶段与 `bm` 状态进 panic）、`offline-owner-boot-manifest.patch`（接受 loader 的同一/合成身份）、
  `zzz-boot-progress.patch`（跟随上下文）。
- **UniXNU/drivers/RamBlock**（`12742d0`）：`start()` 每个静默失败改成带参数的 panic；种子查询失败改 panic。
- **UniXNU-27**（`0cca4ac`）：`kits/XnuPrelink/{fileset_segmented,fileset_emit}.py` 把每个 stub 的
  `site → target` 记进 `kernel.json` 的 `stub_sites`。

## 4. 常用命令（这台机器上验证过）

```bash
# 设备目录 / 输出目录
export UNIXNU_DEVICE=/tmp/spike27
export UNIXNU_OUT=/tmp/spike27-out
export UNIXNU_XNU_HEADERS_DIR=/tmp/spike27-out/muyu/ipados-27.0.1-24A446/muyu-27/tree/build/xnu/fileset-reference/lto-c43d4bdcb1dfa010/source
export UNIXNU_UNIQUE_CHIP_ID=$(cat ~/.unixnu-muyu-chip-id)
export PATH=/tmp/pybin:$PATH
cd /Users/diannao/pad7pro-xnu/UniXNU-27

./unixnu build kernel                       # ~1-2 min 增量
./unixnu build payload --data-ramdisk /Users/diannao/pad7pro-xnu/fw/data-seed.img \
    --trust-cache /Users/diannao/pad7pro-xnu/fw/trust-24A446/24A446-original-trust.bundle
python3 /Users/diannao/pad7pro-xnu/UniXNU-muyu/scripts/patch-vendor-boot.py \
    --base /tmp/vendor_boot_base_96m.img \
    --payload /tmp/spike27-out/muyu/ipados-27.0.1-24A446/muyu-27/payload/payload.img \
    --output /Users/diannao/pad7pro-xnu/fw/vendor_boot_27-uNN.img
cd /Users/diannao/pad7pro-xnu && tools/muyu-flash-mode.sh xnu fw/vendor_boot_27-uNN.img
```

注意：

- 设备树里的 `drivers/`、`kits/` 是指向 `/tmp/spike27/drivers`、`UniXNU-27/kits` 的符号链接；
  `package/` 也是链接。改完仓库文件要 **cp 到 `/tmp/spike27/package|scripts`** 才生效（本轮的 package 改动已同步）。
- `.config` 在 `…/muyu-27/.config`（手工维护）。改 `CONFIG_XNU_*` 后要
  `rm -rf tree/build/xnu/ios-obj tree/build/xnu/ios-sym` 再编。
- `fw/data-seed.img`、board.toml 的 `trust_cache_sha256`、kernel args 都在 `/tmp/spike27/board.toml`。
- 刷机只能用 `tools/muyu-flash-mode.sh`；每次都要**强制关机 → 音量下+电源**进 fastboot。
- 照片在 `/Users/diannao/pad7pro-xnu/logs/2026-11-06-*.jpg`；读图用
  `/Users/diannao/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/bin/python3`
  + `ImageOps.exif_transpose`，再按需要 `transpose(ROTATE_270)` 转正。

## 5. 下次从哪接（唯一任务）

**定位 `0xfffffff00aa7bd74` 属于哪个 kext，以及它想调用的内核函数是哪个。**

已经排除的三条路（§6.56 有细节）：输出镜像没有 `LC_FILESET_ENTRY`（是 MH_MAGIC_64）；
原始 cache 的 entry 是 `0x80000035`（不是 0x1f），换算后越过末尾；`__PRELINK_INFO` 的 load addr
在 `__PRELINK_TEXT`，覆盖不到该地址（kext 代码被搬到合并后 `__TEXT_EXEC` 低半段）。

两个可行做法（都只改本地工具，不用刷机就能验证一半）：

1. **per-target trampoline**：在 `fileset_segmented.rebind` 里为每个不同的 stub 目标生成一个独立
   trampoline，把目标地址作为立即数；panic 文本直接是"缺 `_符号名`"。
2. **代码段区间表**：让 prelink 报告输出"合并后 `__TEXT_EXEC` 内每个 kext 代码段 [start,end) → owner"，
   运行时返回地址一查即得（需要先弄清 kext 代码放置规则，`fileset_layout.py` / `fileset_segmented.py`）。

拿到符号名后再判断：补上该内核函数（最可能），还是让该 kext 走另一条不需要它的路径。

## 6. 已知的、暂时可以不管的事

- `apfs_extract_root_hash_ara:3780: could not retrieve storage-volume-uuid blob from device tree` +
  `import_boot_forwarded_root_hash … (2)`：根卷完整性哈希没导入；目前不影响挂载/exec。
- `Failed to open swap file 2`：动态 swap 打不开（`/private/var` 还没挂），属于后续问题。
- `IOCPUInterruptController: not registry member at registerService()`：uke 同样会打，良性。
- 单核启动（`sheng-smp=0` + `armcpu-spi-ipi=0`），SMP/IPI 的 PAC 陷阱仍未处理。
- 一票调试探针还留在树里（`NativeRamBlock` 的 panic、`SourceStorage: step 1..8`、`step 2a/2c`）；
  通了之后要收回并改成软失败。
