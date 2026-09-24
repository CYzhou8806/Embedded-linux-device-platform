# 嵌入式安全知识地图（中文，学习用）

这份文档有两个用途，别搞混：

1. **地图** —— 把"嵌入式 Linux 平台安全"这片领域摊开，让你知道**有哪些东西、
   它们之间什么关系、自己站在哪**。不求深，求不漏。
2. **复习清单** —— 每个条目后面有状态标记和"你要能回答"的问题。
   按 `docs/notes/learning-qa.md` 的老规矩走：**我提问 → 你复述 →
   复述正确了才归档**，每条独立成 Qn，不打包。

配套：干活的计划在 `private/security-plan.md`（工作块 B0–B9），
这份地图是那份计划的"知识面"，两份对照着看。

**状态标记**：⬜ 没碰过 · 🔨 做过但没复述确认 · ✅ 复述确认过（已进 learning-qa）

---

## 全景：一台联网嵌入式设备的攻击面在哪

先建立骨架。一台设备从上电到联网工作，安全问题分布在**五层加两条横切**：

```
                        ┌─────────────────────────────┐
   横切 A：威胁建模      │  5. 密钥与基础设施           │  谁持有密钥、怎么签、怎么发
   （决定先修哪个）      │     HSM / PKI / provisioning │
                        ├─────────────────────────────┤
                        │  4. 更新与供应链             │  怎么安全地改变上面四层
                        │     签名 OTA / A-B / 防回滚  │
                        ├─────────────────────────────┤
                        │  3. 运行时隔离               │  就算被攻破，能不能限制损失
   横切 B：调试口        │     TEE/OP-TEE, MAC, 容器    │
   （最常被忘掉的后门）  ├─────────────────────────────┤
                        │  2. 系统完整性与机密性       │  文件系统能不能被改、被读
                        │     dm-verity / dm-crypt     │
                        ├─────────────────────────────┤
                        │  1. 启动链                   │  上面全部的地基
                        │     RoT → bootloader → kernel│
                        └─────────────────────────────┘
```

**最重要的一句话**：**上层的安全性依赖下层。** rootfs 加密得再好，
如果启动链没校验，攻击者换个 kernel 就把密钥读走了。所以**顺序是从下往上**，
这也是 `security-plan.md` 里工作块的排序理由。

---

## 第 1 层：启动链（Chain of Trust）

### 1.1 核心概念 🔨（B4，2026-09-23）

**信任链**：每一级代码在把控制权交给下一级之前，先验证下一级的签名。
链条的起点必须是**不可篡改的**——否则攻击者改起点就行了。

```
BootROM (掩膜在芯片里，不可改)  ← Root of Trust
   │ 验签
   ▼
第一级 bootloader
   │ 验签
   ▼
第二级 bootloader (U-Boot / RPi EEPROM bootloader)
   │ 验签
   ▼
kernel + initramfs + device tree
   │ (交给第 2 层：dm-verity 接着管 rootfs)
   ▼
userspace
```

**你要能回答**：
- 为什么 Root of Trust 必须在只读介质里（ROM / OTP）？
- 如果只验 kernel 不验 device tree，攻击者能做什么？
  （提示：DTB 里有 `bootargs`，能改 `init=`、能关掉 verity）
- 签名和加密在启动链里解决的是**不同**问题，分别是什么？

### 1.2 签名 vs 加密（最常混淆的一对） 🔨（B4，2026-09-23）

| | 签名（authenticity + integrity） | 加密（confidentiality） |
| --- | --- | --- |
| 防什么 | 别人**改**你的固件 / 冒充你 | 别人**读**你的固件 |
| 用什么密钥 | 私钥签、**公钥**验（公钥可公开） | 对称密钥，加解密同一把（必须保密） |
| 设备上存什么 | 公钥（或其哈希） | **对称密钥本身**（所以难得多） |
| secure boot 需要 | ✅ 必须 | ❌ 不必须 |

**关键推论**：secure boot **只需要签名**。很多人以为 secure boot 会加密固件，
不是的——固件通常是明文可读的，只是不能被改。想让固件也读不出来（防抄板），
才需要加密，而那要求设备上有个保密的对称密钥，难度陡增。

**你要能回答**：为什么"设备上只需要存公钥"让 secure boot 比固件加密容易得多？

### 1.3 三种平台的具体做法（横向对比） 🔨（B4，2026-09-23）

面试常考，因为它暴露你是"只会一个板子"还是"懂这类问题"。

**A. Raspberry Pi 4/5** —— 我们的平台（B4 按官方 usbboot/rpi-eeprom 核实过，2026-09-23）
- BootROM 验证 RPi 官方签名的 `bootsys`（EEPROM 里的第二级）
- **Pi 5（BCM2712）多一步**：`bootsys` 还必须被**客户私钥副签**（counter-sign）——
  所以连树莓派官方发的 bootloader 更新，没有你签字也装不上。Pi 4 没有这一步。
- `bootconf.txt`（EEPROM 配置）也要客户签名（`bootconf.sig`）
- 你把**整个启动分区**（config.txt、cmdline.txt、DTB、kernel、initramfs、GPU 固件）
  打包成一个 FAT ramdisk `boot.img`，用 RSA-2048 私钥签名，得到 `boot.sig`。
  安全模式下固件**只从 boot.img 里加载东西**——这就是"不验 cmdline 就能加 `init=/bin/sh`"
  那个洞的解法：不一个个签文件，整包签。
- **OTP 里存的是公钥的哈希，不是公钥本身**（公钥 2048 位放 EEPROM，OTP 只"钉住"哪把钥匙算数）
  （🔴 红线，见第 7 节）
- 旧笔记里的 `SIGNED_BOOT=1` 是 Pi 4 时代的配置项，Pi 5 的官方流程是
  `update-pieeprom.sh -f`（副签）+ rpiboot 的 `program_pubkey=1`。
- 特点：方案简单，但**不是通用做法**，别在面试里把它当成 ARM 的标准答案

**B4 实际做下来才知道的（地图原来没写）**：
- **Pi 5 没有"先试后烧"模式**。Pi 4 可以先刷签名 EEPROM 测试再烧 OTP；
  Pi 5 上签名 EEPROM **没烧 OTP 就根本不启动**。唯一可逆能测的是
  `boot_ramdisk=1` 让它从 boot.img 启动——但**非安全模式下不校验签名**，
  所以只能证明镜像格式对，证明不了验签那一步。
- **烧之前最该核对的一件事**：EEPROM 镜像里嵌的公钥的哈希 == 你打算烧的哈希。
  （B4 实测：从生成的 pieeprom.bin 里 `rpi-eeprom-config -x` 抽出 pubkey.bin，
  哈希跟 HSM 里那把一致。）烧错钥匙 = 这块板永远只认那把错钥匙。
- 官方工具原生支持 **HSM wrapper** 接口（`-H`：给文件、吐十六进制签名），
  所以私钥可以从头到尾不落盘——B4 和 B7 在这里接上了。
- 锁定之后 ROM **不再从 SD/eMMC 加载 recovery.bin**，恢复只能走 rpiboot +
  副签过的 recovery.bin —— 这就是"烧之前必须先准备好恢复路径"的原因。
