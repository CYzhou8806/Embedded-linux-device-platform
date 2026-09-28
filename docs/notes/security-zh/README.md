# 安全（中文译本索引）

> 译自 [`docs/security/README.md`](../../security/README.md)。这个目录下的文件都是
> `docs/security/*.md` 的中文译本，供复习用，**以英文原版为准**。

本仓库里的设备原本是为 bring-up 和性能测量构建的。这个目录记录的是把它变成
"可以部署的东西"的过程：先审计，再每组修复写一篇文档，每篇都回指它关闭的发现。

| 文档 | 内容 | 对应发现 |
| --- | --- | --- |
| [threat-model.zh.md](threat-model.zh.md) | 资产、攻击者、信任边界、审计本身、工作顺序 | 全部 |
| [hardening.zh.md](hardening.zh.md) | 产线镜像：账户、SSH、串口控制台、只读 rootfs、服务沙箱、CVE 分诊 | F1, F2, F3, F5, F11 |
| [secure-boot.zh.md](secure-boot.zh.md) | Raspberry Pi 5 的信任链、用 HSM 里的密钥签名、没有执行的 OTP 那一步 | F4 |
| [optee.zh.md](optee.zh.md) | 在 OP-TEE 可信应用里的设备身份密钥（QEMU Armv8-A） | F12 |
| [integrity-and-encryption.zh.md](integrity-and-encryption.zh.md) | rootfs 用 dm-verity、数据用 LUKS，以及这块板子上密钥能放在哪 | F5, F7, F8 |
| [update-and-provisioning.zh.md](update-and-provisioning.zh.md) | 基于 Pi 5 固件 tryboot 的签名 A/B 更新（板上测过：失败的更新回滚、健康的提交）、防回滚、逐台设备 provisioning | F6, F14 |
| [device-authentication.zh.md](device-authentication.zh.md) | 在 SPI 链路上认证 MCU：每个 MCU 一把密钥的挑战-应答，板上实测；读保护 level 1 | F9, F10 |

代码：

- [`security/signing/`](../../../security/signing/) —— PKCS#11 签名：token 初始化，
  以及给 Raspberry Pi 签名工具用的 HSM wrapper。
- [`security/optee-ta/`](../../../security/optee-ta/) —— 设备身份 TA 及其普通世界客户端。
- [`yocto/meta-device-platform/`](../../../yocto/meta-device-platform/) ——
  `device-platform-image-prod` 以及它新增的 recipe。

**硬件的局限**（在每篇文档相关处都有说明）：Raspberry Pi 5 没有 TPM 或安全元件，
STM32F103 没有 TrustZone 或安全存储，OP-TEE 没有 Raspberry Pi 5 的移植。
不可逆的步骤（烧写 Pi 5 的 OTP 密钥哈希、STM32 RDP level 2）只写文档，不执行。
