# 存储的完整性与加密（中文译本）

> 译自 [`docs/security/integrity-and-encryption.md`](../../security/integrity-and-encryption.md)，供自己复习用。
> 以英文原版为准；命令、文件名、日志输出保持原样。

关闭[威胁模型](threat-model.zh.md)中的发现 **F5**（rootfs 可被修改）、**F7**（WiFi PSK 明文）
和 **F8**（静态数据未加密）。各部分状态：

| 部分 | 状态 |
| --- | --- |
| 在真实 rootfs 镜像上做 dm-verity 篡改演示（主机上，不需要 root） | ✅ 完成 —— §1 |
| Raspberry Pi 5 上磁盘加密密钥能放在哪 | ✅ 根据 Raspberry Pi 当前源码分析 —— §3 |
| 给板子用的 dm-verity 镜像：[`device-platform-image-verity`](../../../yocto/meta-device-platform-verity/)（initramfs 打开 verity 根） | ✅ 已构建，并逐字节核对 |
| 在 Pi 5 上启动它；在卡上改一个字节 → 恰好在预测的块上报 `EIO` | ✅ [case 10](../../debugging/case-10-dm-verity-one-byte-on-the-card.md) |
| 板上的 LUKS 数据分区（首次开机创建，存放 SSH 主机密钥和 RAUC 状态） | ✅ §5 |
| dm-verity 和 LUKS 的代价，开/不开 Armv8 加密扩展 | ✅ §5 |

## 1. dm-verity，在本项目构建出的镜像上演示

dm-verity 是一个 device-mapper target，它把从只读文件系统读出来的每一个块，
都和一棵 SHA-256 哈希组成的 Merkle 树（哈希树）比对。只有**根哈希**需要被信任；
其余的一切——数据和哈希树本身——都可以放在不可信的 SD 卡上。

[`security/integrity/verity-tamper-demo.sh`](../../../security/integrity/verity-tamper-demo.sh)
在 `device-platform-image` 的真实 ext4 rootfs 上运行它，使用的是和内核 target 同源的
`veritysetup` 用户态代码，然后在 `device-service` 二进制里改一个字节。
完整输出：[`results/security/dm-verity/host-tamper-demo.txt`](../../../results/security/dm-verity/host-tamper-demo.txt)。

```
hash tree: 1495040 bytes for 188743680 bytes of data           (0.79 %)
verify (untouched):                   OK
one byte changed in /opt/device-service/device-service
  filesystem block 31968 × 1024 + 100 = offset 32735332 → verity block 7992
  plain ext4 reads the modified binary:  sha256 f4c988ee… → 62eab4ec…
verify (tampered, original root hash): Verification failed at position 32735232
                                       (= 7992 × 4096, the 4 KiB block that was changed)
attacker rebuilds the tree:            root hash 56ea5b58…  ≠  original 73f2d9e5…
```

这说明了什么：

- **文件系统本身察觉不到。** ext4 会毫无怨言地交出被改过的二进制；正常的读路径上，
  没有任何环节会把文件内容和什么东西做比较。
- **验证是按块、在读取时进行的。** 在设备上，内核会在那个 4 KiB 块**第一次被读到时**
  返回 `EIO`（或者重启/panic，取决于配置的出错模式）——而不是在挂载时。
  所以一个 188 MB 的镜像不需要在启动前整个哈希一遍。
- **哈希树很便宜。** 4 KiB 块大小下，188 MB 数据只要 1.46 MB 的树。
- **重算哈希树很容易；关键全在根哈希。** 修改了镜像的攻击者可以为它重建一棵合法的树，
  只是根哈希不一样。所以保护的强度完全取决于根哈希从哪来——在签名 `boot.img` 里的
  内核命令行上（[secure-boot](secure-boot.zh.md) §1）。**没有 secure boot 的 dm-verity
  只能发现损坏，发现不了攻击者。**

## 2. dm-verity vs dm-crypt vs fs-verity

