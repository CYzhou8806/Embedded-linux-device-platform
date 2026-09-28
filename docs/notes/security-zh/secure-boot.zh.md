# Raspberry Pi 5 上的 Secure Boot（中文译本）

> 译自 [`docs/security/secure-boot.md`](../../security/secure-boot.md)，供自己复习用。
> 以英文原版为准；命令、文件名、日志输出保持原样。

关闭[威胁模型](threat-model.zh.md)中的发现 **F4**——在不对项目唯一一块板子执行
不可逆步骤的前提下，能关到什么程度就关到什么程度。那一步之前的所有事情都做了、
验证了；那一步本身用 §5 的检查清单代替。

资料来源：Raspberry Pi 的 [`usbboot`](https://github.com/raspberrypi/usbboot)
（`docs/secure-boot.md`、`secure-boot-recovery5/`、`secure-boot-example/`）和
[`rpi-eeprom`](https://github.com/raspberrypi/rpi-eeprom)，于 2026-09-23 检出。
这条链的细节在不同 SoC 代际和固件版本之间会变，所以直接读了这些源码，
而不是依赖旧的网上文章。

## 1. BCM2712 上的信任链

```
 BCM2712 BootROM（掩膜 ROM，内含 Raspberry Pi 的公钥）       ← 信任根
   │  验证 bootsys：必须由 Raspberry Pi 签名，并且由客户反签（counter-sign）
   │  客户公钥来自 EEPROM，只有当 SHA-256(公钥) == OTP 里的客户密钥哈希 时才被接受
   ▼
 bootsys（第二阶段，在 SPI EEPROM 里）
   │  对照内置的哈希列表检查每一个固件依赖
   │  用客户密钥验证 bootconf.txt 与 bootconf.sig
   ▼
 bootmain
   │  从 SD / USB / NVMe / 网络加载 boot.img + boot.sig
   │  SHA-256(boot.img) 必须匹配，RSA-2048 PKCS#1 v1.5 签名必须能用客户密钥验证通过
   │  ——否则放弃这种启动方式
   ▼
 boot.img  = 一个 FAT 格式的 ramdisk：config.txt、cmdline.txt、DTB + overlay、
             GPU 固件、内核、initramfs。在安全模式下，固件**不会**从它外面加载任何东西。
   ▼
 Linux 内核 + initramfs            ── 固件验证到此为止 ──
   ▼
 根文件系统                        ← 需要 dm-verity，并且它的根哈希要放在
                                     boot.img 里面的内核命令行上
```

这条链上有三点很容易搞错：

- **OTP 里存的是哈希，不是密钥。** OTP 容量小且只能写一次；2048 位的公钥放在
  EEPROM 里，OTP 只"钉住"哪一把密钥是可接受的。所以攻击者替换 EEPROM 里的公钥，
  如果没有对应的私钥，什么也得不到。
- **客户也要给 Raspberry Pi 的固件签名（仅 BCM2712）。** 在 Pi 4 上，ROM 只检查
  `bootsys` 上 Raspberry Pi 的签名。Pi 5 还要求客户的反签，所以任何 bootloader 更新——
  哪怕是 Raspberry Pi 官方发布的真固件——不经产品所有者签名都装不上。
- **内核用到的一切都必须在 `boot.img` 里面。** 一个签过名的内核，旁边放一个没签名的
  `cmdline.txt`，攻击者就能加上 `init=/bin/sh`。把整个启动分区打包成一个签名 ramdisk，
  是这个平台避免逐个文件签名的办法。

链在哪里停止同样重要：**固件只验证到内核和 initramfs，之后一概不管。**
把它延伸到根文件系统是 dm-verity 的工作
（[integrity-and-encryption](integrity-and-encryption.zh.md)），而且只有当 verity 根哈希
位于签名的 `boot.img` 里面时，这种延伸才成立。

## 2. Pi 5 和 Pi 4 有一处不同，它改变了计划

在 Pi 4（BCM2711）上，可以在烧 OTP **之前**先刷一个签名的 EEPROM 来测试。
在 Pi 5 上不行：在密钥哈希写进 OTP 之前，签名的 EEPROM 镜像根本不会运行（板子直接
不启动；较老的 C1 步进板会闪错误码）。所以 Pi 5 的 bootloader 没有
"强制验签但还可以撤回"的测试模式。

在 Pi 5 上**能**可逆地测试的，是一个 `boot.img` 格式正确、能启动：把 `boot.img` 和
`boot.sig` 放到启动分区，并在它的 `config.txt` 里写 `boot_ramdisk=1`。bootloader
就会从这个 ramdisk 启动——但它只在安全模式下才强制验签，所以这证明的是**镜像**没问题，
而不是**签名检查**有效。

## 3. 做了什么（全部可逆，什么都没烧）

### 3.1 签名密钥放在（软件）HSM 里

签名密钥是一把在 SoftHSM2 token **内部**生成的 RSA-2048 密钥
（[`security/signing/hsm-init.sh`](../../../security/signing/hsm-init.sh)）。
它从来没有以文件形式存在过：

```
Private Key Object; RSA
  label:      rpi5-boot-rsa2048
  Access:     sensitive, always sensitive, never extractable, local
```

`never extractable`（永不可导出）和 `local`（本地生成）是 token 强制执行的属性：
没有任何 PKCS#11 调用能把密钥返回出来，而且它是在 token 上生成的，不是导入的。
导出的只有公钥（PEM 格式）。

Raspberry Pi 的工具通过 **HSM wrapper** 直接支持这种用法：一个以
`-a rsa2048-sha256 FILE` 调用、输出十六进制 PKCS#1 v1.5 签名的程序。
[`security/signing/pkcs11-hsm-wrapper`](../../../security/signing/pkcs11-hsm-wrapper)
用 `pkcs11-tool` 实现了它（`CKM_SHA256_RSA_PKCS`，由 token 计算），从一个权限 `0600`
的文件读 PIN 而不是从命令行读，并把每一次签名追加到审计日志。换成真的硬件 HSM，
唯一要改的就是把 `PKCS11_MODULE` 指向那台 HSM 的库。

### 3.2 签名和验证一个启动镜像

```
$ rpi-eeprom-digest -H pkcs11-hsm-wrapper -i boot.img -o boot.sig
$ cat boot.sig
902be6c4…1959                      ← boot.img 的 SHA-256
ts: 1790197620
rsa2048: 0c95a983…                 ← token 做出的签名

$ rpi-eeprom-digest -k public.pem -i boot.img -v boot.sig
Verified OK

# 在偏移 100000 处改一个字节：
$ rpi-eeprom-digest -k public.pem -i boot-tampered.img -v boot.sig
rsa routines:ossl_rsa_verify:bad signature
Verification failure
```

### 3.3 给安全模式用的反签 EEPROM 镜像

`update-pieeprom.sh -f -H pkcs11-hsm-wrapper -p public.pem` 生成了产线会刷写的
EEPROM 镜像：Raspberry Pi 的 `bootsys` 由 token 反签，`bootconf.txt` 已签名，公钥已嵌入。
它**没有被刷写**（§2：没有 OTP，板子就起不来）。

之后又把这个镜像拆开，对照密钥检查：

```
SHA-256(token 里的公钥，bootloader 格式)            fe0144747838fcf8…4475d7b5
SHA-256(从 pieeprom.bin 里提取的 pubkey.bin)        fe0144747838fcf8…4475d7b5
```

两者相等。这个相等是烧 OTP 之前最重要的一项检查（§5）：`program_pubkey=1` 烧进去的
哈希，是根据**正在刷写的 EEPROM 镜像里的公钥**算出来的。所以如果镜像是用错的密钥
构建的，得到的板子就会永远只接受那把错的密钥。

### 3.4 在板子上：用签名 `boot.img` 启动设备

这是整条链里在没有 OTP 的 Pi 5 上**唯一能测**的部分（§2）：签名的 ramdisk 能不能把
真实系统启动起来？通过固件的一次性 **tryboot** 来做，这样任何失败都会在下一次断电
重启时被撤销。这个 ramdisk
（[`make-signed-boot-img.sh`](../../../security/signing/make-signed-boot-img.sh)，
以普通用户身份用 `mkfs.vfat -C` + mtools 构建，经 HSM wrapper 签名）里有内核、
全部设备树和 overlay、`config.txt`，以及一个带标记 `bootimg=signed-test` 的 `cmdline.txt`。

试了三次，而且第一次的诊断是错的：

1. **重启后没有网络。** 没有控制台，只能猜：这块板子是 D0 步进的 Rev 1.1（`d04171`），
   第一版 ramdisk 缺了 `overlays/bcm2712d0.dtbo`。加上之后毫无变化——猜错了。
2. **接上 HDMI 屏幕**，看到了 bootloader 的诊断信息
   （[照片](../../../results/security/secure-boot/tryboot-error6-screen.jpeg)）：
   ```
   Read tryboot.txt bytes 2548
   Loading tryboot.img ...
   Error 6 loading tryboot.img
   ```
   **在 tryboot 模式下，bootloader 加载的是 `tryboot.img`，不是 `boot.img`**——
   这和"tryboot 从不碰正常启动文件"的设计是一致的。这点**确实**有文档，只不过写在
   `autoboot.txt` 的页面上，而不是这次读过的 secure-boot 页面：`tryboot_a_b=1` 存在的
   目的恰恰是"加载正常的 `config.txt` 和 `boot.img`，而不是 `tryboot.txt` 和
   `tryboot.img`"。两次失败里，固件都没走到读取 ramdisk 内容那一步。
3. 改名为 `tryboot.img`/`tryboot.sig`：**设备从签名 ramdisk 启动了**——
   `/proc/cmdline` 里有 `bootimg=signed-test`，bootloader 的 `tryboot` 标志已置位，
   根文件系统按 ramdisk 里命令行的指示从 `/dev/mmcblk0p2` 挂载。但 `device-service`
   卡在 `activating`：`spi0.0` 不存在，固件还加上了它的 `bcm2708_fb` 参数——
   **一个 dtoverlay 都没应用**。能解释的原因是：tryboot 模式下，第二遍配置解析读的是
   **ramdisk 里面的** `tryboot.txt`，而 ramdisk 里只有 `config.txt`。在 ramdisk 里放一份
   `config.txt` 的副本命名为 `tryboot.txt` 之后：`spi0.0` 出现，`device-service` 变为
   active，MCU 应答（`0xac00acc0`）。之后正常重启回到了 dm-verity 系统，tryboot 文件被删除。

两个发现现在都做进了脚本（`--tryboot`）。第二个是更危险的那一类：系统启动了，
看起来很健康，只是某个外设悄悄不见了。

这证明了什么、没证明什么：签名镜像格式正确，并且能完整启动这台设备。
在密钥哈希没进 OTP 的情况下，固件**不**检查签名，所以这**不能**证明未签名或被改过的
镜像会被拒绝——那部分只能在一块已经锁定的板子上演示。

## 4. 没有执行的那一步：`program_pubkey=1`

在 `rpiboot` 的 `config.txt` 里设 `program_pubkey=1` 然后刷写，就会把密钥哈希写进 OTP。
从那以后：

- 只有用这把密钥反签的 `bootsys` 能运行，只有用它签名的 `boot.img` 能启动；
- EEPROM 配置必须签名；无法降级到不支持 secure boot 的 bootloader；
- ROM 不再从 SD/eMMC 加载 `recovery.bin`，所以 bootloader 恢复只能通过 `rpiboot`
  配合一个反签过的 `recovery.bin`。

它**不能撤销，以后也不能换成另一把密钥**。在一个只有一块 Raspberry Pi 5、而且这块板子
还用来做内核和延迟实验（自编内核都通过 `tryboot` 启动，锁定后全都要签名）的项目里，
烧了它就等于这块板子上的其余工作全部结束。`program_jtag_lock`（永久禁用 VideoCore JTAG）
同样没用；它只有在密钥哈希烧写之后才生效。

## 5. 烧 OTP 之前的检查清单

产线（或者第一块工程板）在设置 `program_pubkey=1` 之前应该证明的事情。
标 ✅ 的是这里已经做过的。

**密钥**
- ✅ 密钥是 RSA-2048（BCM2712 唯一支持的长度），在 HSM 上生成，不可导出。
- ⬜ 有备份，而且不依赖创建它的那个人也能恢复（HSM 备份/克隆，或密钥分片）——
  丢了密钥，所有已锁定的设备都再也无法更新。这里用 SoftHSM 代替；这一项正是真 HSM 存在的意义。
- ⬜ 开发密钥和产线密钥分开。开发板永不锁定，或者只锁定到开发密钥。

**镜像**
- ✅ `boot.img` 的签名能用公钥验证通过，被改过的镜像验证失败。
- ✅ EEPROM 镜像里嵌入的公钥，其哈希等于将要烧进去的值。
- ✅ 签名的 `boot.img` 在目标板上以 `boot_ramdisk=1` 启动成功（§3.4；通过 tryboot——
  这要求文件命名为 `tryboot.img`，并且 ramdisk 里要有 `tryboot.txt`）。
- ⬜ bootloader 的 UART 日志显示了预期的 `Customer key hash`（需 `BOOT_UART=1`，
  已在 `boot.conf` 里设置）。
- ⬜ 存在一个反签过的 `recovery.bin` 并且测试过，因为锁定之后它是唯一的恢复途径。

**流程**
- ⬜ 先锁一块板，在它上面跑通完整的现场更新流程（签一个新镜像、部署、回滚到之前的
  签名镜像），之后才锁更多。
- ⬜ 按设备记录 `rpiboot` 的元数据输出（`CUSTOMER_KEY_HASH`、`SECURE_BOOT_PROVISION`、
  序列号、MAC）：锁到了哪把密钥、什么时候、哪个工站。
- ⬜ 保持 `ENABLE_SELF_UPDATE=0`，这样 bootloader 只能通过签名路径更新。

## 6. 其他平台上的同一个问题

Pi 5 "一个签名 ramdisk" 的设计并不常见。BSP 工程师最常遇到的是下面两种：

**Arm Trusted Firmware-A（大多数 Armv8-A SoC）。** ROM 里的 BL1 → BL2（可信启动固件）→
BL31（EL3 运行时 / 安全监控器）、BL32（安全世界操作系统，例如 OP-TEE）和 BL33
（普通世界 bootloader，通常是 U-Boot）→ Linux。每个镜像都按 TBBR 信任链，通过 FIP
（Firmware Image Package）里的 X.509 证书认证；信任根是熔丝里的 ROT 公钥哈希。
防回滚使用非易失计数器（同样在熔丝里），与每张证书里的计数器比较。

**U-Boot verified boot（FIT 镜像）。** 内核、DTB 和 initramfs 打包成一个 FIT 镜像；
U-Boot 在自己的 control DTB 里持有公钥，启动前验证。重要细节是要签名
**configuration（配置）**，而不只是签名各个镜像：每个镜像单独签名的话，攻击者仍然可以把
一个合法签名的内核和另一个合法签名的 DTB 混搭起来。U-Boot 自身还必须由前一级
（TF-A 或 SoC ROM）验证，否则信任链的起点就有个洞。

**Zynq-7000。** BootROM 用 RSA-2048 认证 FSBL（主公钥的哈希在 eFUSE 里），并可以用
AES-256 解密它。AES 密钥放在 **eFUSE**（永久，什么都抹不掉）或 **电池供电的 RAM（BBRAM）**
（可以擦除或替换，电池没电就会丢——这恰恰也是它的用处：防拆电路可以把它擦掉）。
在两者之间选择，就是在"永远不能改"和"可以被故意销毁"之间选择。

和这些相比，Pi 5 用灵活性换取了简单：一把客户密钥、一个签名文件、没有多级证书链，
而且上述资料里也没有描述针对 `boot.img` 的通用防回滚计数器——这必须由更新系统来处理
（[update-and-provisioning](update-and-provisioning.zh.md)）。

### 3.5 A/B 两个槽里都放签名 `boot.img`

§3.4 是一次性地启动了一个签名 ramdisk。`device-platform-image-ab` 1.1.0 及之后的版本在
**每个启动槽**里都放一个，RAUC 更新包里的 `boot` 镜像也是同一个签名分区
（[`make-signed-ab-release.sh`](../../../security/signing/make-signed-ab-release.sh)，
[记录](../../../results/security/update/on-target-signed-ab.txt)）：

```
p2 / p3   boot.img  （FAT ramdisk，62.8 MB：带 dm-verity initramfs 和根哈希的内核、
                     DTB、overlay、config.txt + tryboot.txt、
                     cmdline-a.txt / cmdline-b.txt）
          boot.sig  （RSA-2048，HSM 密钥 01）
          config.txt: boot_ramdisk=1
```

构建过程从不碰 boot 密钥：bitbake 产出镜像，签名主机取出分区 2 的内容，打包并通过 HSM
签名 ramdisk，写进镜像副本的 p2 和 p3，再用它构建更新包（先开发签名，再 release 签名）。
刷写之前检查过：rootfs 分区与 verity 镜像逐字节相同，根哈希能验证它，**同一个根哈希出现在
签名的内核里**（initramfs 以未压缩形式嵌入），签名能用公钥验证通过。

在板子上：

- **从 p2 正常启动**：p2 里只有 `boot.img`、`boot.sig`、`config.txt`；内核是 6.12.93，
  `root=/dev/mmcblk0p5 rauc.slot=A` 来自 ramdisk 内部。
- **更新 1.1.1 → B，tryboot**：`partition=3 tryboot=1`，`root=/dev/mmcblk0p6 rauc.slot=B`。
  ramdisk 里的同一个 `config.txt` 用 `[boot_partition=2]` / `[boot_partition=3]` 同时服务两个槽——
  这个过滤条件在 ramdisk 内部的第二遍配置解析里也生效，这一点没有任何文档说过。
  （如果不生效，ramdisk 默认的 `cmdline.txt` 指向槽 A，B 启动就会 verity 校验失败并回退——
  默认值正是为这种情况选的。）健康检查通过 → 提交，`[all] boot_partition=3`。

这在这块板子允许的范围内补上了威胁模型 F4 那一行的缺口：固件 → 签名 ramdisk →
内核 + 根哈希 → 经验证的根文件系统，这条链现在在每个槽、每次更新里都存在。
第一环——固件**强制**验签——仍然是 OTP 那一步（§4）。
