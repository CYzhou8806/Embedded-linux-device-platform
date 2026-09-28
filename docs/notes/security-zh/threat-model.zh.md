# 威胁模型与安全审计（中文译本）

> 译自 [`docs/security/threat-model.md`](../../security/threat-model.md)，供自己复习用。
> 以英文原版为准；编号（F1–F17、A1–A4）、命令、文件名保持原样。

这是对本仓库构建出来的设备**当前状态**的一次审计，在改动任何东西之前完成。
它决定了安全工作的先后顺序：`docs/security/` 下后面的每一篇文档都修复下面的
一条或多条发现，并用编号回链到这里。

当前镜像是为 bring-up 和性能测量构建的，不是为部署构建的，所以下面大部分问题
都在意料之中。把它们写下来的意义在于：得到一份**排好序的清单**而不是一种感觉，
同时得到一个基线，用来衡量每一项修复的效果。

## 1. 系统与部署场景

```
                         ┌──────────── 物理外壳 ─────────────────────┐
                         │                                            │
  网络（WiFi、LAN）  ◄───┼── Raspberry Pi 5（Yocto 镜像）             │
  ─ SSH (dropbear :22)   │     启动分区（FAT）：固件、                │
                         │       config.txt、cmdline.txt、内核、DTB   │
                         │     rootfs（ext4，可读写）                 │
                         │     device-service (root) ─► devbus 共享内存 ─► │ 消费者
                         │     custom-acq 驱动，/dev/acq0             │
                         │          │ SPI + DATA_READY GPIO           │
                         │          ▼                                 │
                         │     STM32F103 采集 MCU                     │
                         │                                            │
  维修接触 ──────────────┼─► SD 卡槽、UART 调试口、                   │
                         │   MCU SWD 调试口、USB 口                   │
                         └────────────────────────────────────────────┘
```

本模型假设的场景是一台**安装在客户现场的联网设备**：它测量某些东西（温度、
过程数据），把数据上传，并由**不是厂家员工的技师**上门维护。设备在整个生命周期
内都不在厂家的物理控制之下，包括转卖和报废。

这个场景比任何单项技术选择都重要：它意味着
**物理接触是正常生命周期的一部分，而不是什么罕见的攻击**。

## 2. 资产

| 资产 | 为什么重要 | 需要的安全属性 |
| --- | --- | --- |
| 采集数据（样本、指标） | 客户依赖的过程/质量记录 | 完整性优先，其次机密性 |
| 固件与系统镜像 | 控制整台设备；被改过的镜像 = 持久性的沦陷 | 完整性、真实性 |
| 设备身份 | 让后端能信任设备上报的内容 | 真实性、唯一性 |
| 签名密钥（将来会有） | 谁拿到它，谁就能给每一台设备推代码 | 机密性——整个系统里最值钱的单一秘密 |
| 网络凭据（WiFi PSK） | 能进入客户的网络，而不仅仅是这台设备 | 机密性 |
| 配置 | 采样率、背压阈值——会改变记录下来的内容 | 完整性 |
| 日志 | 事故之后的调试与取证 | 完整性、可用性 |

## 3. 攻击者

| 攻击者 | 能接触到什么 | 现实的目标 |
| --- | --- | --- |
| **A1 — 网络攻击者** | 与设备同一个 LAN/WLAN，没有物理接触 | 接管设备，以它为跳板进入客户网络 |
| **A2 — 维修技师 / 内部人员** | 几分钟到几小时的物理接触；能打开外壳、拔 SD 卡、接 UART 或 SWD 探针 | 篡改记录、提取凭据、装改过的固件、克隆设备 |
| **A3 — 二手买家 / 报废处理** | 对一台退役设备的无限期物理接触 | 读取上一任主人的数据和凭据 |
| **A4 — 供应链** | 构建主机、第三方层、上游软件包 | 在镜像被签名之前把代码塞进去 |