- **上板实测（2026-09-24）：签名的 boot.img 在真机上启动成功**（走一次性 tryboot，失败断电即回滚）。
  两个上板才撞到的坑：
  ① **tryboot 模式下 bootloader 找的是 `tryboot.img`，不是 `boot.img`**（HDMI 诊断屏：
  `Loading tryboot.img ... Error 6`）——**其实官方写了**，只是写在 `autoboot.txt` 那页
  （`tryboot_a_b=1` 就是用来"改读正常的 config.txt/boot.img"的），我只读了 secure boot 那几页。
  教训：一个功能的文档可能散在好几页，**用到哪个开关就去读那个开关所在的页**；
  ② 从 ramdisk 启动时，**第二遍配置解析读的是 ramdisk 里的 `tryboot.txt`**——没有的话
  所有 dtoverlay 都不生效，系统照样起来，但 SPI 采集设备**悄悄消失**。
  而且**第一次失败时我没有控制台，猜是缺 D0 芯片的 overlay——猜错了**，白让用户断电一次。
  没有可观测性的时候不要连续盲猜，先要一个能看到启动过程的手段（HDMI / UART）。
- 这次证明的是"签名镜像格式对、能完整启动"，**不是**"固件会拒绝未签名镜像"——
  没烧 OTP 固件根本不验签，后者只能在锁定过的板子上证明。

**B. Xilinx Zynq-7000**（你手里那块 FPGA 板）
- BootROM 验证 FSBL（First Stage Boot Loader）
- RSA-2048 认证 + AES-256 解密（**同时支持签名和加密**）
- 密钥存 **eFUSE**（一次性，掉电不丢）或 **BBRAM**（电池供电 RAM，可改写、可主动清零）
- **BBRAM vs eFUSE 是个经典取舍**：eFUSE 永久但不可换；BBRAM 可换密钥、
  还能做"检测到开盖就清零"的防拆，但需要电池

**C. 通用 ARM + TF-A**（工业界最常见）
```
BL1 (BootROM) → BL2 → BL31 (EL3 运行时/PSCI) → BL33 (U-Boot) → kernel
                              └→ BL32 (OP-TEE，见第 3 层)
```
- U-Boot 这一级通常用 **FIT image**：把 kernel/DTB/initramfs 打包成一个
  镜像，里面带哈希和签名，U-Boot 用编译进自己的公钥验证
- **这个是面试的标准答案**，Pi 的方案反而是特例

**你要能回答**：
- Pi 5 的 OTP 里存的是什么？为什么不直接存公钥？
- Pi 5 的客户副签（counter-sign）防的是什么？Pi 4 为什么没有？
- 你在没烧 OTP 的 Pi 5 上测"签名镜像能启动"，这个测试证明了什么、没证明什么？
- 为什么 Pi 5 要把整个启动分区打包成一个 boot.img 来签，而不是分别签 kernel 和 DTB？
- TF-A 的 BL31 和 BL32 分别是什么？为什么 OP-TEE 挂在 BL32？
- FIT image 相比"分别签三个文件"好在哪？（提示：原子性、防混搭）

---

## 第 2 层：系统完整性与机密性

### 2.1 dm-verity 🔨（B3 主机 + 上板 case-10；第二轮加了 restart_on_corruption + pstore，见 8.3）

**是什么**：给只读文件系统做**块级**完整性校验。构建时算出一棵
**哈希树（Merkle tree）**：每个数据块算哈希 → 每层哈希再算哈希 → 收敛成
一个**根哈希（root hash）**。运行时每读一个块就验一次。

**为什么是树不是一个大哈希**：一个大哈希得把整个分区读完才能验，开机要等几十秒；
哈希树只需要验**从这个块到根的那条路径**（log n 个哈希），按需验证、随机访问。

**根哈希放哪是关键**：
- 放在被 secure boot 验证过的 kernel 命令行 / DTB 里 → 安全 ✅
- 放在同一个分区里 → **没用**，攻击者连数据带哈希一起改 ❌

**这一条就是"上层依赖下层"的最好例子**：dm-verity 的安全性完全建立在
"根哈希本身不可篡改"上，而那要靠第 1 层。

**B3 实际做下来才知道的**（`security/integrity/verity-tamper-demo.sh`，
在我们自己 188 MB 的真实 rootfs 上跑，不用 root 不用板子）：
- **哈希树很便宜**：4 KiB 块，188 MB 数据 → 树只有 1.46 MB（0.79%），364 个哈希块。
- **普通 ext4 完全不管内容被改**：改 device-service 二进制里 1 个字节，
  debugfs 照样读出来，只是 sha256 变了。
- **定位精确到块**：`Verification failed at position 32735232` = 7992 × 4096，
  正好是被改字节所在的那个 4 KiB 块。（文件系统块是 1024 字节，verity 块是 4096，
  要换算：31968 × 1024 + 100 → 第 7992 个 verity 块。）
- **攻击者自己重算树很容易**，得到另一个根哈希——所以整个方案的安全性
  就压在"根哈希从哪来"上。没有 secure boot 的 dm-verity 只能防**损坏**，防不了**攻击者**。
- **上板（case-10）**：root 直接写 verity 下面的原始分区——**写得进去**；读的时候内核返回 `EIO`，
  日志报的块号 75372 就是主机上算出来的那个。**dm-verity 是读时检测，不是写保护**；
  而且**没有记忆**——把字节写回去它就当什么都没发生过。默认错误模式只是 `EIO`，
  产品上要主动选 `restart_on_corruption` / `panic_on_corruption` 并配合 A/B 回退。
- **构建坑**：wks 里给 rawcopy 的 rootfs 加 `--label`，wic 会把卷标写进 ext4 超级块——
  分区跟哈希树对不上，**构建成功、上板第一次开机就校验失败**。只有逐字节 cmp 能抓到。
- 每次 `veritysetup format` 的根哈希都不一样——因为默认**随机 salt**。
  所以根哈希必须跟着这次构建的产物一起记录，不能"重新算一下对得上"。

**你要能回答**：
- 哈希树验证一个块，要读几个哈希？
- 为什么 dm-verity 只能用于**只读**文件系统？
- 没有 secure boot 的 dm-verity 能防什么、防不了什么？
- 为什么两次对同一个镜像 `veritysetup format`，根哈希不一样？
- 被篡改的块什么时候被发现——挂载时还是读取时？后果是什么？

### 2.2 dm-crypt / LUKS 🔨（B3 上板 + 测速；第二轮加了 integrity，见 8.4）

**是什么**：块设备级加密。LUKS 是 dm-crypt 之上的密钥管理格式
（头部存多个 keyslot，每个 slot 用一个口令解开同一把主密钥 —— 所以能换口令而
不用重新加密整个分区）。

**难点从来不是加密，是密钥放哪**：
| 方案 | 安全性 | 备注 |
| --- | --- | --- |
| 密钥文件放在 rootfs 里 | ❌ 几乎没用 | 物理接触即可读出 |
| 口令由人输入 | ✅ 强 | 无人值守设备做不到（我们的场景就是） |
| TPM 密封到 PCR | ✅ 强 | **绑定到"启动状态"**：改了 bootloader/kernel 就解不开 |
| SoC OTP/eFUSE 派生 | ✅ 强 | 需要 SoC 支持 |
| 外挂安全元件 | ✅ 强 | 加 BOM 成本 |

