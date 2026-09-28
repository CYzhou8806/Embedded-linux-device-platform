# 签名更新、密钥与 provisioning（中文译本）

> 译自 [`docs/security/update-and-provisioning.md`](../../security/update-and-provisioning.md)，供自己复习用。
> 以英文原版为准；命令、配置项、日志输出保持原样。

关闭[威胁模型](threat-model.zh.md)中的发现 **F6**（没有更新机制）和 **F14**（没有密钥管理），
并为 F12 的设备身份签发证书。状态：

| 部分 | 状态 |
| --- | --- |
| PKCS#11 token（SoftHSM2）里的密钥层级 | ✅ 完成 |
| BitBake 构建的 RAUC 更新包，用开发密钥签名 | ✅ 完成 |
| 用 HSM 密钥做 release 重签名，验证矩阵 | ✅ 完成 —— §2 |
| 产线工站：设备身份 → 证书 → 审计日志 | ✅ 对着测试替身完成 —— §5 |
| 用固件 `tryboot` 在 Pi 5 上做 A/B 槽：安装、自动回退、提交 | ✅ 在板子上 —— §3.1 |
| 防回滚 | ✅ 在板子上（`min-bundle-version`）—— §4 |
| 不动 keyring 就轮换签名密钥；用 CRL 吊销旧密钥 | ✅ 用 token 里的密钥（签发了密钥 05，吊销了 release-1）—— §6 |
| 没有 RTC 的板子：NTP 之前证书时间检查会怎样 | ✅ 在主机上用板子真实的开机时钟测过 —— §7 |

## 1. 四把密钥，四份工作

所有私钥都在 token 内部生成，属性为 `sensitive` 和 `never extractable`；离开 token 的只有公钥和证书。

| Token id | 密钥 | 签什么 | 被谁信任 | 脚本 |
| --- | --- | --- | --- | --- |
| 01 | RSA-2048 boot 密钥 | `boot.img`、EEPROM 配置、反签 Pi 固件 | Pi 5 的 OTP 密钥哈希 | [`hsm-init.sh`](../../../security/signing/hsm-init.sh) |
| 02 | P-256 Device CA | 每个设备身份一张证书 | 后端 | [`hsm-device-ca-init.sh`](../../../security/signing/hsm-device-ca-init.sh) |
| 03 | P-256 Update CA | 只签 release 签名证书 | 每台设备（它的 RAUC keyring） | [`hsm-update-keys-init.sh`](../../../security/signing/hsm-update-keys-init.sh) |
| 04 | P-256 release 签名密钥 | 更新包 | 经由 Update CA | 同上 |

为什么要分开：

- **一处泄露，只有一个后果。** Device CA 密钥泄露，别人能伪造设备身份；但不能同时还让他们能发固件。
- **轮换不用动现场设备。** 设备信任的是 *Update CA*，而不是签名密钥。如果签名密钥泄露或过期，
  CA 签发 `release-2`，设备不用更新 keyring 就接受它。boot 密钥（01）没有这层间接——Pi 5 在 OTP 里
  只钉一个密钥哈希——所以丢了它或泄露它的后果严重得多。
- **PIN 从不出现在 argv 里。** `pkcs11-tool` 通过 `--pin env:…` 获取，OpenSSL 的 engine 通过一个 `0600`
  的配置文件，RAUC 通过 `RAUC_PKCS11_PIN`。（这些脚本的第一版是把 PIN 放在命令行上的，任何用户用 `ps`
  都能看到；写 Device CA 脚本时发现的，所有脚本一起改了。）

## 2. 构建签名 vs release 签名

```
 构建服务器（BitBake）                      签名主机（HSM）
 device-platform-bundle.bb                 sign-release-bundle.sh
   rootfs = device-platform-image-prod       rauc resign
   用开发密钥签名  ─────────────────►        验证开发签名
   （普通文件，只在构建服务器上）             用 token 里的密钥 04 签名
                                               对照 Update CA 验证
                                               追加到签名审计日志
```

