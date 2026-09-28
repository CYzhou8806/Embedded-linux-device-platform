# OP-TEE 里的设备身份密钥（中文译本）

> 译自 [`docs/security/optee.md`](../../security/optee.md)，供自己复习用。
> 以英文原版为准；命令、错误码、日志输出保持原样。

关闭[威胁模型](threat-model.zh.md)中的发现 **F12**——"设备上没有任何东西能向后端证明它是哪一台"——
办法是一个可信应用（TA）：它持有一把私钥，并且只用它来回答挑战。
代码：[`security/optee-ta/device_identity/`](../../../security/optee-ta/device_identity/)。
下面每次运行的原始输出：[`results/security/optee/`](../../../results/security/optee/)。

## 1. 为什么跑在 QEMU 上而不是 Raspberry Pi 5 上

OP-TEE 没有维护中的 Raspberry Pi 5 移植（旧的 Raspberry Pi 3 移植只是个演示，没有安全内存隔离，而且已无人维护）。
所以 TA 是在 OP-TEE 自己的参考目标上构建和运行的：**QEMU `virt` Armv8-A，`secure=on`**，OP-TEE 本身就是在这里
开发和测试的。§3 的一切都与板子无关；**确实**依赖板子的部分列在 §5。

软件栈，用 OP-TEE 的 manifest `qemu_v8.xml` 在 **4.10.0** 版本构建：
TF-A（BL1/BL2/BL31）→ OP-TEE OS 作为 BL32 → U-Boot 作为 BL33 → Linux + buildroot，普通世界里有
`tee-supplicant` 和 `libteec`。

构建本身先用 OP-TEE 的测试套件检查过：

```
xtest: 143 test cases of which 0 failed (41358 subtests), 11 min under QEMU
```

## 2. 一次调用是怎么到达 TA 的

```
 普通世界（EL0）      devid ── libteec ── ioctl(/dev/tee0)
 普通世界（EL1）                Linux TEE 驱动（optee）
                                    │ SMC
 EL3                              TF-A BL31（安全监控器）── 世界切换
 安全世界（S-EL1）                OP-TEE OS ── 加载/验证 TA，拥有它的内存
 安全世界（S-EL0）                device_identity TA ── 安全存储里的密钥对象
                                    │ 为文件 I/O 回调（RPC）到普通世界
 普通世界                         tee-supplicant ── /var/lib/tee/*（加密、带 MAC）
```

两个容易忽略的后果：

- **TA 的数据仍然由普通世界来存。** OP-TEE 的 REE FS 把安全存储作为文件放在 Linux 文件系统上，通过
  `tee-supplicant` 写入。它们用从硬件唯一密钥推导出的密钥加密并做完整性保护，但**普通世界可以删除、损坏或回滚它们**
  （§3.3、§3.4）。
- **参数放在共享内存里。** 传给 TA 的 memref 指向的内存，在 TA 运行期间普通世界仍然可以继续写。TA 只拷贝一次 nonce，
  之后只用这份拷贝，所以被检查的就是被哈希的（没有 double fetch 问题）。

## 3. TA 以及运行结果

TA 持有一把 ECDSA P-256 密钥，有三个真正的命令：

| 命令 | 做什么 |
| --- | --- |
| `PROVISION` | 在 **TA 内部**生成密钥（`TEE_GenerateKey`），限制为 `TEE_USAGE_SIGN`、不带 `TEE_USAGE_EXTRACTABLE`，存为持久对象，返回公钥。如果密钥已存在则拒绝。 |
| `GET_PUBKEY` | 返回公钥。 |
| `SIGN_CHALLENGE` | 签 `SHA-256("device-platform/devid-challenge/v1" ‖ nonce)`，只接受 16–64 字节的 nonce。 |

固定的域字符串是有意设计的：TA 从不签调用者选定的摘要，所以普通世界的 root——它**能**调用 TA——可以用它证明
"我是这台设备"，但不能用它签一个更新包、一个证书请求，或任何恰好是 SHA-256 值的其他东西。

另一台机器上的验证者（[`verify.py`](../../../security/optee-ta/device_identity/verify.py)）生成 nonce，只用
provisioning 时记录的公钥来检查回答。一次运行，由 [`run-demo.sh`](../../../security/optee-ta/device_identity/run-demo.sh)
无人值守驱动：

| # | 步骤 | 结果 |
| --- | --- | --- |
| 1 | provisioning 之前 `GET_PUBKEY` | `0xffff0008` ITEM_NOT_FOUND |
| 2 | `PROVISION` | 公钥 `04063dbc…dec9` |
| 3 | 再次 `PROVISION` | `0xffff0003` ACCESS_CONFLICT |
| 4 | 签验证者的 32 字节 nonce | 验证者：**VALID**；同一个签名对另一个 nonce：**INVALID** |
| 5 | 8 字节 nonce | `0xffff0006` BAD_PARAMETERS |
| 6 | TA 尝试读自己的私钥 | **TA panic**（客户端收到 `0xffff3024` TARGET_DEAD） |
| 7 | panic 之后 `GET_PUBKEY` | 同一把密钥 |
| 8 | 查看 `/var/lib/tee` | 37 个文件，属主为用户 `tee`，内容是密文 |
| 9 | 把存储**回滚**到第 2 步之前拍的副本 | `GET_PUBKEY` → ITEM_NOT_FOUND；`PROVISION` 成功，并创建了一个**不同的**身份 |
| 10 | 每个存储文件翻转一个字节 | `0xf0100001` CORRUPT_OBJECT |