🔴 **我们平台的诚实结论：Raspberry Pi 5 没有 TPM**，所以这个项目里的 LUKS
密钥无处安放，物理接触就能绕过。**面试要主动说这一点**，并说出正确做法是什么。

**🔄 修正（2026-09-23，B4 查官方资料时发现，上面那句话说得太绝对了）**：
Pi 5 虽然没有 TPM，但**固件里有一个"简化版安全元件"**——`rpi-fw-crypto`
（raspberrypi/utils，2025–2026 年的固件陆续加的）：
- OTP 里可以存一把 **256 位设备私钥**（ECDSA P-256 的 d），`genkey` 可以让固件
  **自己在 OTP 里生成**，密钥从没在外面出现过。⚠️ 写 OTP = 不可逆（不会变砖，
  但那几行 OTP 永久占用），归到"不动"那一类。
- 固件提供 `sign`（ECDSA）和 `hmac`（HMAC-SHA256）——**用密钥但不交出密钥**。
  官方建议的 LUKS 方案：`HMAC(设备密钥, 序列号 + eMMC CID)` 当 LUKS 口令。
- **按键锁到下次重启**：`set-key-status 1 HMAC_LOCKED|READ_LOCKED`。
  完整流程：initramfs（在被签名的 boot.img 里）→ HMAC 派生 LUKS 口令 →
  `cryptsetup open` → **锁 HMAC 和读取** → switch_root。之后就算拿到 root，
  重启之前也派生不出口令了。
- `lock_device_private_key=1` 写在 config.txt 里——**只有开了 secure boot 才可信**，
  因为 config.txt 在被签名的 boot.img 里，否则攻击者删掉这行就行。
- **剩下的洞（官方文档原话）**："It is not possible to prevent code running in ARM
  supervisor mode (e.g. kernel code) from accessing OTP hardware directly."
  → 内核被攻破 = OTP 可读。这就是它和 **TrustZone/OP-TEE 的本质区别**：
  TEE 的安全内存由硬件（TZASC）隔离，连普通世界的内核都读不到。
  另外 dm-crypt 的主密钥解锁后一直在内核内存里（冷启动攻击、内核漏洞都能拿到）。

**所以面试的完整答案**：Pi 5 没 TPM，但有 OTP 设备密钥 + 固件 HMAC + 重启前锁定；
配合 secure boot 能挡住"拔卡拿走"和"启动后普通 root 进程"，
挡不住"内核被攻破"——要挡那个得有 TrustZone 隔离或独立安全元件。

**上板真数据（2026-09-24）**：LUKS2 数据分区首次启动自动创建；读吞吐开销 **7%**（`xts-aes-ce`）。
dm-verity 开销 **27% → 16%**：verity 在 initramfs 里建立，那时模块还没加载，绑定的是纯软件的
`sha256-generic`，之后也不会换——把 `CONFIG_CRYPTO_SHA2_ARM64_CE` 编进内核才用上硬件指令。
（密钥仍是序列号派生的替身，真方案是 rpi-fw-crypto + OTP。）

**你要能回答**：
- TPM 的 PCR sealing 是什么意思？它和 secure boot 怎么配合？
- Pi 5 上用 `rpi-fw-crypto hmac` 派生 LUKS 口令，为什么派生完要立刻锁 HMAC？
  为什么 `lock_device_private_key=1` 不开 secure boot 就没意义？
- 同样是"密钥不出门"，Pi 5 固件 HMAC 和 OP-TEE TA 差在哪？（提示：内核能不能碰到）
- LUKS 为什么能"换口令而不重新加密整盘"？
- 加密对吞吐/延迟的代价，你打算怎么量？（你有现成方法论）

### 2.3 dm-verity vs dm-crypt vs fs-verity 🔨（B3，2026-09-24，布局已上板）

| | 保护什么 | 粒度 | 读写 |
| --- | --- | --- | --- |
| dm-verity | 完整性（不能改） | 块 | 只读 |
| dm-crypt | 机密性（不能读） | 块 | 读写 |
| fs-verity | 完整性 | **单个文件** | 文件只读，文件系统可写 |

典型组合：**rootfs 用 dm-verity（只读、不可改）+ 数据分区用 dm-crypt（可写、加密）**。
**已经在 Pi 上跑起来了**（device-platform-image-ab：p5/p6 verity、p7 LUKS2）。

**做下来才知道的一个细节**：dm-crypt **不提供完整性**——改密文某一块，解出来是垃圾，
文件系统照读不误（除非叠加 dm-integrity 做认证加密）。所以"加密了"不等于"防篡改"，
rootfs 要防篡改用的是 verity，不是 crypt。

**你要能回答**：dm-crypt 加密的分区，攻击者改了一块密文，会发生什么？

---

## 第 3 层：运行时隔离与 TEE

### 3.1 TrustZone 的基本模型 🔨（B5，概念，QEMU 上没法亲眼看到硬件隔离）

ARM TrustZone 把系统劈成两个世界：**Normal World (REE)** 和
**Secure World (TEE)**，靠硬件隔离：
- CPU 有个 **NS bit**（Non-Secure），标记当前处于哪个世界
- **TZASC** 管内存分区、**TZPC** 管外设归属——安全世界能访问全部，
  普通世界**在硬件上**访问不到安全世界的内存
- 两个世界靠 **SMC 指令**（Secure Monitor Call）切换，EL3 的 monitor 负责转发

**关键理解**：这不是软件权限（root 也不行），是**总线级的硬件隔离**。

### 3.2 OP-TEE 🔨（B5，2026-09-24，`docs/security/optee.md`）

OP-TEE 是安全世界里跑的一个小 OS（在 TF-A 里作为 BL32 加载）。

```
Normal World                    │  Secure World
                                │
你的应用                        │   TA (Trusted Application)
  │ libteec (客户端库)          │     ▲
  ▼                             │     │
/dev/tee0 (内核 tee 驱动)       │     │
  │                             │     │
  └──── SMC ──► EL3 monitor ────┴─────┘
```

**一次调用的完整路径**（面试爱问）：
应用调 `TEEC_InvokeCommand` → libteec → ioctl 到 tee 驱动 → 驱动发 SMC →
EL3 monitor 切世界 → OP-TEE 内核 → 找到对应 TA → TA 执行 → 原路返回。

**什么适合放进 TA**：
- ✅ 密钥的保管和使用（密钥**永不出 TEE**，只输出运算结果）
- ✅ 设备身份、签名/验签、安全存储
- ❌ 大数据量处理（每次世界切换有开销）
- ❌ 需要复杂库的逻辑（TA 环境很受限，而且 **TA 自己有漏洞就全完了**）

**Secure Storage 落在哪**：
- **RPMB**（Replay Protected Memory Block，eMMC 的一个特殊分区）—— 有**防回放**
  保护，是正经做法
- 或者加密后存普通文件系统 —— 简单，但**防不住回放攻击**（攻击者把旧的
  加密文件复制回去）

🔴 **诚实结论：OP-TEE 没有 Raspberry Pi 5 的移植**（RPi3 那个是不安全的
演示且早已不维护）。所以我们在 **QEMU armv8** 上做。面试要主动讲这个判断。