| | 保护什么 | 粒度 | 可写吗？ | 设备上需要的密钥 |
| --- | --- | --- | --- | --- |
| **dm-verity** | 只读块设备的完整性 | 块 | 否 | 不需要——只要一个可信的根哈希 |
| **dm-crypt / LUKS** | 块设备的机密性 | 块 | 是 | 一把秘密密钥（难点所在，§3） |
| **fs-verity** | 可写文件系统上单个只读文件的完整性 | 文件 | 文件变为不可变 | 不需要——每个文件一个摘要，可选签名 |

对这台设备：根文件系统用 dm-verity（产线镜像里它本来就是只读的——
[hardening](hardening.zh.md)），一个单独的数据分区用 LUKS，存放采集的样本、日志和
网络凭据，rootfs 里不放任何秘密。**单独的 `dm-crypt` 不提供完整性**：一个被改过的
密文块会解密成乱码，文件系统照读不误（除非再加上 `dm-integrity` 做认证加密）。

## 3. Raspberry Pi 5 上加密密钥能放在哪

Pi 5 没有 TPM，也没有安全元件。下面按从差到好列出选项，以及每种选项在面对威胁模型里的
攻击者时实际值多少：

| 密钥放在哪 | 能挡住 A3（拔出 SD 卡）吗 | 能挡住运行中设备上的 root 吗 | 能挡住内核被攻破吗 |
| --- | --- | --- | --- |
| rootfs 里的密钥文件 | 否 | 否 | 否 |
| 由 SoC 序列号 / MAC 推导 | 否——两者在任何运行中的系统上都能读到，或者直接印在板子上 | 否 | 否 |
| 开机时由人输入 | 能 | 否 | 否——无人值守设备，这里不可行 |
| **OTP 设备密钥 + 固件 HMAC，用完即锁**（见下） | **能，需配合 secure boot** | **能，直到重启** | **否** |
| TPM 绑定 PCR / 安全元件 / TEE 持有的密钥 | 能 | 能 | 大体能——取决于 TEE |