构建过程永远接触不到 release 密钥，而 release 步骤只接受开发签名能验证通过的更新包——
所以它签的是构建产出的东西，而不是随便递给它的什么东西。

更新包（[`device-platform-bundle.bb`](../../../yocto/meta-device-platform/recipes-core/bundles/device-platform-bundle.bb)）：
RAUC 1.15.2，**verity** 格式，一个 `rootfs` 槽（产线镜像，ext4，184.5 MB，更新包 54 MB），
compatible 字符串 `device-platform-rpi5`。

设备会接受什么——也就是 `rauc install` 第一步做的签名检查，在主机上用 `rauc info` 和产线 keyring 运行
（[`test-bundle-verification.sh`](../../../security/signing/test-bundle-verification.sh)，
输出在 [`results/security/update/`](../../../results/security/update/bundle-verification.txt)）：

| 更新包 | Keyring | 结果 |
| --- | --- | --- |
| release（用 HSM 密钥重签） | 产线（Update CA） | **接受** |
| 开发版 | 产线 | 拒绝：`unable to get local issuer certificate` |
| release | 开发 | 拒绝：`unable to get local issuer certificate` |
| release，签名里改 1 字节 | 产线 | 拒绝：`Signature data is no valid CMS` |
| release，payload 里改 1 字节 | 产线 | **`rauc info` 接受**——见下文 |
| 同上，payload 对照签名的根哈希检查 | — | **失败**：`Verification failed at position 27168768`（被改的那个 4 KiB 块） |

由此发现的两件事：

- **在 verity 格式里，签名合法不代表 payload 完好。** 签名覆盖的是 manifest；manifest 里有一棵覆盖 payload
  的 dm-verity 树的根哈希；payload 在 `rauc install` 挂载更新包时由内核逐块检查。所以 `rauc info` 通过，
  对 payload 什么都说明不了——检查发生在安装时，测试里用 `veritysetup` 在用户态复现了这个检查。
  好处是设备在信任读到的内容之前，从来不必把整个更新包哈希一遍（甚至不必完整下载）。
- **会检查证书用途。** RAUC 默认的用途检查拒绝了 `codeSigning` 证书（`unsuitable certificate purpose`）；
  设备的 `system.conf` 需要 `[keyring] check-purpose=codesign`，也就是测试里用 `-C` 传的那个设置。

## 3. Raspberry Pi 5 上的 A/B

meta-rauc-community 的 Raspberry Pi 示例用 U-Boot，Pi 5 还要额外用 meta-lts-mixins 来获得更新的 U-Boot。
这和本项目有两处冲突：Pi 5 secure boot 是由固件直接验证 `boot.img`（[secure-boot](secure-boot.zh.md)），
而且本项目已经依赖固件自己的一次性 `tryboot` 来安全地启动实验内核。所以设计上用固件的 A/B 支持代替 U-Boot，
通过 RAUC 的 `custom` bootloader 后端接入：

```
mmcblk0p1  autoboot.txt（FAT，很小）   [all] boot_partition=2   [tryboot] boot_partition=3
mmcblk0p2  boot A  — boot.img + boot.sig   （内核命令行：root=p5，verity 根哈希）
mmcblk0p3  boot B  — boot.img + boot.sig   （内核命令行：root=p6，verity 根哈希）
mmcblk0p5  rootfs A（只读，dm-verity）
mmcblk0p6  rootfs B
mmcblk0p7  data — LUKS：样本、日志、WiFi 凭据、SSH 主机密钥、RAUC 状态
```

- **安装**：RAUC 写入未激活的那一对 boot + rootfs，然后 custom 后端用 `tryboot` 重启——固件只启动另一对**一次**。
- **提交**：如果新系统健康地起来了（device-service 在运行、网络可达——和 `devbus-bootcheck.service` 已经为
  内核实验做的检查是同一类），一个服务把该槽标记为 good 并改写 `autoboot.txt`，使其成为默认。
- **回滚**：如果没起来，任何一次重启——看门狗、`panic=10`、断电——都会回到旧的那一对，因为 `tryboot` 是一次性的。
  写入过程中断电，损坏的永远只是**未激活**的那个槽。