**B5 实际做下来才知道的（QEMU 上真跑出来的，结果在 `results/security/optee/`）**：
- **回放攻击不是理论**：把 `/var/lib/tee` 恢复成"还没 provision 时"的旧副本，
  OP-TEE 照单全收（报"没有密钥"），然后 TA 的"只能 provision 一次"规则被绕过，
  **生成了第二个不同的设备身份**。结论：**TEE 通过存储执行的任何策略，
  强度都不超过存储的防回滚能力**。OP-TEE 开机日志自己就写了：
  `WARNING (insecure configuration): Failed to get monotonic counter for REE FS, using 0`。
- **篡改能检测，但变成了拒绝服务**：每个存储文件翻一个字节 →
  `TEE_ERROR_CORRUPT_OBJECT`，连会话都开不了；OP-TEE 会删掉它认为损坏的文件。
  完整性守住了（伪造不进去），**可用性没守住**（普通世界 root 能毁掉设备身份）。
- **"不可导出"是 TEE 内核强制的，不是 TA 自觉**：TA 自己调
  `TEE_GetObjectBufferAttribute` 读私钥 → GP 规范要求 panic，
  日志 `TA panicked with code 0xffff0006`，客户端收到 `0xffff3024` TARGET_DEAD；
  TA 重新加载后密钥还在。
- **QEMU 上的 HUK（硬件唯一密钥）是全 0 常量**（`CFG_INSECURE` 桩函数）——
  secure storage 的加密密钥都从 HUK 派生，所以 QEMU 上的"加密存储"只演示流程，
  不提供真保密。真板子上 HUK 来自熔丝/OTP，只有安全世界能读。
- **参数在共享内存里**：TA 收到的 memref 指向普通世界还能继续改的内存 →
  先拷一份再校验再用（防 double fetch / TOCTOU）。
- **TA 不签调用者给的任意摘要**：签的是 `SHA-256(固定域名串 ‖ nonce)`。
  普通世界的 root 能调用 TA（这挡不住），但只能用它"证明我是这台设备"，
  不能拿它当通用签名机去签更新包或证书请求。
- 构建：OP-TEE 4.10.0 官方 manifest，xtest **143 个用例 / 41358 个子测试全过**，
  QEMU 里跑 11 分钟。Ubuntu 24.04 缺 gnutls 头文件导致 U-Boot 的 `mkeficapsule`
  编不过，用 config fragment 关掉即可。
- **跟 Pi 5 的对照**：Pi 5 固件的 `rpi-fw-crypto` 也是"用密钥不交出密钥"的接口，
  但官方明说**内核能直接读 OTP**；TrustZone 连普通世界内核都挡在外面。

**你要能回答**：
- NS bit / TZASC / TZPC 各管什么？
- 为什么说"TEE 也不是万能的"？（侧信道、TA 漏洞、安全世界代码量越大越危险）
- RPMB 防的"回放攻击"具体是什么场景？（**你亲手做过**：恢复旧副本 → 第二个身份）
- 为什么 TA 要先把 memref 里的数据拷一份再用？
- 你的 TA 为什么不直接签调用者传进来的哈希？
- 篡改 secure storage 文件，OP-TEE 能发现，那攻击者还能得到什么？

### 3.3 其他隔离手段 🔨（B2，2026-09-24，systemd 沙箱 + 专用用户，上板）
- **MAC（强制访问控制）**：SELinux / AppArmor / Smack —— 就算进程被攻破，
  也只能做策略允许的事。`meta-security` 里有。
- **容器 / namespace**：隔离粒度粗，但便宜。
- **seccomp**：限制进程能调哪些系统调用。

**B2 实际做了（不是 MAC，是 systemd 自带的那一套，成本最低）**：
device-service 从 root 改成专用用户 `acq`，udev 规则**只**放开它要写的两个 sysfs 属性；
systemd drop-in 加上 `CapabilityBoundingSet`（只剩 2 个能力）、`ProtectSystem=strict`、
`PrivateNetwork`、`SystemCallFilter=@system-service`（seccomp）、`DeviceAllow` 只允许 `/dev/acq0`。
`systemd-analyze security` 从 9.4 降到 1.8；上板确认进程 `CapEff=0x804000`、`Seccomp: 2`、独立网络命名空间。
**反直觉的一点**：`ProtectKernelTunables` 不能开——它会让 `/sys` 只读，背压就写不了 `sample_rate` 了。
通用加固清单照抄会把功能弄坏，每一条都要对着"这个服务到底要什么"判断。

**你要能回答**：
- 没有 SELinux 的时候，你用什么限制一个服务被攻破后的损失？
- 为什么以 root 运行但去掉所有能力，比以 root 运行安全？还剩什么风险？

---

## 第 4 层：更新与供应链

### 4.1 为什么更新机制本身是安全功能 🔨（B1 的 F6：没有更新机制 = 其他所有发现在现场永久存在）

**没有安全更新能力 = 任何漏洞都是永久漏洞。** 这是最容易被低估的一层。
我们当前的镜像**完全没有更新机制**（B1 审计的重点发现之一）。

### 4.2 A/B 双槽更新 🔨（B7，2026-09-24，**上板全流程验证**）

两套完整系统分区轮流用：运行 A 的时候把新版写进 B，写完切换启动标记，
重启进 B；**B 启动失败则自动退回 A**。

- 解决的核心问题：**断电原子性**。就地升级（in-place）在写到一半断电时，
  系统处于"半新半旧"的不可启动状态
- 代价：**存储翻倍**
- 我们项目里的类比：你已经用过 Pi 的 **一次性 tryboot** 做内核实验
  （`experiments/rt-kernel/`）——那本质上就是 A/B 的思路：
  **试新的，失败自动回到已知可用的**

**工具**：RAUC、SWUpdate、Mender。Yocto 里有 `meta-rauc`。

**B7 实际做下来才知道的**（`docs/security/update-and-provisioning.md`）：
- **构建签名和发布签名分开**：BitBake 用开发密钥签 bundle；签名主机用 HSM 里的
  发布密钥 `rauc resign`。构建服务器永远碰不到发布密钥；设备的 keyring 只有
  Update CA，所以**开发包装不进产线设备**（实测：`unable to get local issuer certificate`）。
  顺带一个坑：把 HSM 的 PIN 喂进 BitBake 会进构建元数据/日志，这也是要分开签的原因之一。
- **verity 格式下，签名有效 ≠ payload 完好**：签名只覆盖 manifest，manifest 里有
  payload 的 dm-verity 根哈希；payload 是 `rauc install` 挂载时内核**逐块**校验的。
  实测：payload 改 1 字节，`rauc info` 照样通过；用 veritysetup 按签名里的根哈希一验，
  精确报出被改的 4 KiB 块。（跟 B3 的 dm-verity 是同一个机制，换了个地方用。）
- RAUC 默认检查证书用途，`codeSigning` 证书会被拒（`unsuitable certificate purpose`），
  设备的 system.conf 要写 `[keyring] check-purpose=codesign`。
- **上板实测**：坏更新（新槽里 SSH 起不来）→ **断一次电自动回到 A**，`[all]` 从没被改；好更新 →
  tryboot 进 B → 30 秒后健康检查 `rauc status mark-good` → 后端把 `[all]` 改成 B → 正常重启留在 B。
  0.9.0 被 `min-bundle-version` 拒掉。