Raspberry Pi 当前的固件（2025–2026 年的版本，
[`raspberrypi/utils`](https://github.com/raspberrypi/utils/tree/master/rpifwcrypto) 里的 `rpi-fw-crypto`）
提供了第四行：

- 一把 256 位、**存在 OTP 里的设备唯一私钥**（一个 ECDSA P-256 标量），固件可以自己生成
  （`genkey`），所以密钥从不出现在芯片之外。*写 OTP 是永久性的；在本项目唯一一块板子上不做。*
- **`hmac` 和 `sign`** 操作：使用密钥但不返回密钥。Raspberry Pi 自己给磁盘加密的建议正是如此：
  用 `HMAC(设备密钥, 序列号 + eMMC CID)` 作为 LUKS 口令。
- **每把密钥可以锁到下次重启**（`set-key-status … HMAC_LOCKED READ_LOCKED`），
  以及 `config.txt` 里的 `lock_device_private_key=1`。

由此得到的启动流程：

```
signed boot.img ─► kernel + initramfs（由固件验证）
                     initramfs: 口令 = rpi-fw-crypto hmac(key 1, serial ‖ CID)
                                cryptsetup open /dev/mmcblk0p3 data
                                rpi-fw-crypto set-key-status 1 HMAC_LOCKED|READ_LOCKED
                                switch_root  ── 从这里开始，连 root 也无法再次推导出口令
```

它的局限，必须说出来而不是藏起来：

- `lock_device_private_key=1` 只是 `config.txt` 里的一行。**没有 secure boot 它一文不值**：
  拿着 SD 卡的攻击者把这行删掉就行。有了 secure boot，`config.txt` 就在签名的 `boot.img` 里。
- Raspberry Pi 的文档直说了：*"无法阻止运行在 ARM supervisor 模式下的代码（例如内核代码）
  直接访问 OTP 硬件。"* 一个内核漏洞就能拿到密钥。这就是它和 TrustZone TEE 的区别——
  TEE 连普通世界的内核也能挡住（[optee](optee.zh.md) §5）。
- `cryptsetup open` 之后，卷密钥在卷打开期间一直在内核内存里——任何 dm-crypt 方案都是如此，
  不管有没有 TPM。

## 4. 还没做的（需要板子）

- ~~启动 verity 镜像并篡改卡上数据~~ —— 已完成，见
  [case 10](../../debugging/case-10-dm-verity-one-byte-on-the-card.md)。当时的计划是：
  [`device-platform-image-verity`](../../../yocto/meta-device-platform-verity/)
  （已构建：meta-security 的 `dm-verity-img` class，它的 initramfs 打包进一个内建 dm-verity 的
  内核，根哈希在 initramfs 里——每一部分都在构建主机上相互核对过）；在卡上改一个块；
  展示内核的 `EIO` 和启动结果。这是 §1 的板上那一半，会写成
  `docs/debugging/case-10-*.md`。构建时已经发现的两个坑写在那个层的 README 里
  （分区标签被写进了被验证的文件系统；一个带条件的 `SRC_URI:append` 仍然导致主构建的内核被重编）。
- 根哈希目前放在 initramfs 里，initramfs 又在未经验证的启动分区上的内核镜像里。只有当这个内核
  成为签名 `boot.img` 的一部分时（[secure-boot](secure-boot.zh.md)），根哈希才可信——
  在那之前，这个镜像能发现损坏，发现不了攻击者。
- LUKS 数据分区及其代价：在 Pi 5 上跑 `cryptsetup benchmark`（Cortex-A76 有 Armv8 加密扩展，
  所以 AES-XTS 应该远不是瓶颈），再加上加密/不加密时的读写吞吐，按本项目其他地方的测量方式
  来测——多次重复，不是只取一个数字。

## 5. 板上：数据分区，以及完整性和加密的代价

[`device-platform-image-ab`](../../../yocto/meta-device-platform-verity/recipes-core/images/device-platform-image-ab.bb)
有一个 LUKS2 数据分区（aes-xts-plain64，512 位密钥），由
[`device-platform-data`](../../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/device-platform-data)
在首次开机时创建，之后每次开机打开；SSH 主机密钥和 RAUC 的槽状态放在里面，这正是它们能在
只读根文件系统下保存下来的原因（[hardening](hardening.zh.md) §5）。**它的密钥就是脚本里说明的那个
不安全的替代品**——由板子的序列号推导——因为设计中要用的 OTP 设备密钥（§3）在这块板子上没有烧写。
这里真实的部分是：启动顺序、持久化和代价。

顺序读 150 MiB，每次运行前清空页缓存，每项三次（彼此相差在 ±1.5% 以内）
（[原始数据](../../../results/security/dm-verity/read-throughput-ab-image.txt)，
[基线](../../../results/security/dm-verity/read-throughput-sha256-generic.txt)）：

| | 原始分区 | 经过映射 | 代价 |
| --- | --- | --- | --- |
| dm-verity，`sha256-generic`（加密扩展编成模块） | 85.4 MiB/s | 62.2 MiB/s | −27% |
| dm-verity，`sha256-ce`（编进内核） | 82.9 MiB/s | 69.7 MiB/s | **−16%** |
| LUKS2，`xts-aes-ce` | 83.2 MiB/s | 77.2 MiB/s | **−7%** |

- **为什么"编进内核"只对 dm-verity 有影响。** verity target 是在 initramfs 里建立的，那时任何模块
  都还没法加载，所以它绑定的是那一刻存在的 SHA-256 实现——旧镜像里是 `sha256-generic`，
  尽管几秒钟后 `sha256-ce` 就加载了。把 `CONFIG_CRYPTO_SHA2_ARM64_CE` 编进内核后，verity 读取
  提升 12%，开销从 27% 降到 16%。LUKS 是在模块加载之后才打开的，所以无论如何都能用上 `xts-aes-ce`。
- 这些是从 SD 卡读取的数据，而 SD 卡本身就是瓶颈（约 83 MiB/s）；这些数字说明的是完整性和加密
  *在这种存储上*的代价，而不是 CPU 能做到多快。

## 6. 数据分区的认证加密

§2 说得很清楚：单独的 dm-crypt 只提供机密性，不提供完整性——被改过的密文扇区会解密成乱码，
文件系统照读不误。从 `device-platform-image-ab` 1.1.0 起，数据分区用 `--integrity hmac-sha256`
格式化：每个 512 字节扇区都有一个 32 字节的 HMAC，由 dm-crypt 下面的 dm-integrity 存储
（`data` 下面是 `data_dif`），内核把两者组合成 `authenc(hmac(sha256-ce),xts-aes-ce)`。
XTS 密钥和 HMAC 密钥都来自 LUKS 卷密钥（768 位 = 512 + 256）。

**篡改测试**（[记录](../../../results/security/dm-verity/luks2-integrity-tamper.txt)）：
在原始分区上、两层之下、靠近末尾的空闲区域改一个字节；先读出原字节，事后恢复。

```
read the end of /dev/mapper/data                    -> OK
one byte on /dev/mmcblk0p7 changed (fe -> ff)
read again                                          -> dd: Input/output error
  device-mapper: crypt: dm-1: INTEGRITY AEAD ERROR, sector 1687560
  audit: module=crypt op=integrity-aead dev=254:1 sector=1687560 res=0
  Buffer I/O error on dev dm-2, logical block 210945    (1687560 / 8 = 210945)
byte restored                                       -> OK again
```

和 dm-verity 一样，它没有记忆：把字节恢复，扇区就又合法了。和 dm-verity 不同的是，它的标签是
带密钥的 MAC，而不是公开的哈希树——没有密钥的攻击者造不出合法的扇区，但**可以回放一个旧扇区**：
dm-integrity 用扇区自己的编号来认证每个扇区，而不是用版本号，所以把一对旧的（扇区，标签）写回去
仍然能验证通过。数据的防回滚需要计数器——和 [optee](optee.zh.md) §3.2 是同一个教训。

**代价**（[记录](../../../results/security/dm-verity/luks2-integrity-throughput.txt)，内核 6.12，每项三次）：

| | 读（150 MiB，清缓存） | 写（100 MiB + sync） |
| --- | --- | --- |
| 原始 SD 卡分区 | 85.4 MiB/s | 30–43 MiB/s |
| 普通 dm-crypt（XTS） | 77.2 MiB/s（−7%，§5） | 36–42 MiB/s（≈ 原始） |
| **LUKS2 + integrity** | **64.9 MiB/s（−24%）** | **16–21 MiB/s（≈ −50%）** |

- 读要付出 HMAC 的代价，以及读取与数据交错存放的标签的代价。
- 写要付双倍：dm-integrity 默认的日志模式把每个扇区写两次，这样断电时数据和标签不会不一致。
  替代方案（`--integrity-no-journal` 或 bitmap 模式）是用这种断电一致性换速度。对于一台主要是
  追加写测量数据、并且会毫无预警断电的设备，日志模式是正确的默认值；50% 就是代价。
- 写的几行走的不是同一条路径（原始/dm-crypt 是块设备，integrity 是通过 `/data` 上的 ext4 测的），
  所以它们给出的是代价的量级，而不是精确的百分比。基线是在未激活的槽 B 根分区上测的，
  反正下一次更新也会覆盖它。
- 首次开机更慢：带 integrity 格式化时要把整个分区写一遍来初始化标签（这里 1 GB，大约一分钟）。

## 7. 选择"损坏"时怎么办：重启，然后回退

Case 10 用的是 dm-verity 默认的出错模式：坏块返回 `EIO`，系统继续在一个无法完整读取的根上运行。
从 1.1.0 起，initramfs 用 `--restart-on-corruption` 打开根
（[`dmverity-errmode`](../../../yocto/meta-device-platform-verity/recipes-core/initrdscripts/files/dmverity-errmode)；
模式来自内核命令行上的 `dmverity.error=`——在签名 ramdisk 里——默认 `restart`）：

```
0 286720 verity 1 179:5 179:5 1024 4096 143360 35841 sha256 <root hash> <salt> 1 restart_on_corruption
```

单独看，对于一个损坏的**已提交**槽，这比 `EIO` 更糟：设备会永远重启进同一个损坏里。它只有和 A/B
配合、用在它针对的场景时才是正确选择——**一次更新装进了损坏或被篡改的内容**。在板子上测过
（[记录](../../../results/security/update/on-target-signed-ab.txt)）：

```
install 1.1.0 into slot A (not yet confirmed)
predicted on the host: systemd's first block = fs block 61756, byte +64 = 0x06
on the card: p5 + 61756*1024 = ".ELF", +64 = 06                -> matches
change that byte to 0xff, reboot "0 tryboot" into A
-> 14 s later the device is booting slot B again: partition=3, tryboot=0,
   bootloader boot count 4 (A, B tryboot, A tryboot, B), A never confirmed
byte restored; sha256 of p5 == the bundle's rootfs image
```

PID 1 的第一个块一被读到，内核就重启了；一次性的 tryboot 随之用掉，固件启动了已提交的槽。
整个过程没有用到健康检查、看门狗或网络。第一次运行没能抓到的是内核自己的
`data block … is corrupted` 那一行：失败那次启动的日志只在内存里存在了几秒钟。
下面的"后续"补的就是这个。

另外两点：

- **出错模式差点没进镜像**：替换脚本最初是用 `do_install:append` 安装的，结果构建出来的 initramfs
  里仍然是上游脚本——meta-security 的 bbappend（层优先级 8）在本层（7）之后运行，把它的文件装在了
  我们的文件上面。构建是成功的；只有解开 initramfs 才看出来。现在改成了 `do_install[postfuncs]`。
- 一个损坏的**已提交**槽仍然会无限重启。要打破它，需要一个固件认可的启动计数器（Pi 5 的 bootloader
  目前对 `autoboot.txt` 分区没有这个功能），或者用 `panic_on_corruption` 加一个恢复分区。
  作为局限写明，不解决。

**后续：用 pstore 抓证据。** 从 1.2.1 起，镜像为 ramoops 预留了 256 KiB 内存，任何重启之后，
上一次启动的内核日志都在 `/sys/fs/pstore` 里。这需要一个自己的 overlay：内核源码树里的 `ramoops`
overlay 写 `reg` 时用的是一个地址 cell，而 BCM2712 的 `reserved-memory` 用两个；`ramoops-pi4`
（布局对）声明的却是 Pi 4 的 SoC。用通用版时，`dtoverlay=ramoops` 被接受了，但静默地什么都没产生——
没有节点，没有 pstore——只有把 `/proc/device-tree` 读回来才能看出来
（[`ramoops-pi5-overlay.dts`](../../../yocto/meta-device-platform-verity/recipes-bsp/ramoops-pi5-overlay/files/ramoops-pi5-overlay.dts)）。
两个槽都是 1.2.1，再做一次同样的测试
（[完整日志](../../../results/security/dm-verity/pstore-verity-restart-console.txt)）：

```
predicted on the host: systemd's first block = 61755; card matches; byte changed; tryboot into A
back on B 38 s later; /sys/fs/pstore/console-ramoops-0 holds the failed attempt:
  [3.148742] Run /init as init process
  [4.440277] device-mapper: verity: sha256 using shash "sha256-ce"
  [4.905102] device-mapper: verity: 179:5: data block 61755 is corrupted
  [4.939623] reboot: Restarting system with command 'dm-verity device corrupted'
```

内核拒绝的块正是构建主机上算出来的那个，34 ms 后重启。启动记录器会把 pstore 的内容拷到分区 1，
所以即使事后没人登录，意外重启的原因也能保留下来。（第一次尝试时只更新了一个槽，看到的是另一次
启动的日志：那次尝试跑的是旧镜像，旧镜像的 ramoops 不工作——证据必须在**失败的那个镜像**里，
而不是负责恢复的那个。）