远程攻击者（A1）决定**优先级**，因为他们能规模化。物理攻击者（A2、A3）决定
**设计**，因为在这个场景里他们必然存在。

## 4. 信任边界

1. **网络 ↔ 设备** —— SSH 是唯一在监听的服务。
2. **启动介质 ↔ 启动链** —— SD 卡上的一切，包括加载内核的第一个文件，
   目前都是不经检查就被信任的。
3. **内核 ↔ 用户态** —— `/dev/acq0` 和 sysfs 接口；驱动会校验它写给 MCU 的内容。
4. **进程 ↔ 进程** —— `device-service` 把数据发布到一个 devbus 共享内存段，
   订阅者映射这个段来读。
5. **Linux ↔ MCU** —— SPI 寄存器协议。每一帧都有回显校验来发现传输错误，
   但没有任何东西能证明回答的是**哪一个**设备。
6. **构建主机 ↔ 镜像** —— 上游层和本地配置（包括 WiFi PSK）不经检查地流入镜像。

## 5. 发现

严重度 = "谁"那一列的攻击者做到这件事有多容易 × 做到后能得到什么。
**Critical** 表示无需凭据即可远程利用；**High** 表示仅凭物理接触就能完全攻破
某项资产；**Medium** 表示真实存在的弱点，但需要前提条件或影响有限；
**Low** 表示纵深防御层面的问题。

| ID | 发现 | 谁 | 严重度 | 在哪修 |
| --- | --- | --- | --- | --- |
| **F1** | 开了 `debug-tweaks`：`root` 密码为空，dropbear 接受空密码的 root 登录。网络上任何人都能拿下这台设备。 | A1 | **Critical** | [hardening](hardening.zh.md) |
| **F2** | 没有漏洞管理：没人知道镜像里带着哪些已知 CVE。 | A1 | **High** | [hardening](hardening.zh.md)（`cve-check`） |
| **F3** | 串口控制台（`ttyAMA10`，Pi 5 的 UART 排针）上有 root 登录，同样是空密码。 | A2 | **High** | [hardening](hardening.zh.md)（产线镜像没有 getty） |
| **F4** | 没有验证启动。FAT 启动分区上的 `config.txt`、`cmdline.txt`、内核和设备树，任何能拔 SD 卡的人都能替换。 | A2, A4 | **High** | [secure-boot](secure-boot.zh.md) |
| **F5** | 根文件系统可读写，且完整性从不检查。被改过的二进制会在重启后继续存在而不被察觉。 | A1（在 F1 之后）, A2 | **High** | [hardening](hardening.zh.md)（只读）、[integrity-and-encryption](integrity-and-encryption.zh.md)（dm-verity） |
| **F6** | 完全没有更新机制，因此也没有防回滚。这里其他所有发现在现场设备上都是**永久的**。 | 全部 | **High** | [update-and-provisioning](update-and-provisioning.zh.md) |
| **F7** | WiFi PSK 以明文存在未加密 rootfs 的 `/etc/wpa_supplicant/` 里。拔出 SD 卡就拿到了客户的网络凭据。 | A2, A3 | **Medium** | [integrity-and-encryption](integrity-and-encryption.zh.md) |
| **F8** | 静态数据没有加密。退役设备上的记录和日志，谁拿到都能读。 | A3 | **Medium** | [integrity-and-encryption](integrity-and-encryption.zh.md) |
| **F9** | SPI 上没有设备认证。任何会说这套寄存器协议的 MCU 都会被当作采集外设接受，所以数据可以从源头被伪造。 | A2 | **Medium** | [device-authentication](device-authentication.zh.md) |
| **F10** | MCU 的 SWD 口是开着的，flash 读保护关闭（RDP level 0）：固件可以被读出和替换。 | A2 | **Medium** | [device-authentication](device-authentication.zh.md)（RDP level 1；局限在那篇写明） |
| **F11** | `device-service` 以 root 运行，没有任何 systemd 沙箱，而它其实只需要 `/dev/acq0`、一个 sysfs 属性和它自己的共享内存段。 | A1（需先有另一个漏洞） | **Medium** | [hardening](hardening.zh.md) |
| **F12** | 没有设备身份。设备上没有任何东西能向后端证明它是哪一台。 | A2 | **Medium** | [optee](optee.zh.md)、[update-and-provisioning](update-and-provisioning.zh.md) |
| **F13** | devbus 订阅者以读写方式映射共享内存段，所以一个被攻破的订阅者可以篡改其他订阅者正在读的 payload。发布者已经把订阅者写回来的索引当作不可信输入处理；但订阅者之间的 payload 完整性没有保护，文件权限（`0660`）是唯一的边界。 | A1（需先有另一个漏洞） | **Low** | 已记录；见 §7 |
| **F14** | 构建主机上保存着 WiFi PSK 的唯一一份副本，是个普通文件；而且目前还没有签名密钥，所以没有任何机制能阻止将来的密钥出现在构建脚本里。 | A4 | **Low** | [update-and-provisioning](update-and-provisioning.zh.md)（密钥放进 PKCS#11 token） |