- **健康检查是整个方案的闸门，而它差点形同虚设**：它 `WantedBy=multi-user.target` 又排在
  device-service 之后，而 device-service 自己 `After=multi-user.target`——依赖环，systemd 每次开机
  **静默删掉这个 job**，只留一行日志。失败方向是安全的（永远不确认），但闸门根本不存在。改成定时器触发。
  教训：**安全机制要验证"它真的跑了"，不是只验证"它跑的时候对"**。
- meta-rauc 自带的 `rauc-mark-good` 会无条件把每个启动的槽标 good——装了它健康检查就没意义，要排除掉。
- **Pi 5 上 meta-rauc-community 的例子要换成 U-Boot**，跟 Pi 5 secure boot（固件直接验
  boot.img）和我们已经在用的 tryboot 冲突。设计改为 RAUC `custom` 后端 + 固件自带的
  `autoboot.txt` + 一次性 `tryboot`：新系统没确认健康之前，任何重启都自动回旧槽。

### 4.3 防回滚（anti-rollback）🔨（B7，min-bundle-version 已上板；第二轮加了吊销和时钟，见 8.6、8.7）

**光有签名不够**：攻击者可以装一个**你自己签过的、有已知漏洞的旧版本**——
签名是有效的，但系统被降级到可攻击状态。

解法：单调递增的版本计数器，且计数器要存在**攻击者改不了的地方**
（OTP 计数器 / TEE 里 / RPMB）。

**B7 查到的**：RAUC 1.15 自带 `[system] min-bundle-version`（system.conf），
比旧版本号低的 bundle 直接拒。但两个坑（官方文档/API 里写着的）：
① 真要回滚一个坏版本，得把旧内容**换个更高的版本号**重新发；
② D-Bus 安装接口有 `ignore-version-limit` 参数——谁能调 RAUC 的 D-Bus 必须限制，
否则 root 直接让它跳过检查。而且这个限制存在 rootfs 里，只防"走更新通道"的降级，
**拿着 SD 卡直接写旧镜像的人**得靠攻击者改不了的计数器（OTP 位 / RPMB / TPM）。
跟 B5 的 OP-TEE 回滚实验是同一个道理：**存在可回滚存储里的规则，都能被回滚掉**。

**你要能回答**：
- 为什么防回滚的计数器不能存在普通文件系统里？
- RAUC 的 `min-bundle-version` 挡得住什么、挡不住什么？
- 一个版本出了严重 bug 要退回去，有了防回滚之后怎么操作？

### 4.4 SBOM 与 CVE 管理 🔨（B2，2026-09-24，`docs/security/hardening.md` §4）

- **SBOM**（软件物料清单）：镜像里到底有哪些组件、什么版本。
  没有它就无法回答"log4shell 爆了，我们受影响吗"
- **Yocto 自带 `cve-check`**：`INHERIT += "cve-check"`，出整镜像的 CVE 报告
- **分诊**才是真功夫：报出来一百条，哪些是误报、哪些组件我们根本没启用那个功能、
  哪些必须升级。Yocto 里用 `CVE_STATUS` 标注理由——**这个标注过程本身是交付物**

**B2 实际做下来才知道的（真数据，prod 镜像）**：
- NVD 数据库第一次下载：378,490 条，**49 分钟**（没 API key 被限速）。
  scarthgap **默认就生成 SPDX 2.2 SBOM**，不用额外开。
- 原始报告：**3,869 条未修复**，其中 **3,853 条是内核**，用户态只有 16 条。
- **用户态 16 → 4，每条都有证据**：openssh 6 条全是 ssh 客户端/agent/sshd 的洞，
  镜像里只有 `openssh-sftp-server`（服务端是 dropbear）；expat 6 条的唯一使用者是
  dbus-daemon，读的是只读 rootfs 上 root 的配置；glibc 那条 9.8 分的要 `scanf("%1025mc")`，
  **全镜像没有一个 ELF 含 `%mc` 格式串**。写成 `CVE_STATUS` 进 bbappend。
  **CVE_STATUS 的内在矛盾**：这是"这个镜像装了什么"的事实，却记在"配方"上——
  哪天有人加了 ssh 客户端，openssh 那几条就错了。所以理由字符串里写明证据。
- **内核才是真问题，而且答案不是逐条分析**：
  - poky 自带 `improve_kernel_cve_report.py`（用内核 CNA 自己的 vulns.git + 实际编译的文件列表）。
    在 scarthgap + linux-raspberrypi 上要绕三个坑：版本号带 epoch（`1_6.6.63+git`）、
    旧 JSON 缺 `detail` 字段、**`SPDX_INCLUDE_COMPILED_SOURCES` 对这个内核不生效**
    （列出了整棵树 33,155 个 .c，连 AMD 显卡驱动都在，实际只编了 7,048 个 .o）。
  - 用 .o 反推真实编译列表后：**2,875 条因为代码根本没编进来被排除**（原来那个列表只排除了 90 条）。
  - 剩下 3,602 条里 **1,102 条只要跟到 6.6 最新 stable（6.6.157）就修了**——
    meta-raspberrypi scarthgap 钉在 6.6.63（2024-12），落后 94 个 stable 版本。
- **结论（面试答案）**：内核 CVE 的处理是**流程**（跟 stable、重建、跑回归），
  不是逐条分析；逐条只留给 stable 没覆盖的那几百条。而且"排除没编译的代码"
  必须用**真实**编译列表，否则工具给你一个看起来很专业但几乎没过滤的结果。

**B2 上板才发现的（跟 CVE 无关，记在这里是因为同属"镜像该怎么出"）**：
只读 rootfs 下 `/etc/machine-id` 每次开机重新生成 → systemd-networkd 的 DHCP 客户端标识
（DUID）由 machine-id 派生 → **每次开机路由器给一个新 IP**（实测 .177 → .178）。
加上 SSH host key 每次也变，这台设备在网络上"每次开机都是一台新机器"——
设备身份类的东西（machine-id、host key、设备密钥）**都得在产线生成、放持久分区**，
不能在镜像里（全都一样）也不能每次开机现生成（每次都不一样）。

**你要能回答**：
- 只读 rootfs 的设备，为什么每次开机 IP 都变？怎么修？
- `cve-check` 报了 100 条（我们是 3,869 条），你的处理流程是什么？
- 为什么内核 CVE 要按"实际编译了哪些文件"过滤？这个列表从哪来、怎么验证它是对的？
- `CVE_STATUS` 记在配方上，有什么隐患？
- 为什么说"跟 stable 分支"比"逐条打补丁"更重要？


### 4.5 SDK（不是安全，但岗位名里写着 "BSP/SDK"）🔨（B8，2026-09-24）

- `populate_sdk`：交叉工具链 + **跟镜像一模一样的 sysroot**（同一个 glibc、同一套库版本、
  同一套编译加固参数）。`populate_sdk_ext`（eSDK）多带 BitBake 环境和 sstate，
  可以用 `devtool modify / build / deploy-target / finish` 改配方——给"改配方但没有
  完整 Yocto 环境"的人用。