### 3.1 密钥不可导出——连 TA 自己也不行

第 6 步是一个只为测试这一点而存在的命令：它对存储的密钥调用
`TEE_GetObjectBufferAttribute(TEE_ATTR_ECC_PRIVATE_VALUE)`。GlobalPlatform API 规定：从一个不带
`TEE_USAGE_EXTRACTABLE` 的对象读取受保护属性时必须 panic；OP-TEE 的日志显示
`TA panicked with code 0xffff0006`，TA 被拆除，重新加载后密钥依然在（第 7 步）。这个属性是由 TEE 内核强制的，
而不是靠 TA 自己守规矩。

### 3.2 每台设备只有一个身份——前提是存储不能被回滚

第 3 步显示 TA 拒绝替换它的密钥。第 9 步显示为什么这个检查**不够**：恢复一份旧的 `/var/lib/tee` 副本——
普通世界里任何 root 进程都能做到——会让 OP-TEE 把那个更旧的、没有密钥的状态当成合法的，于是"只能一次"的规则
就高高兴兴地创建了第二个身份。

OP-TEE 自己在开机时就在安全世界日志里说了：

```
I/TC: WARNING (insecure configuration): Failed to get monotonic counter for REE FS, using 0
```

这是 OP-TEE 文档里对没有 RPMB 的 REE 文件系统所描述的行为：每个文件都加密并认证了，但没有任何普通世界无法重置的
东西来记录**哪个版本**是当前的。用 `CFG_RPMB_FS=y` 和 `CFG_REE_FS_INTEGRITY_RPMB` 时，目录文件的哈希保存在 eMMC 的
RPMB 分区里，它的写计数器由 eMMC 维护，写入需要一把普通世界没有的密钥——第 9 步的回滚就会被发现。QEMU 没有 eMMC，
但 `tee-supplicant` 可以模拟一个。

**用 RPMB（模拟）再跑一次。** 用 `CFG_RPMB_FS=y CFG_RPMB_TESTKEY=y CFG_RPMB_WRITE_KEY=y` 重新构建 OP-TEE 内核
（27 秒；TA 和普通世界都没变），同一个演示脚本（[记录](../../../results/security/optee/rpmb-emulated/)）：

| | 只有 REE FS | REE FS + RPMB（模拟） |
| --- | --- | --- |
| 开机日志 | `Failed to get monotonic counter for REE FS, using 0` | 消失；取而代之的是 `RPMB: Using test key`、`Auth key not yet written` → 写入密钥 → `Found working RPMB device` |
| 第 1–8 步（provision、拒绝重复 provision、签名、密钥不可导出、TA panic 后仍在） | 符合设计 | 完全相同 |
| **第 9 步：把旧的 `/var/lib/tee` 放回去** | 接受；`provision` 创建了**第二个身份** | **拒绝**：`TEEC_OpenSession: 0xf0100001`（`TEE_ERROR_CORRUPT_OBJECT`）；没有第二个身份 |
| 第 10 步：每个文件翻转一个字节 | 拒绝（`CORRUPT_OBJECT`） | 拒绝（`CORRUPT_OBJECT`） |

安全世界日志显示了失败**在哪里**：OP-TEE 从 RPMB 读目录文件的哈希（`fh->filename=/dirfile.db.hash`），恢复回来的
`dirf.db` 与之不匹配，整个存储被拒绝——彻底到连 TA 都加载不了了
（`ldelf_syscall_open_bin … (Secure Storage TA) res=0xf0100001`）。回滚被发现了，设备**失败即关闭（fails closed）**：
攻击者得不到第二个身份，但设备失去了它的身份以及安全存储里的所有其他对象，直到重新 provision。发现把伪造变成了
拒绝服务，和 §3.3 是同样的取舍。

为什么这只是**机制**的演示，而不是**属性**的演示：

- **这个"RPMB"是 `tee-supplicant` 里的一个数据结构**，而 `tee-supplicant` 是普通世界的进程。它防的恰好是那个改写
  `/var/lib/tee` 里文件的攻击者——但同一个攻击者（普通世界的 root）可以重启 `tee-supplicant`，得到一个空白的"RPMB"。
  在真实的 eMMC 上，计数器和认证密钥都在闪存控制器里，普通世界够不着。