审计中检查过、**不算发现**的：

- 驱动输入：`sample_rate` 在发给 MCU 之前做了范围检查，`control` 被掩码成 1 位，
  `read()` 只通过 `kfifo_to_user()` 交出完整的样本。可写的模块参数和 sysfs 属性
  只有 root 能写（`0644` / `DEVICE_ATTR_WO`）。
- SPI 协议对每一帧做回显校验，所以传输损坏能被发现——但这防的是噪声，
  不是攻击者（F9）。

## 6. 修复顺序，以及为什么不是简单的"严重度从高到低"

修复顺序综合了严重度、**依赖关系**和**成本**：

1. **F1、F2、F3、F11 —— Yocto 加固。** 严重且便宜，而且全都是镜像配置。
   远程 root 登录会让其他一切防护失去意义，所以它排第一，哪怕它是最没技术含量的工作。
2. **F5（dm-verity）和 F7/F8（加密）。** rootfs 的完整性，是让
   "我们签的镜像就是在跑的镜像"这句话在内核启动之后依然成立的东西。
3. **F4 —— secure boot。** 它是 F5 的锚：dm-verity 的根哈希只有在携带它的
   内核和命令行被验证过的情况下才可信。它在工作顺序上排在 dm-verity 后面，
   仅仅是因为这块硬件上的最后一步（把密钥哈希烧进 OTP）是不可逆的，
   我们有意不做——见 [secure-boot](secure-boot.zh.md)。
4. **F12 —— 在 TEE 里做设备身份**，以及 **F9/F10 —— SPI 设备认证**，
   后者复用这个身份。
5. **F6、F14 —— 签名更新与密钥管理。** 没有它们，上面每一项修复都会冻结在
   出厂时的那个版本。

## 7. 这块硬件的局限，提前说明

以下几点不会被上面任何工作解决，在相关的地方都会指出：

- **Raspberry Pi 5 没有 TPM，也没有安全元件。** 任何磁盘加密密钥都只能放在
  一个"拿着 SD 卡、时间足够的攻击者能够到"的地方——除非把它绑定到一个经过验证的
  启动状态，而这块板子只有在一个不可逆的 OTP 烧写步骤之后才能提供这种状态。
- **STM32F103 没有 TrustZone、没有加密加速器、没有安全存储。** RDP level 1
  提高了门槛，但它不是安全边界。Level 2 是永久的，项目里唯一一块 MCU 不用它。
- **OP-TEE 没有维护中的 Raspberry Pi 5 移植。** TEE 相关工作跑在 QEMU
  （Armv8-A）上，这本来也是 OP-TEE 通常的开发环境。
- **F13** 是零拷贝共享内存的固有属性，不是 bug：要让 payload 对订阅者只读，
  需要为每个订阅者单独做一个只读映射，这是 devbus 的设计变更，不在本次审计范围内。

## 8. 做完之后

