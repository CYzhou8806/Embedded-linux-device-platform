# 在 SPI 链路上认证 MCU —— 设计（中文译本）

> 译自 [`docs/security/device-authentication.md`](../../security/device-authentication.md)，供自己复习用。
> 以英文原版为准；寄存器名、命令、日志输出保持原样。

针对[威胁模型](threat-model.zh.md)中的发现 **F9**（任何会说这套寄存器协议的 MCU 都被接受）和
**F10**（MCU 的 SWD 口和 flash 是开放的）。

**状态：已实现，并在板子上测量过**（§6），与下面的设计有两处有意的差异，在 §6.4 说明。
代码：固件 [`Core/Src/devauth.c`](../../../v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/devauth.c)
（v1.4）、驱动 sysfs 属性 `auth_challenge` / `auth_response` / `auth_cycles`、
[`tools/pair-mcu.sh`](../../../tools/pair-mcu.sh)、
[`security/device-auth/mcu-auth-check.py`](../../../security/device-auth/mcu-auth-check.py)。
原始输出：[`results/security/device-auth/`](../../../results/security/device-auth/)。

## 1. 它防什么，不防什么

攻击者是威胁模型里的维修技师或二手买家（A2/A3），他把采集板换成一块说同样协议的板子，然后随心所欲地上报数值——
从未测量过的温度、从未真实发生过的过程记录。目前没有任何东西能阻止：驱动检查的是 `DEVICE_ID`，而任何克隆板都能返回它。

| 攻击 | 这个设计能挡住吗？ |
| --- | --- |
| 一块不同的或仿冒的采集板 | **能**——没有配对密钥就回答不了挑战 |
| 回放一次录下来的认证交互 | **能**——每次挑战都带一个新鲜的 nonce |
| 从正品 MCU 里读出密钥，然后克隆 | **只是提高门槛**——见 §4 |
| 认证**之后**插在 SPI 总线中间篡改样本的设备 | **不能**——那需要每帧或每批数据一个 MAC（§5） |

## 2. 交互过程

挑战-应答，使用一个 Pi 和一个 MCU 之间共享的密钥，走现有的寄存器协议（5 字节帧：命令字节 = 写标志 + 7 位寄存器地址，
然后是 32 位值；对第 N 帧的回复在第 N+1 帧里到达）：

```
Pi（驱动，在 probe 时以及每次 MCU 复位后）                MCU
  nonce = 16 个随机字节（内核 RNG）
  写 REG_AUTH_NONCE0..3               ─────────────►  保存 nonce
  写 REG_AUTH_CTRL = START            ─────────────►  在主循环里，而不是 SPI 中断里：
                                                        mac = HMAC-SHA256(K,
                                                          "acq-auth-v1" ‖ DEVICE_ID ‖
                                                          FW_VERSION ‖ nonce)
  轮询 REG_STATUS.AUTH_DONE           ◄─────────────  置位 AUTH_DONE
  读 REG_AUTH_MAC0..3（mac 的前 16 字节）
  重新计算并以常数时间比较
  之后才注册 /dev/acq0
```

- **新寄存器** 放在 `REG_SPI_ERROR_COUNT`（0x0A）之后的空闲区间：四个 nonce 字、一个控制寄存器、四个 MAC 字，
  以及 `REG_STATUS` 里的两个状态位（`AUTH_BUSY`、`AUTH_DONE`）。
- **在中断之外计算。** SPI 中断必须在帧间隙内重新挂起（re-arm）（case-01 和 case-02 的教训：这条链路对任何延迟中断的东西
  都很敏感）。MAC 在主循环里计算，通过一个状态位通知。
- **`DEVICE_ID` 和 `FW_VERSION` 放进 MAC 里**，这样一个固件的回答不能被冒充成另一个固件的。
- **截断到 128 位**：读四个寄存器而不是八个；HMAC 的 128 位远超通过 SPI 在线猜测所能达到的范围。
- **谁来验证**：内核驱动，在注册 `/dev/acq0` 之前。用户态永远看不到未经认证的设备，失败时只有一条 `dev_err` 且没有设备节点，
  很容易监控。

## 3. 密钥放在哪