- 每个启动分区都带自己的签名 `boot.img` 和自己的 verity 根哈希，所以无论启动哪个槽，信任链都覆盖到。

Raspberry Pi 2026-05 的固件还为 EEPROM bootloader 本身加了 A/B 更新，那会是同一设计的下一层。

### 3.1 在板子上

镜像 [`device-platform-image-ab`](../../../yocto/meta-device-platform-verity/recipes-core/images/device-platform-image-ab.bb)：
上面的分区布局；`autoboot.txt` 带 `tryboot_a_b=1`（这样固件会读被尝试分区的正常 `config.txt`）；一个 `config.txt`
用 `[boot_partition=N]` 选择 `cmdline-a.txt` 或 `cmdline-b.txt`；一个 RAUC custom 后端
（[`rauc-tryboot-backend`](../../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/rauc-tryboot-backend)，
它的状态机先在主机上测过：[`tests/test-rauc-backend.sh`](../../../yocto/meta-device-platform-verity/tests/test-rauc-backend.sh)）；
以及一个健康检查，只有当 device-service 处于 active 且 MCU 应答时才运行 `rauc status mark-good`。
meta-rauc 的 `rauc-mark-good` 会无条件把每个启动过的槽标为 good，被排除在镜像之外。完整记录：
[`results/security/update/on-target-ab.txt`](../../../results/security/update/on-target-ab.txt)。

| 测试 | 结果 |
| --- | --- |
| 在 1.0.0 系统上安装一个 release 签名的 0.9.0 更新包 | 拒绝：`Version mismatch: Expected at least '1.0.0' but bundle manifest has '0.9.0'` |
| **失败的更新**：槽 B 收到一个 SSH 永远起不来的镜像 | tryboot 进 B，设备不可用；**断电一次 → 回到 A**，`autoboot.txt` 从未改变，B 保持 `bad` |
| **成功的更新**：1.0.2 装进 B | 安装（约 15 秒）；tryboot 进 B；开机 30 秒后健康检查 → `marked slot(s) rootfs.1 as good` → 后端提交：`[all] boot_partition=3`；正常重启后仍停在 B |
| 跨槽的状态 | `/data` 上的 SSH 主机密钥在 A 和 B 上是同一把 |

那次失败的更新不是故意安排的：那个更新包是从一个更早的、有真实 bug 的镜像构建的（见下）。它恰好走了这个设计
存在的意义所在的那条路径。

有四个问题只在板子上才发现，在构建主机上全都看不见：

- **镜像里没有 `install`。** 把镜像精简到 171 个包时去掉了 coreutils；这里的 BusyBox 没有 `install` 小程序。
  数据分区创建并挂载了，然后脚本最后一行失败了，SSH 主机密钥脚本也跟着失败——没有 SSH，而没有密码的管理员
  没有任何其他方式登录。现在脚本用到的每一个命令都会对照镜像的 rootfs 检查。
- **SSH 依赖了新组件。** 现在主机密钥只在 `/data` 挂载且正常时才放到 `/data`，否则放到 `/run`：
  远程访问不能依赖最可能出问题的那个部分。
- **坏启动之后没有证据。** 在只读系统上，journal 只在内存里。现在有一个启动记录器，在开机两分钟后把本次启动
  失败的 unit 和 journal 写到分区 1（保留最近三次）；拔出卡就足以看出 SSH 为什么挂了。
- **健康检查从来没运行过。** 它是 `WantedBy=multi-user.target`，并且排在 `device-service` 之后，而
  `device-service` 自己又排在 `multi-user.target` 之后：systemd 记录了
  "Found ordering cycle … deleted to break ordering cycle"，每次开机都把这个任务丢掉。失败模式是安全的——
  什么都没被提交——但设计所依赖的那道门根本不存在，而只有一行日志说明了这一点。现在改由 timer 启动。

## 4. 防回滚——为什么光有签名不够

每一个旧版本都是合法签名的，包括那些有已知漏洞的。没有防回滚，攻击者就装上去年的版本，然后利用去年的 bug。