- **`CFG_RPMB_TESTKEY=y`**：RPMB 密钥是一把固定的测试密钥，不是从硬件唯一密钥推导的，调试日志还会把它打印出来。
  用 `CFG_RPMB_WRITE_KEY=y` 时，OP-TEE 会把这把密钥写进它找到的任何未编程的 RPMB——在产线上这必须恰好发生一次，
  而且在可信环境里进行，因为 RPMB 密钥是一次性可编程的。
- Raspberry Pi 5 从 SD 卡启动，SD 卡根本没有 RPMB。这个设计要成真需要 eMMC（或 UFS）。

一般性的教训是：**TEE 通过存储来执行的任何策略，其强度都取决于那份存储的防回滚能力**——和固件更新防回滚是同一个道理
（[update-and-provisioning](update-and-provisioning.zh.md)）。

### 3.3 损坏能被发现——然后变成拒绝服务

第 10 步在每个存储文件里翻转一个字节。OP-TEE 拒绝这些数据（`TEE_ERROR_CORRUPT_OBJECT`，连会话都打不开），它的
REE FS 代码还会删除发现损坏的文件（安全世界日志里的 `ree_fs_open_primitive: Remove corrupt file`——同一行在开机时、
任何篡改之前也出现过一次，所以它不是这个测试独有的）。完整性守住了：没有伪造的东西能通过。可用性没守住：任何能写
`/var/lib/tee` 的人都能毁掉设备的身份。产品必须为此做规划——重新 provision，或者让后端能区分"身份丢失"和"设备被替换"。

## 4. TA 适合做什么、不适合做什么

适合的，因为秘密永远不需要离开：设备身份和证明（attestation）密钥、磁盘加密的密钥派生、单调计数器、
用一把普通世界不能替换的密钥去验证某样东西。

不适合的：任何又大又复杂的东西（TCB 里代码更多）、任何需要安全世界没有的驱动的东西，以及任何**决定**反正是在普通世界
做出的东西——一个向普通世界程序返回"valid"、而那个程序可以无视这个答案的 TA，什么都保护不了。

TEE 防不了的：TA 或 OP-TEE 本身的 bug（两者都是可信计算基的一部分）、共享缓存上的侧信道、被攻破的 secure boot 链
（谁控制了 BL31/BL32，谁就控制了 TEE），以及——如 §3.2/§3.3 所示——普通世界对存储的控制。

## 5. 在真实硬件上会有什么不同

| 在 QEMU 上 | 在真实 SoC 上 |
| --- | --- |
| 硬件唯一密钥（HUK）是一个**常量（全零）**——OP-TEE 的 `CFG_INSECURE` 桩函数 `tee_otp_get_hw_unique_key()` | HUK 来自熔丝/OTP，只有安全世界能读。所有安全存储密钥都由它派生。在 QEMU 上，任何拿到存储文件和 OP-TEE 源码的人都能解密它们。 |
| 安全内存隔离是模拟的 | TZASC/TZC-400 把 DRAM 区域标记为安全；总线拒绝普通世界的访问。TZPC 对外设做同样的事。 |
| 只有 REE FS，可以回滚（§3.2） | eMMC 上的 RPMB，或其他防回放的存储。 |
| 没有 secure boot | ROM 里的 BL1 验证 BL2，BL2 验证 BL31/BL32/BL33（TBBR 链）——见 [secure-boot](secure-boot.zh.md)。没有它，谁替换了 BL32，谁就拥有 TEE 持有的每一把密钥。 |

**在 Raspberry Pi 5 上、没有 TEE 时的同一思路。** Pi 5 的固件可以在 OTP 里持有一把设备唯一的 ECDSA P-256 密钥，
用它签名或做 HMAC 而不交出它（`rpi-fw-crypto sign|hmac`），这些操作可以锁定到下次重启。这和这个 TA 是同一种接口——
"用密钥，但永远看不到它"——只有一个根本区别，Raspberry Pi 自己的文档里写着：运行在 Arm 内核里的代码可以直接读 OTP。
TrustZone TEE 连普通世界的内核也能挡住；Pi 的固件服务只能挡住用户态。这个怎么用于磁盘加密，见
[integrity-and-encryption](integrity-and-encryption.zh.md)。

## 6. 复现

```bash
mkdir ~/optee && cd ~/optee
repo init -u https://github.com/OP-TEE/manifest.git -m qemu_v8.xml -b refs/tags/4.10.0
repo sync -j4 --no-clone-bundle
ln -s <this repo>/security/optee-ta/device_identity optee_examples/device_identity
cd build && make toolchains && make -j$(nproc) all
make check-only                          # xtest
<this repo>/security/optee-ta/device_identity/run-demo.sh
```

在没有 `libgnutls28-dev` 的 Ubuntu 24.04 上，U-Boot 的主机工具 `mkeficapsule` 会编译失败；这里用不到它，在 `make`
命令行上往 `UBOOT_DEFCONFIG_FILES` 里加一个带 `CONFIG_TOOLS_MKEFICAPSULE=n` 的配置片段即可避开。