- **每一对一把密钥，而不是整个产品线共用一把。** 从一个 MCU 里提取出密钥，不能对任何其他设备有帮助。Pi 这样推导：
  `K = HMAC(pi_device_key, "acq-pairing-v1" ‖ MCU_UID)`，其中 `MCU_UID` 是 STM32 的 96 位唯一 ID。
- **Pi 这边**：在这块板子上，`pi_device_key` 正好就是 Raspberry Pi 固件 HMAC 服务所提供的（用 OTP 设备密钥做
  `rpi-fw-crypto hmac`，可锁定到重启——见 [integrity-and-encryption](integrity-and-encryption.zh.md) §3），在有 TEE 的平台上
  则是设备身份 TA（[optee](optee.zh.md)）。推导出的 `K` 根本不需要存在 Pi 上。
- **MCU 这边**：配对时 `K` 被一次性写入一个专用 flash 页——这是一个 provisioning 步骤
  （[update-and-provisioning](update-and-provisioning.zh.md) §5）：通过 SWD 读 MCU 的 UID，在 Pi 上推导 `K`，烧进去，
  设置 RDP level 1。
- **重新配对**（合法更换板子之后）是同一个步骤，由有授权的人来做——这正是重点：换板子从一件看不见的事变成一件有记录的事。

## 4. 诚实的局限：STM32F103

F103 没有安全元件、没有 TrustZone、没有硬件加密。密钥就放在普通 flash 里。