- **在更新器里**（板上验证过，§3.1）：RAUC 1.15 内置了这个功能——`system.conf` 里的 `[system]
  min-bundle-version` 拒绝任何 manifest 版本更老的更新包。每个版本都附带一个把下限提高的 `system.conf`，
  所以一旦它运行起来，更老的都装不上。来自 RAUC 自己的文档和 API 的两个注意事项：有意的回滚（某个版本有问题）
  必须把旧内容用一个**新**版本号发布；而且 D-Bus 安装调用接受 `ignore-version-limit`，所以必须限制谁能调用
  RAUC 的 D-Bus API，否则 root 可以直接让它跳过检查。（在找到这个选项之前，这里的计划是写一个 `pre-install`
  处理器，比较 RAUC 导出给处理器的 `RAUC_MF_VERSION`——内置检查是更好的答案。）这个下限存在经过验证的 rootfs 里，
  所以只要运行中的系统可信它就成立；`DEVICE_PLATFORM_RELEASE` 必须由发版流程设置，并且只能增加。
- **面对拿着 SD 卡的人：** 他不需要更新器；他直接写入一个旧的、合法签名的镜像。这只能靠一个他无法重置的计数器
  来阻止：OTP 位（物理上单调——每发一个版本多烧一位；数量有限，不可逆）、eMMC 的 RPMB 计数器，或者 TPM。
  Pi 5 有 OTP 行，但 SD 卡上没有 RPMB。
- OP-TEE 的工作里得出了同样的教训（[optee](optee.zh.md) §3.2）：恢复一份旧的安全存储副本，让 TA 创建了
  第二个身份。**任何通过可重写存储来执行的规则，都可以通过回滚那份存储来撤销。**

## 5. Provisioning（产线注入）

[`tools/provision-device.sh`](../../../tools/provision-device.sh) 就是工厂工站。设备自己生成身份密钥
（[optee](optee.zh.md) 里的 TA）；工站从不接触私钥：

1. 读序列号；如果已经在日志里，拒绝；
2. 在设备上执行 `devid provision` → 得到公钥（如果设备已经有身份，TA 会拒绝——那是一台不是我们 provision 的设备）；
3. **持有证明**：发一个新鲜的 nonce，用上报的公钥验证签名；
4. 为这把公钥签发 X.509 证书，由 HSM 里的 Device CA 签名（`CA:FALSE`、`digitalSignature`、`clientAuth`、
   subject = 序列号）；
5. 追加一行 JSON：时间、工站、操作员、序列号、密钥指纹、证书序列号和指纹、CA 指纹。

第 3 步的存在源于 TA 的一个设计选择：证明持有密钥的常规方式是 CSR，但 TA 拒绝签任何东西，除了做过域分隔的
身份挑战。所以持有证明走挑战路径，证书则由一个只带 subject 的一次性请求构建，用 `-force_pubkey` 塞入设备的公钥。

在没有板子的情况下，对着一个明确标注的软件替身（`testing/fake-devid`，密钥在文件里）测试——
[`test-provisioning.sh`](../../../security/optee-ta/device_identity/testing/test-provisioning.sh)，
[`results/security/provisioning/`](../../../results/security/provisioning/station-tests.txt)：

```
PASS  new device is provisioned and certified
PASS  same serial a second time is refused (log)
PASS  device with an identity we never issued is refused
PASS  device that can't prove possession is refused
PASS  certificate carries the device's key, not the throwaway CSR key
PASS  exactly one log record
```

真正的产线会多出而这里没有的：日志放在一个工站只能追加、不能改写的数据库里；在同一次会话中在设备上生成
SSH 主机密钥和数据分区的 LUKS 设置；以及在设备获得身份之前，先检查它启动的是预期的签名镜像。

## 6. 轮换和吊销 release 密钥

§1 声称两级层级让签名密钥可以在不碰设备的情况下被替换。本节测试这个说法，以及通常伴随它的另一个说法：
泄露的密钥可以被吊销。