- **坑 1**：SDK 的 sysroot **只含镜像里装了的包的 -dev**。header-only 的 nlohmann-json、
  只在测试用的 gtest 从来不进镜像，所以第一版 SDK 编不了 device-service。
  `TOOLCHAIN_TARGET_TASK:append` 把纯构建依赖加进去。
- **坑 2**：`gtest_discover_tests()` 默认**在构建时执行测试二进制**来列出用例——
  交叉编译时那是个 aarch64 程序跑在 x86 上。`DISCOVERY_MODE PRE_TEST` 推到 ctest 时再做。
- **不上板也能跑交叉编译的测试**：Yocto 自带的 `qemu-native` 里有 `qemu-aarch64`，
  `qemu-aarch64 -L <sdk 的 target sysroot> ./tests` → 16/16 + 28/28 通过。
- 数字：SDK 首次 49 分钟，改内容后 5.6 分钟；安装包 254 MB，装开 1.7 GB。

**你要能回答**：
- SDK 和 eSDK 分别给谁用？devtool 解决什么问题？
- 为什么用 SDK 编出来的程序比用"自己凑的 sysroot / 容器"编出来的更可靠？
- 交叉编译的单元测试，不上板怎么跑？

---

## 第 5 层：密钥与基础设施

### 5.1 密钥的生命周期 🔨（B7，2026-09-24）

生成 → 存储 → 使用 → 轮换 → 撤销 → 销毁。**大多数人只想到前三步。**

**你要能回答**：
- 签名私钥泄露了怎么办？你的设计里能不能换密钥？换了之后**已经出厂的设备**
  怎么办？（这题很多人答不上来——答案通常是：设备里要预置多个信任锚，
  或者保留一条"用旧密钥签一个换新密钥的更新包"的路径）
- 每台设备的唯一身份在产线上怎么注入？谁能看到这些密钥？

**B4/B7 实际做的**：token 里四把钥匙、四个用途（boot 签名 / Device CA / Update CA /
发布签名），**一把泄露只影响一件事**。更新签名做成两级：设备信 Update CA，
不直接信签名密钥——签名密钥泄露或到期，CA 签一张 `release-2` 证书就换了，
**出厂设备不用改 keyring**。对比：Pi 5 的 boot 密钥没有这层间接，OTP 里只钉一把钥匙的哈希，
所以它丢了/泄露了后果严重得多。
**自己踩的坑**：第一版脚本把 PIN 放命令行参数（`ps` 谁都能看见），写 Device CA 脚本时发现，
全部改成 env / 0600 配置文件。

**你要能回答**：
- 为什么签名密钥要挂在一个 CA 下面，而不是让设备直接信这把签名密钥？
- 为什么 boot 签名密钥和更新签名密钥不能是同一把？

### 5.2 HSM 与 PKCS#11 🔨（B4 boot 签名 + B7 更新包签名 / Device CA，2026-09-24）

**HSM 解决的问题**：私钥**永远不离开硬件**。签名不是"把密钥拿出来用"，
而是"把待签数据送进去、把签名结果取出来"。私钥物理上无法导出。

**PKCS#11** 是访问它的标准接口。好处：开发时用 **SoftHSM2**（软件实现），
上产线换成真 HSM，**接口不变**。我们 B7 就这么做。

**B4 实际做下来才知道的**：
- 密钥是在 token **里面生成**的，`pkcs11-tool` 显示它的属性是
  `sensitive, always sensitive, never extractable, local`——
  `never extractable` 是 token 强制执行的（没有任何 PKCS#11 调用能把它拿出来），
  `local` 说明它是在 token 里生成的、不是导入的（导入的密钥曾经以文件形式存在过）。
- 生成密钥对时加 `--private`，**公钥对象也会变成要登录才能看**——导出公钥时忘了
  `--login` 会报 "object not found"，很迷惑。
- PIN 不能放命令行参数（`ps` 和 shell 历史都看得见），从 0600 文件读。
- 签名走 `CKM_SHA256_RSA_PKCS`：数据送进去、签名出来，哈希和签名都在 token 里算。
- 每次签名记审计日志（时间、谁、签了哪个哈希）——真 HSM 自带，SoftHSM 没有，得自己补。
- 换真 HSM 只改 `PKCS11_MODULE` 路径和 token 名，脚本不动——这就是 PKCS#11 的价值。

**你要能回答**：
- 为什么"把签名密钥放在 CI 的环境变量里"是个坏主意，就算那个变量是加密的？
- `never extractable` 和 `local` 这两个属性分别证明了什么？

### 5.3 Provisioning（产线注入）🔨（B7，2026-09-24，`tools/provision-device.sh`，用测试替身验证）

每台设备需要：唯一序列号、设备私钥/证书、可能还有对称密钥。
要考虑：谁生成、怎么写入、**怎么审计**（哪台设备什么时候被注入了什么）、
产线人员能不能偷走密钥。

**B7 实际做下来才知道的**：
- **密钥在设备里生成**（B5 的 TA），产线工站只拿到公钥——工站根本碰不到设备私钥，
  "产线人员偷密钥"这个问题从结构上就没了。
- **持有证明（proof of possession）**：常规做法是让设备出 CSR，但我们的 TA **故意**
  只签域隔离的身份挑战、不签任意数据——所以改成工站发 nonce、设备签、工站用上报的公钥验。
  "TA 不当通用签名机"这个安全设计，直接影响了产线流程怎么写。
- 拒绝的三种情况都测了：同一序列号第二次、设备已有我们没发过的身份、设备证明不了持有私钥。
- 审计记录：时间、工站、操作员、序列号、公钥指纹、证书序列号和指纹、CA 指纹，一行一条 JSON。

**你要能回答**：
- 为什么设备私钥应该在设备里生成，而不是工站生成后写进去？
- 设备没法出 CSR 的时候，工站怎么确认它真的持有那把私钥？

---

## 横切 A：威胁建模 🔨（B1，2026-09-23，`docs/security/threat-model.md`）

**没有威胁模型，安全工作就是买彩票。** 它回答"先修哪个"。

四个问题：
1. **保护什么**（资产）：数据、固件、密钥、设备身份、可用性
2. **防谁**（攻击者及其能力）：远程网络攻击者 / **能接触设备的服务技师** /
   供应链 / 二手设备买家
3. **边界在哪**（信任边界）：MCU↔SPI↔Pi↔网络↔云；启动链；更新通道；调试口
4. **怎么排序**：可利用性 × 影响

**我们的场景（对应 RATIONAL 的产品）**：商用厨房设备，**放在客户后厨、
由第三方技师维护、联网上传数据、会被转售**。这个场景决定了
"物理接触"不是极端假设，而是**日常情况**——所以调试口和数据加密的优先级要往上提。

**B1 实际做下来才知道的**：
- **排序不等于严重度排序**。审计出 14 条（F1–F14），修的顺序是
  "严重度 × 依赖 × 成本"：F1（远程 root 空密码）最先修，不是因为它最有技术含量，
  而是它一天不修，其他所有防护都没意义；secure boot（F4）严重度高但排在
  dm-verity 后面，因为最后一步（烧 OTP）不可逆、要单独决策。
