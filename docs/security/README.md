# Security

The device in this repository was built for bring-up and measurement.
This directory is the work of turning it into something that could be
deployed: an audit first, then one document per group of fixes, each
pointing back to the findings it closes.

| Document | Covers | Findings |
| --- | --- | --- |
| [threat-model.md](threat-model.md) | Assets, attackers, trust boundaries, the audit, and the order of work | all |
| [hardening.md](hardening.md) | Production image: accounts, SSH, serial console, read-only rootfs, service sandboxing, CVE triage | F1, F2, F3, F5, F11 |
| [secure-boot.md](secure-boot.md) | Raspberry Pi 5 chain of trust, signing with a key in an HSM, the OTP step not taken | F4 |
| [optee.md](optee.md) | Device identity key in an OP-TEE trusted application (QEMU Armv8-A) | F12 |
| [integrity-and-encryption.md](integrity-and-encryption.md) | dm-verity for the rootfs, LUKS for data, and where the key can live on this board | F5, F7, F8 |
| [update-and-provisioning.md](update-and-provisioning.md) | Signed A/B updates on the Pi 5 firmware's tryboot (tested on the board: failed update rolled back, healthy one committed), anti-rollback, per-device provisioning | F6, F14 |
| [device-authentication.md](device-authentication.md) | Authenticating the MCU on the SPI link: challenge–response with a per-MCU key, measured on the board; read-out protection level 1 | F9, F10 |
| [remote-monitoring.md](remote-monitoring.md) | Read-only status over mutual TLS 1.3 from a separate, network-only process; Device CA + a new Operator CA in the HSM, CRL that fails closed | M7 (not a finding) |

Code:

- [`security/signing/`](../../security/signing/) — PKCS#11 signing: token setup
  and an HSM wrapper for Raspberry Pi's signing tools.
- [`security/optee-ta/`](../../security/optee-ta/) — the device identity TA
  and its normal-world client.
- [`security/cve/`](../../security/cve/) and [`security/hardening/`](../../security/hardening/) —
  kernel CVE triage, binary and kernel-config hardening reports.
- [`security/integrity/`](../../security/integrity/) — the dm-verity tamper demo.
- [`security/device-auth/`](../../security/device-auth/) — the MCU challenge–response verifier.
- [`yocto/meta-device-platform/`](../../yocto/meta-device-platform/) —
  `device-platform-image-prod` and the recipes it adds.
- [`yocto/meta-device-platform-verity/`](../../yocto/meta-device-platform-verity/) —
  the signed A/B image: dm-verity root, LUKS2 data partition, RAUC tryboot
  backend, health check, hardened 6.12 kernel.

Raw output of every run cited in these documents: [`results/security/`](../../results/security/).
Case studies from this work: [case 10](../debugging/case-10-dm-verity-one-byte-on-the-card.md)
(dm-verity on the card) and [case 11](../debugging/case-11-ab-update-four-failures-invisible-on-the-build-host.md)
(A/B updates on the board).

**Limits of the hardware**, stated in each document where they matter:
the Raspberry Pi 5 has no TPM or secure element, the STM32F103 has no
TrustZone or secure storage, and OP-TEE has no Raspberry Pi 5 port.
Irreversible steps (programming the Pi 5's OTP key hash, STM32 RDP level
2) are documented and not performed.