[`hsm-rotate-release-key.sh`](../../../security/signing/hsm-rotate-release-key.sh) 在 token 里生成一把新密钥（id 05），
让 Update CA（id 03）签发 `release signing 2`，把两张证书记进一个小型 `openssl ca` 数据库，吊销 release-1
（`keyCompromise`）并签发一份 CRL——每个签名都由 token 里的 CA 密钥完成。
[`test-key-rotation.sh`](../../../security/signing/test-key-rotation.sh) 通过正常的 release 步骤用 release-2
给真实的更新包签名，然后跑下面的矩阵。测了两次：第一次用一个同样配置、基于文件的一次性 CA，第二次用 token 里的
真密钥（密钥 05，release-2 序列号 `7EA5…97D3`，release-1 `0FC4…A2E3` 被吊销）——结果完全相同
（[记录](../../../results/security/update/key-rotation-and-revocation.txt)）：

| 更新包 | 设备 keyring | `check-crl` | 结果 |
| --- | --- | --- | --- |
| release-1 | 只有 CA（出厂状态） | 关 | 接受 |
| **release-2** | **只有 CA（出厂状态）** | 关 | **接受——轮换不需要设备上做任何事** |
| **release-1** | CA + CRL | 开 | **拒绝：`certificate revoked`** |
| release-2 | CA + CRL | 开 | 接受 |
| release-1 | CA + CRL | 关 | **接受**——RAUC 只警告 `Detected CRL but CRL checking is disabled!` |
| release-2 | 只有 CA | 开 | **拒绝：`unable to get certificate CRL`** |
| release-2 | CA + 过期的 CRL | 开 | **拒绝：`CRL has expired`** |

**在设备上**：更新 1.1.1 只用 release-2 签名，装到 keyring 里只有 Update CA 的板子上：
`Verified inline signature by '… release signing 2'`，安装、tryboot、提交
（[记录](../../../results/security/update/on-target-signed-ab.txt)）。

**设备上的吊销**（1.2.0 起）：镜像里的 keyring 是 Update CA **加上** CRL，`system.conf` 里 `check-crl=true`。
recipe 拒绝构建 `check-crl=true` 但没有 CRL 的 keyring，因为这种组合会拒绝所有更新包。在板子上：

```
rauc install 1.1.0 (signed with release-1):  signature verification failed: Verify error: certificate revoked
rauc install 1.2.1 / 1.2.2 (release-2):      Verified inline signature by '… release signing 2' -> installed, committed
```

第一份 CRL 签发时有效期是 30 天；进镜像之前重新签发为 365 天，因为设备上的 CRL 一过期，就会挡住所有更新。
续签（`CRL_DAYS=365 hsm-rotate-release-key.sh`）现在是每次发版的一部分，远早于 `nextUpdate`（2027-09-24）。

这意味着：

- **轮换和吊销是不对称的。** 轮换只靠签名端就能完成。吊销只在同时持有 CRL **并且** `check-crl=true` 的设备上有效——
  CRL 必须送到设备上，而这通常意味着放在一个由设备仍然信任的密钥签名的更新里。离线或从未更新过的设备仍然接受
  被吊销的密钥。密钥泄露时的顺序是：签发 release-2，发一个（用 release-2 签名的）更新，里面带上 CRL 并打开检查，
  然后接受"从不接收这个更新的设备会一直暴露"这个事实。
- **检查必须和 CRL 一起下发，从出厂就开始。** keyring 里没有 CRL 却开了 `check-crl=true`，会拒绝**所有**
  更新包，包括合法的。在现场打开它却没把 CRL 一起放过去，就等于把更新通道变砖了。
- **CRL 会过期，过期的 CRL 同样会变砖。** 每份 CRL 都有 `nextUpdate`；过了这个时间，RAUC 拒绝所有更新包。
  一台在仓库里放得比 CRL 有效期还长的设备，就完全没法更新了。RAUC 的文档对此有明确警告。可选方案：长寿命的 CRL
  （一年；这样吊销信息最多要一年才算"新鲜"，但这无所谓，因为设备本来就只能通过更新得知吊销），或者每次发版都刷新 CRL。