- **RDP level 1** 阻止调试器读 flash，而且是可逆的（回到 level 0 会整片擦除芯片，密钥也一起擦掉）。这里用的就是这一级。
- **在这个系列上，RDP level 1 不是安全边界。** 已发表的研究（Schink & Obermaier，
  [*Exception(al) Failure — Breaking the STM32F1 Read-Out Protection*](https://blog.zapb.de/stm32f1-exceptional-failure/)，
  2020，CVE-2020-8004）通过调试接口从受 RDP-1 保护的 STM32F1 芯片中提取 flash：调试器对 flash 的读取被阻止了，但 CPU 进入异常时
  仍然会通过 ICode 总线从 flash 取向量。拿着板子、论文和一个调试探针的人就能拿到密钥。
- **RDP level 2** 永久禁用调试口，不可逆；项目唯一的 MCU 不用它（见项目计划里的红线）。

所以这个设计把"换板子"从**任何有克隆板的人**变成了**得先从正品 MCU 里提取出密钥的人**——门槛更高，而不是保证。
真正的解决办法在硬件上：一个外部安全元件（例如 ATECC608 这类器件，持有 `K` 并自己计算 MAC），或者一个带 TrustZone-M
和安全存储的 MCU（Cortex-M33 这一级）。

## 5. 代价，以及要测什么

F103 用软件算 SHA-256。要在板子上测的数字，用本项目一贯的方法（多次重复，在计算前后用 GPIO 打标记接逻辑分析仪）：

- 72 MHz 下对约 48 字节消息做一次 HMAC-SHA256 的周期数；
- 主循环计算它时，SPI 中断是否还能及时重新挂起（`REG_SPI_REARM_FAIL` 保持为 0）；
- probe 增加的时间。

probe 时的认证是一次性成本。保护每一个样本不被中间人篡改是另一笔预算：每秒 1000 个样本时，每个样本的代价必须和现有的
约 1 ms 端到端延迟放在一起（[performance](../../performance.md)）——这就是为什么 §1 在知道每个 MAC 的代价之前把它列为不在范围内，
也是为什么如果真的需要，对**一批**样本做 MAC 很可能才是答案。

## 6. 在板子上

### 6.1 正确性

SHA-256 和 HMAC-SHA256 用可移植 C 写成、不依赖 HAL，所以同一个文件先在开发主机上对照 Python 的 `hashlib` 和 `hmac` 检查过
（[`tests/devauth_test.py`](../../../v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/tests/devauth_test.py)）：
RFC 4231 测试向量、0 到 129 字节的每一种消息长度（覆盖所有填充边界）、以及设备认证的消息格式——**321 项，0 差异**。

在设备上，验证者生成 nonce 并用密钥检查每个回答；每个回答还会用一把随机的错误密钥检查一遍：

```
10/10 valid (MCU idle), 100/100 valid (during acquisition)
every answer rejected under a wrong key; answers distinct per nonce
before pairing (erased key page): auth_response -> ENOKEY ("Required key not available")
```

### 6.2 代价，以及对采集的影响

| | 周期数 | 72 MHz 下 |
| --- | --- | --- |
| MAC，MCU 空闲（10 轮） | 35 062 – 35 116 | **487 µs** |
| MAC，1000 Hz 采集进行中（100 轮） | 40 411 – 41 727 | **561 – 580 µs** |

周期数来自 `devauth_compute()` 前后 Cortex-M3 的 DWT 计数器；72 MHz 通过 `RCC_CFGR` 确认（`0x001d040a`：PLL 来自 HSE，×9，AHB ÷1）。
负载下增加 15–19% 正说明设计在起作用：MAC 在主循环里跑，SPI 和定时器中断抢占它，计数器把它们的时间也算进去了。
是认证给采集让路，而不是反过来：

```
100 authentications, back to back, during acquisition:
  spi_rearm_fail 0 -> 0, spi_error_count 0 -> 0, kfifo_overflow 0 -> 0, policy_dropped 0 -> 0
  device-service: rate=1000.2/s gap_count=0
```

固件大小：flash 7.3 KB → 9.3 KB。

### 6.3 读保护，level 1

（[记录](../../../results/security/device-auth/rdp-level1-demo.txt)）

- **之前**：调试器直接从最后一个 flash 页读出了密钥（`0x0807f800: 61b413d4 a88f1b26 …`）——发现 F10，实际演示。
- **设置 level 1 之后**：同样的读取失败（`Failed to read memory`）。设备继续工作、继续认证——**但要在 MCU 上电复位之后**：
  如果设置保护时调试器连着，flash 对 CPU 也保持不可访问，直到上电复位（RM0008）；按复位键不够。
- **给受保护的芯片接上调试探针，它又停了。** 在 RDP 1 下，探针读不到密钥，但可以让设备停机。
- **解除 level 1 会整片擦除芯片**：密钥页和固件都变成 `ffffffff`——"降级即擦除"的设计，亲眼看到了。固件和密钥用
  `pair-mcu.sh` 恢复（同样的 UID → 同样的密钥）。

§4 的局限依然成立：这个系列的 level 1 有一个已公开的绕过方法（CVE-2020-8004），不用降级就能读出 flash。

### 6.4 与设计的差异，以及原因

- **验证者还不在内核驱动里。** 驱动只负责搬运字节（`auth_challenge` / `auth_response`）；检查在 `mcu-auth-check.py` 里做。
  在 probe 时把关 `/dev/acq0` 需要内核能拿到密钥——或者能拿到推导密钥的固件 HMAC 服务——而在这块板子上，这意味着那把没烧的 OTP 密钥。
- **配对主密钥是一个文件，而不是 Pi 的 OTP 设备密钥。** `pair-mcu.sh` 完全按设计推导 `K = HMAC(master, "acq-pairing-v1" ‖ UID)`，
  但 `master` 是签名主机上一个随机生成、权限 0600 的文件。烧写 Pi 的 OTP 密钥不可逆，在项目唯一一块板子上不做。

### 6.5 测试时出了什么问题

这次工作中有两次失败不在被测代码里，把它们写下来是因为它们耗费的时间：

- **MOSI 上的杜邦线松了。** 固件烧进去之后，只有寄存器 `0x00` 能读。通过 SWD 读 MCU 的接收缓冲区，看到每一帧到达时都是全零——
  也就是"读 `DEVICE_ID`"——所以只有地址为零的那个寄存器"能用"。把线重新插好就好了；重新烧旧固件、复位 MCU、重启 Pi 都没用。
- **测试工具本身。** 之后每一次 sysfs 写入仍然失败——换回原来的驱动也一样。对 SPI 传输做 ftrace
  （`events/spi/spi_transfer_*`）显示，写的是 `1000`，驱动发出去的却是 `1`：在这个 BusyBox 系统上，
  `echo 1000 | sudo tee <attr>` 没有在一次写入里把完整的值交给 sysfs。改用 `sudo sh -c "printf 1000 > <attr>"` 一切正常。
  链路和驱动从头到尾都没问题。

## 7. 把检查挪进驱动——设计，以及为什么没做

§6.4 把验证者留在了一个用户态工具里。显而易见的下一步是在 `probe()` 里挑战 MCU，只有回答正确才注册 `/dev/acq0`。
机制很简单——驱动已经有 `auth_challenge` / `auth_response`，`get_random_bytes()` 能给它 nonce。难的是整个设计的关键所在：
**内核从哪里拿到密钥？**

| 密钥从哪来 | 需要什么 | 在这块板子上值多少 |
| --- | --- | --- |
| 编译进模块 | 什么都不需要 | **一文不值**——模块是 rootfs 上的一个文件，拿到卡的人都能读出密钥，而且每台设备都一样 |
| 由用户态加载进内核 keyring（`keyctl add logon …`），驱动用 `request_key()` 读取 | 一个密钥类型、一个开机加载器 | 和现在同样的信任——密钥仍然来自一个 root（或拿到卡的人）能读的文件；挪动的是检查，不是秘密 |
| 在内核里通过固件的 HMAC 服务（`rpi-fw-crypto`）由 Pi 的 OTP 设备密钥推导——即 §3 的设计 | 驱动发一个 mailbox 调用；**OTP 密钥已烧写** | 真正有价值：每台设备不同、从不在文件里、可锁定到重启——但烧 OTP 不可逆，唯一一块板子上不做 |
| 由 TEE 持有；驱动通过内核的 TEE 客户端 API 向 TA 请求（`tee_client_open_session()`，OP-TEE 的 RNG 和 fTPM 驱动就是这么做的） | 一个 TA、平台上有 TEE | 在带 TrustZone 固件的 SoC 上是正确答案；Pi 5 没有 OP-TEE 移植（[optee](optee.zh.md) §1） |

另外两个性质决定了结论：

- **probe 时的检查就是开机时的检查。** 系统运行期间 MCU 可以被换掉；只在 `probe()` 时做的检查，对一小时后接上的板子什么都说明不了。
  它必须重复进行——每次 `open()` 时，或定期——即便如此，也只有逐帧 MAC（§5）能覆盖两次检查**之间**的数据。
- **在内核里失败即关闭是有代价的。** 一个认证失败的 MCU——合法维修之后密钥不对、固件更新弄丢了密钥页——会让设备没有 `/dev/acq0`，
  在这台设备上就等于没有产品。这是一个策略决定（外加一套重新配对的服务流程），而不是驱动的细节。

**决定：不实现。** 用这块板子在不做不可逆操作的前提下能提供的密钥来源，驱动里的检查看起来会比用户态的更强，但信任根是一样的——
一个文件。内核把关的诚实版本是表里的第三行，而它离一个 `program_pubkey` 级别的决定只差一步。

**值得做而且便宜的**：把现有的检查从一个工具变成一道门。从 1.2.0 起，
[`device-platform-mcu-auth`](../../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/device-platform-mcu-auth)
作为 `device-service` 的 `ExecStartPre=+` 运行——用 BusyBox 和 `openssl` 命令行，不需要 Python：从内核 RNG 取 16 字节 nonce，
通过驱动的 sysfs 属性发挑战，用 `openssl dgst -mac HMAC` 重新计算 MAC。配对密钥放在加密的数据分区上
（`/data/devauth/mcu.key`，root 0600）——如上表所说仍然是个文件，但不再在签名主机上了
（[记录](../../../results/security/update/on-target-signed-ab.txt)）：

```
right key : mcu-auth: OK - MCU 0xac00acc0 fw 0x00010400 answered the challenge (38729 cycles)
random key: mcu-auth: FAIL - wrong answer from MCU 0xac00acc0 (not the paired device, or not paired)
no key    : mcu-auth: FAIL - no pairing key at …
```

认证失败会阻止 `device-service` 启动；健康检查于是永远不会提交这个槽，所以一个破坏了配对的更新会自己回滚。
它在每次服务启动时都运行，而不只是开机时——系统运行期间被换掉的 MCU 会在下一次服务重启时被抓到，但不会更早
（逐帧 MAC，§5，仍然是那个问题的答案）。