同样的十四条发现，在这个目录下的每篇文档都完成后重新审计了一遍——并且在板子
允许的范围内，在运行 `device-platform-image-ab` 的 Raspberry Pi 5 上核对过。
**"已关闭"** 表示在设备上看到修复生效，包括反向测试（本该失败的访问真的试过并且
失败了）。**"部分"** 表示修复存在但缺了一环。**"受硬件限制"** 表示剩下的部分需要
这块板子没有的东西，或者需要一个在项目唯一一块板子上不做的不可逆步骤。

| ID | 之前 | 现在 | 证据 | 还剩什么 |
| --- | --- | --- | --- | --- |
| **F1** | root 空密码，走 SSH | **已关闭** | [hardening](hardening.zh.md) §5：用 admin 的密钥登 root → `Permission denied (publickey)`；服务器根本不提供密码方式；端口转发 → `administratively prohibited`（[记录](../../../results/security/hardening/first-boot-prod.txt)） | — |
| **F2** | 没有 CVE 跟踪 | **部分** | [hardening](hardening.zh.md) §4：用户态 16 → 4，每条都记录了理由；内核经 CNA 数据 + 编译文件分诊后 3,853 → 3,602（[汇总](../../../results/security/cve/kernel-triage-summary.txt)） | 内核落后 stable 94 个版本；解决办法是流程（跟 stable），不是分诊 |
| **F3** | UART 排针上有 root 登录 | **已关闭** | [hardening](hardening.zh.md) §5：板子上 `serial-getty@ttyAMA10` 已 masked/inactive | HDMI/USB 键盘控制台没有进一步审计，只确认了"没有任何密码能匹配" |
| **F4** | 启动分区不验证 | **受硬件限制** | [secure-boot](secure-boot.zh.md)：boot 密钥在 HSM 里，签名/反签名的镜像在主机上验证过，一个签名 `boot.img` **在板子上启动成功**（§3.4） | 在密钥哈希烧进 OTP 之前（不可逆，没做），固件不检查签名。从 1.1.0 起，每个 A/B 槽和每个更新包都带签名 `boot.img`（[secure-boot](secure-boot.zh.md) §3.5） |
| **F5** | rootfs 可读写，从不检查 | **已关闭**（完整性）/ 部分（真实性） | 只读根（[hardening](hardening.zh.md) §5）；dm-verity：在卡上改 1 个字节 → 恰好在预测的那个块上报 `EIO`（[case 10](../../debugging/case-10-dm-verity-one-byte-on-the-card.md)）；verity 代价 16%（[integrity](integrity-and-encryption.zh.md) §5） | 根哈希的可信程度取决于携带它的内核——即 F4 |
| **F6** | 没有更新，没有防回滚 | **已关闭**（更新器）/ 受限（SD 卡） | [update-and-provisioning](update-and-provisioning.zh.md) §3.1：板子上通过 tryboot 做签名 A/B；一次真实的坏更新在断电一次后回滚；好更新由健康检查提交；0.9.0 被 `min-bundle-version` 拒绝（[记录](../../../results/security/update/on-target-ab.txt)） | 拿着 SD 卡的人仍然可以直接写入一个旧的、合法签名的镜像——需要单调计数器（OTP 位 / RPMB / TPM） |
| **F7** | WiFi PSK 明文在卡上 | **部分** | 板子上有 LUKS2 数据分区（[integrity](integrity-and-encryption.zh.md) §5）；SSH 主机密钥和 RAUC 状态已经放在里面 | PSK 仍在 rootfs 镜像里，而且 LUKS 密钥是由序列号推出来的替代品——**对 A3 来说这还不算保护**（那篇文档 §3 说明了真正的密钥应该放哪）。从 1.1.0 起该分区还带完整性（每扇区 HMAC；改一个字节 → `EIO`，[§6](integrity-and-encryption.zh.md)） |
| **F8** | 静态数据不加密 | **部分** | 同 F7：加密机制是真的、测过（−7%） | 同样的密钥问题；device-service 写的数据还没导向 `/data` |
| **F9** | 任何 MCU 都被接受 | **部分** | [device-authentication](device-authentication.zh.md) §6：固件 v1.4 里的 HMAC 挑战-应答，110/110 有效，错误密钥一律被拒，连续 100 次认证期间没有丢一个样本 | 从 1.2.0 起它是 `device-service` 的启动门（因而也是每次更新能否提交的门）——[§7](device-authentication.zh.md)；密钥仍是文件，不在驱动里，也没有逐帧 MAC，所以认证之后插在中间的设备拦不住 |
| **F10** | SWD 开着，RDP 0 | **受硬件限制** | 演示了 RDP 1 能阻止调试器读出密钥，解除它会整片擦除（[记录](../../../results/security/device-auth/rdp-level1-demo.txt)） | F103 的 RDP 1 有公开绕过（CVE-2020-8004）；RDP 2 是永久的，不用；MCU 为了开发留在 RDP 0 |
| **F11** | device-service 以 root 运行，无约束 | **已关闭** | 暴露评分 9.4 → 1.8；板子上以用户 `acq` 运行，`CapEff = 0x804000`，seccomp，独立网络命名空间，没有丢样本 | — |
| **F12** | 没有设备身份 | **受硬件限制** | 身份密钥在 OP-TEE TA 里生成（QEMU），证书由 HSM Device CA 通过带持有证明的产线工站签发（[optee](optee.zh.md)、[provisioning](update-and-provisioning.zh.md) §5） | Pi 5 上没有 TEE；没有 RPMB 时，回滚 TEE 存储会产生第二个身份（[optee](optee.zh.md) §3.2） |
| **F13** | devbus payload 订阅者可写 | **未关闭，设计如此** | 未变；在 §7 中说明 | 需要每个订阅者单独的只读映射——devbus 的设计变更 |
| **F14** | 没有密钥管理 | **已关闭** | 四把密钥，在 PKCS#11 token 中以不可导出方式生成，PIN 从不进 argv（[update-and-provisioning](update-and-provisioning.zh.md) §1）；构建端只持有开发密钥 | SoftHSM 是软件替代品：真 HSM 多出来的是备份和物理保护 |