- **吊销做不到的**：泄露**之前**用 release-1 签的更新包也一起被吊销了。如果还有东西必须安装它们，就得用 release-2
  重签——release 步骤做的正是这件事。

boot 密钥（id 01）这些都没有：Pi 5 在 OTP 里钉死一个客户密钥哈希，上面没有 CA，也没有 CRL。泄露的 boot 密钥会被
每一台已锁定的设备终生信任。这是用 HSM 最有力的理由，也是永远不要在构建服务器上签启动镜像的最有力理由。

## 7. 没有实时时钟的设备

Raspberry Pi 5 有 RTC，但只有在 `BAT` 接口上接了电池时才能在断电期间保持时间，而这台设备没有电池。
（精简后的模块列表是否仍加载 RTC 驱动，没有检查。）启动记录器的日志显示了 NTP 之前时钟的值：每条早期消息的日期
都是 **2025-06-26 08:44**。这是 systemd 的 `time-epoch`——poky 把它设为 systemd 的可复现构建时间戳
（`SOURCE_DATE_EPOCH` = 1750927453）——而每次开机都从这里开始，因为本该在重启之间保存上次同步时间的文件
（`/var/lib/systemd/timesync/clock`）在只读镜像里位于 tmpfs 上。

证书签发于 2026-09-23。把时钟设成板子开机时的值，测试 RAUC 的证书检查（主机测试，同一个 RAUC 二进制，
通过 `LD_PRELOAD` 替换 `time()`；[记录](../../../results/security/update/clock-without-rtc.txt)）：

| 时钟 | 选项 | 结果 |
| --- | --- | --- |
| 正确 | — | 接受 |
| **2025-06-26（开机，NTP 之前）** | — | **拒绝：`certificate is not yet valid`** |
| 1970 | — | 拒绝：`certificate is not yet valid` |
| 2029（release-1 已过期） | — | 拒绝：`certificate has expired` |
| 2025-06-26 | `use-bundle-signing-time=true` | 接受 |
| **2029** | `use-bundle-signing-time=true` | **接受——过期的密钥照样能用** |
| 2025-06-26，CA + CRL | `check-crl=true` | 拒绝：`CRL is not yet valid` |

所以在这台设备上，NTP 同步之前安装更新会失败，而且错误信息指向的是证书而不是时钟。联网时这只是开机后的一个
竞态；而离线设备（在没有互联网的客户现场插 U 盘更新）就永远没法更新。可选方案及其代价：

- **`use-bundle-signing-time=true`** —— RAUC 按更新包声称的签名时间来检查证书。时钟问题完全解决，但签名时间是
  签名者写的：谁持有泄露或过期的密钥，谁就能把签名时间往前改，过期就没有意义了。那样吊销就成了让密钥退役的唯一办法，
  而 CRL 有它自己的有效期窗口，除非设了这个选项，否则它也是用同一个错误的时钟来检查的（最后一行）。
- **给时钟设一个下限** —— 如果 `/usr/lib/clock-epoch` 的 mtime 比 systemd 内置的 epoch 新，systemd 就绝不会
  从比它更早的时间开始。把这个文件的时间戳打成镜像构建时间，意味着设备永远不会认为现在比它正在运行的镜像更早，
  而镜像构建时间总是晚于给它的前代版本签名的证书。成本很低，而且保留了真正的过期检查。把 timesyncd 的时钟文件
  持久化到 `/data` 上，还会让这个下限随每次同步往前推。
- **一块 RTC 电池** —— 对产品来说是正确答案，代价是多一个零件。
- **安全时间**（来自 TSA 的签名时间戳，或 roughtime）—— 真正解决了"谁说了算现在几点"的问题；需要的基础设施超出本项目所需。

对这台设备的建议是第二种，它让 `check-crl` 和证书过期保持有意义。一般性的结论：**证书检查就是时钟检查**，
而在嵌入式设备上，时钟恰恰是没人去 provision 的那一部分。