- **有依赖关系的防护要成对讲**：dm-verity 的根哈希只有放在**被签名的** boot.img
  （cmdline）里才可信；secure boot 只验到 kernel+initramfs 为止，rootfs 得靠 dm-verity 接力。
  单讲任何一个都是半截。
- **"不是问题"也要写**：审计时查了驱动的 sysfs 输入校验（sample_rate 有范围检查、
  control 只取 1 位、read() 只交整条样本），写进报告说明"查过、没问题"——
  这证明审计是查过的，不是只列自己知道的坑。
- 真实发现的细节：devbus 的订阅者以读写方式映射共享内存，一个被攻破的订阅者能改
  其他订阅者正在读的数据（F13）。发布者已经把订阅者写回的索引当不可信输入处理，
  但 payload 本身没保护——零拷贝的代价。

**你要能回答**：
- 你的 14 条发现，为什么修的顺序不是严重度从高到低？
- dm-verity 和 secure boot 谁依赖谁？只做一个会留下什么洞？

---

## 横切 B：调试口（最常被忘掉的后门）🔨（B2/B6，2026-09-24）

**一个开着的串口 root shell，让上面五层的努力全部归零。**

我们当前镜像的状态就是反面教材：
```
EXTRA_IMAGE_FEATURES += "debug-tweaks ssh-server-dropbear"
                          ▲
                          └─ root 可登录、空密码、串口控制台开着
```

要处理的：串口控制台、SSH、JTAG/SWD、U-Boot 的交互式命令行
（**能改 bootargs 就等于能绕过一切**）。

**真正的难点是"开发要方便、产线要锁死"这个矛盾**，标准解法是
**开发镜像 / 生产镜像分开**（两个 image recipe）——这就是 BSP 工程师的日常。

**这个项目里三个调试口，都亲手处理过**：
- **串口（Pi 5 的 UART 口）**：产线镜像把 `serial-getty@ttyAMA10` 屏蔽掉（`SERIAL_CONSOLES` 是机器级
  设置，开发镜像还要用，所以只在产线镜像里屏蔽）。上板确认 `masked`。
- **SWD（STM32）**：RDP0 时调试器直接读出了认证密钥；RDP1 挡住了读，但调试器一接上被保护的芯片
  它就停摆（拒绝服务），而且 F1 的 RDP1 有公开绕过（CVE-2020-8004）。
- **VideoCore JTAG（Pi 5）**：只能在烧了 OTP 公钥之后用 `program_jtag_lock` 永久关掉——没烧，写进了文档。
- 另一个**调试口视角的收获**：HDMI 诊断屏和 bootloader 日志是这次排查 tryboot 失败的唯一线索。
  **调试口既是后门也是救命绳**——产线上锁死之前，要先想好坏了之后怎么诊断（我们的答案：
  启动记录器把日志写到 FAT 分区，不需要任何调试口）。

**你要能回答**：产线设备把调试口都锁了，现场出了启动故障你怎么诊断？

---

## 🔴 第 7 节：三条不可逆红线（只讲原理，不碰硬件）

你要求"告诉我大概怎么回事就行"，这节就是。**这三个操作一旦做了没法回头**，
我们的项目里一律只写文档。

### 7.1 OTP / eFUSE —— 一次性可编程熔丝

**物理原理**：芯片里有一排微型熔丝（或反熔丝）。写 1 的时候给大电流**烧断**
（或击穿）——这是**物理的、不可逆的**变化。所以：
- 0 → 1 可以
- **1 → 0 不可能**，没有任何软件手段能恢复

**用来存什么**：公钥哈希（不是公钥本身，省空间）、设备唯一 ID、
使能位（比如"从此只接受签名镜像"）、防回滚计数器。

**为什么危险**：
- 烧错公钥哈希 → 你签的镜像验不过 → **板子再也启动不了，变砖**
- 烧了"强制签名启动"的使能位 → 从此只能跑你签名的镜像，开发流程全变

**产线上怎么做才安全**（这才是面试答案）：
1. 先在**非强制模式**下验证整条签名链能通过（签名做了、验证器认了，
   但失败不阻止启动）
2. 备份并**多人复核**公钥哈希
3. 小批量试烧，验证后再全量
4. 保留恢复路径（可写的 recovery 介质、JTAG 恢复、厂返流程）

### 7.2 STM32 RDP（读保护）Level 2

| 级别 | 效果 | 可逆？ |
| --- | --- | --- |
| Level 0 | 无保护，随便读写 | — |
| **Level 1** | 调试器连上后**读不到 flash**；降级到 L0 会**触发整片擦除** | ✅ 可逆（代价是擦掉固件） |
| **Level 2** | **调试接口永久关闭** | ❌ **完全不可逆** |

Level 1 的设计很聪明：**降级 = 擦除**，所以攻击者能解保护，但解开时代码已经没了。

Level 2 把 SWD/JTAG 从硬件上关死，**芯片再也连不上调试器**——
固件出问题只能靠已有的 bootloader 升级，没有 bootloader 就是废片。

🔴 **我们只有一块 MCU，绝对不做 Level 2。** 要演示读保护就用 Level 1。

**⚠️ B6 设计时查到的（2026-09-24）：Level 1 在 F1 系列上已经被公开攻破了。**
Schink & Obermaier 2020，《Exception(al) Failure — Breaking the STM32F1 Read-Out
Protection》，CVE-2020-8004：Level 1 挡住了调试器直接读 flash，但 **CPU 进异常时
取中断向量走的是 ICode 总线，照样能读 flash**——通过调试口反复触发异常，
一点点把 flash 内容"借"出来。所以"降级即擦除"那个巧妙设计被绕过了：
**不用降级，直接读**。结论：F103 上把密钥放 flash + RDP1，只是提高门槛，
**不是安全边界**。面试说 RDP 的时候要带上这一条，不然会被问住。

**B6 上板亲手做了 RDP1（2026-09-24）**：
- 保护前：调试器直接 `mdw 0x0807f800` 读出了认证密钥——威胁模型 F10 当场演示。
- 打开 Level 1：同样的读取 `Failed to read memory`；设备照常工作、认证照常通过——
  **但前提是给 MCU 断一次电（POR）**：打开保护时调试器连着，flash 对 CPU 也锁住，按 reset 不够（RM0008）。
- **调试器再接上被保护的芯片，它就停摆了**——读不出密钥，但能让设备拒绝服务。
- 解除保护：整片擦除，密钥页和固件都变 `ffffffff`——"降级即擦除"亲眼看到了。
- 所以对 F103 的完整说法：RDP1 挡住"随手接调试器读"，挡不住 CVE-2020-8004，
  还会被调试器拿来做拒绝服务；密钥真要保护得靠外挂安全元件或带 TrustZone-M 的 MCU。

**B6 的其他真数据**：HMAC-SHA256 在 72 MHz 的 Cortex-M3 上 **487 µs**（35,062 周期），
采集进行中 561–580 µs——MAC 放在主循环、被 SPI/定时器中断抢占，**认证给采集让路**；
1000 Hz 采集中连续 100 次认证，零丢样、零 SPI 错误。

**你要能回答**：
- STM32F1 的 RDP Level 1 为什么挡不住有调试探针的攻击者？
- 为什么 MAC 要在主循环里算、不在 SPI 中断里算？你怎么证明它没影响采集？
- 为什么每颗 MCU 一把派生密钥，而不是全部设备共用一把？