十四条中有七条在板子上完全或基本关闭。每一条"受硬件限制"都归结到同一个缺失的
东西——**一个拿着 SD 卡的攻击者碰不到的秘密或计数器**：OTP 里的密钥哈希（F4）、
OTP 设备密钥（F7、F8、F9 的配对密钥）、单调计数器（F6、F12）。这张表要记住的
就是这一句话：**在这块板子上，每项防护的软件部分都做完了，信任根是被模拟的那一部分。**

### 新发现：做了才发现的

§5 的审计是通过阅读镜像完成的。下面这几条是在构建和运行修复时冒出来的，
原始清单里一条都没有：

| ID | 发现 | 状态 |
| --- | --- | --- |
| **F15** | 根文件系统只读后，SSH 主机密钥和 `/etc/machine-id` 每次开机都重新生成；不停变化的主机密钥会训练管理员接受任何指纹（这正是中间人攻击的前提），而且由 machine-id 推出来的 DHCP client ID 让 IP 也变了 | 已关闭：主机密钥放到 `/data`，`ClientIdentifier=mac` —— 重启之间、A/B 槽之间密钥和 IP 都不变 |
| **F16** | 远程访问依赖了最新的组件（数据分区）；一个缺失的 BusyBox 小程序就让 SSH 挂了，而设备又没有密码登录 | 已关闭：主机密钥回退到 `/run`；启动记录器把证据留在分区 1（[update-and-provisioning](update-and-provisioning.zh.md) §3.1） |
| **F17** | 一个 systemd 排序环让健康检查任务在每次开机时被静默删除——A/B 设计所依赖的那道门根本不存在 | 已关闭：改由 timer 启动；`journalctl -b \| grep "ordering cycle"` 现在是检查项之一 |