### 7.3 Zynq eFUSE vs BBRAM

- **eFUSE**：同 7.1，永久
- **BBRAM**：电池供电的 RAM 存 AES 密钥，**可改写、可主动清零**。
  好处是支持**防拆**（检测到开盖就清密钥，固件立刻变成不可解密的砖）和密钥轮换；
  代价是需要电池，电池没电密钥就丢了

**取舍题**：永久但不可换 vs 可换但依赖电池——面试可能会问你选哪个、为什么。

---

## 第 8 节：第二轮 + 后续新增的知识点（2026-09-24 晚）🔨

第一轮每块只做到"能讲"；第二轮把它们串起来并上板，下面是**只有做了才知道**的东西。
每条后面是"你要能回答"的问题。证据全在 `private/security-review-guide.md` §4 列的文件里。

### 8.1 签名 boot.img 进 A/B 两个槽 🔨
- 构建端不碰 boot 密钥；签名主机把启动分区打包成 ramdisk、HSM 签名、写进 p2/p3、再做 bundle。
- ramdisk 里一份 config.txt 用 `[boot_partition=2/3]` 选 cmdline，**这个过滤在 ramdisk 里也生效**（实测，无文档）。
- 默认 cmdline 指向 A 槽：万一过滤不生效，B 启动会校验失败、回退，而不是开不了机。
- **要能回答**：信任链现在从哪到哪？缺的是哪一环（固件强制验签 = 烧 OTP）？为什么签名要在构建之外做？

### 8.2 内核：编进 ≠ 启用 🔨
- Pi defconfig `CONFIG_LSM=""` → 只有 capability，Yama/AppArmor 编了也没启用；`/sys/kernel/security/lsm` 读回才发现。
- FORTIFY_SOURCE 第一次开机就在 brcmfmac 抓到 field-spanning memcpy（`u8 mask[1]`，误报，只警告）。
- MODULE_SIG_ALL 连树外模块也签；看到都被接受后才开 MODULE_SIG_FORCE。
- BTI/PAC：编译器标了，A76（Armv8.2）不支持 → NOP。
- **要能回答**：为什么先 SIG_ALL 再 SIG_FORCE？为什么产线内核应该另编一个（ftrace/kprobes/lockdown）？
  sysctl 里"key 不存在"的三种含义？

### 8.3 dm-verity 出错模式 + pstore 🔨
- EIO（默认）：服务反复崩、系统继续跑；restart：立即重启；panic：看 panic= 参数。
- restart + 一次性 tryboot = 坏更新自动回退（实测 14 秒回 B）；**已确认的槽**坏了会无限重启。
- ramoops：保留上一次启动的内核日志；Pi 5 要自己写 overlay（地址 cell 数不同）。抓到 `data block 61755 is corrupted`。
- **要能回答**：三种模式各适合什么场景？为什么证据要在"失败的那个镜像"里，而不是"恢复的那个"？

### 8.4 LUKS2 + dm-integrity 🔨
- 普通 XTS 只保密不防篡改（改密文 → 解出垃圾，文件系统照读）；加 HMAC 后改 1 字节 → I/O error。
- 代价：读 −24%、写 ≈ −50%（日志模式写两遍，换断电一致性）。
- 防篡改但**不防回放**（旧扇区+旧标签照样验得过）→ 要计数器。
- **要能回答**：authenc(hmac(sha256),xts(aes)) 的两把钥匙从哪来？为什么写代价是一半？

### 8.5 CVE：NVD vs 内核 CNA 🔨
- 6.12 上 NVD 版本匹配只有 782，CNA 数据后 1,789：NVD 对新 CVE 补录滞后。比较要用分诊后的数。
- 升级到 6.12：3,602 → 1,789；但升级只是清零，跟 stable 才是流程。
- **要能回答**：为什么"编译过的文件"过滤那么重要（2,875 条）？为什么说分诊不是修复？

### 8.6 签名密钥轮换与吊销 🔨
- 两级 PKI：设备信任 CA → 换签名密钥设备不用动（真机：release-2 签的包直接被接受）。
- 吊销要 CRL **在设备上** + `check-crl=true`；开了没 CRL = 拒绝一切；CRL 过期 = 拒绝一切。
- 真机：装 CRL 后，release-1 签的包 → `certificate revoked`。CRL 有效期是运维问题（30 天差点装进设备）。
- boot 密钥没有这一套（OTP 只钉一个哈希）→ 泄露就是永久的。
- **要能回答**：轮换和吊销为什么不对称？一个离线从不更新的设备，吊销对它有用吗？

### 8.7 没有 RTC 的设备，证书检查就是时钟检查 🔨
- 开机时钟 = systemd 的 SOURCE_DATE_EPOCH（2025-06-26），NTP 前证书"尚未生效"。
- use-bundle-signing-time：解决时钟，但签名时间由签名者写 → 过期密钥也能用。
- 时钟下限（镜像构建时间 + 上次同步时间）：便宜、保留过期检查。更好：RTC 电池 / 安全时间（TSA、roughtime）。
- **要能回答**：为什么说 use-bundle-signing-time 让"过期"失去意义？

### 8.8 OP-TEE 回滚 + RPMB 🔨
- REE FS 无 RPMB：回滚存储 → 第二个身份。有 RPMB（模拟）：回滚被发现 → 失败关闭（身份丢了，要重新 provision）。
- 模拟 RPMB 在普通世界进程里、测试密钥 → 证明机制不证明属性；SD 卡没有 RPMB，要 eMMC/UFS。
- **要能回答**：任何靠可改写存储执行的规则，为什么都能被回滚存储绕过？

### 8.9 MCU 认证：驱动还是门 🔨
- 挪进驱动 probe：密钥还是文件 → 看着更强、信任根没变 → 不做。probe 只检查一次，热插换板要重复检查。
- 做成 `ExecStartPre=+` 门：认证失败 → 服务不起 → 健康检查不过 → 更新不提交。
- **要能回答**：内核拿密钥的四种来源各值多少？为什么"fail closed"在内核里有代价？

### 8.10 工程教训（横切）🔨
- 配置是请求不是确认（Yama、ramoops、bbappend 优先级、CRL 30 天）。
- 先在主机上预测块号，再上板验证。
- 主机不是设备（BusyBox：无 install、date 无 %N、head 无 -c）。
- 恢复路径不能依赖它要恢复的东西。

---

## 复习流程（跟以前一样）

1. 我按这份地图提问（一次一个小主题，不一次问一堆）
2. 你用自己的话复述
3. **复述正确了**，我把它写进 `docs/notes/learning-qa.md`，**每条独立成 Qn**，
   只记最终正确的答案，不记纠正过程
4. 同时把这份地图里对应条目的状态从 ⬜/🔨 改成 ✅

**通用概念**（信任链、签名vs加密、哈希树、TEE 模型、防回滚……）如果足够通用，
也同步补进 `docs/notes/systems-programming-patterns.md`，那份文件的定位是
"换个项目也用得上的地基知识"。

**触发词**（新对话里说这些我就会找到这份文档）：
安全计划 · 知识地图 · secure boot · OP-TEE · dm-verity · LUKS ·
威胁建模 · RATIONAL · 面试准备
