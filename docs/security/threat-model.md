# Threat model and security audit

This is an audit of the device this repository builds, **as it is today**,
done before changing anything. It decides the order the security work is
done in: every later document in `docs/security/` fixes one or more of the
findings below and links back to them by ID.

The current image was built for bring-up and measurement, not for
deployment, so most of what follows is expected. The point of writing it
down is to have a ranked list instead of a feeling, and a baseline to
measure each fix against.

## 1. System and deployment scenario

```
                         ┌──────────── physical enclosure ────────────┐
                         │                                            │
  network (WiFi, LAN) ◄──┼── Raspberry Pi 5 (Yocto image)             │
  ─ SSH (dropbear :22)   │     boot partition (FAT): firmware,        │
                         │       config.txt, cmdline.txt, kernel, DTBs│
                         │     rootfs (ext4, read-write)              │
                         │     device-service (root) ─► devbus shm ─► │ consumers
                         │     custom-acq driver, /dev/acq0           │
                         │          │ SPI + DATA_READY GPIO           │
                         │          ▼                                 │
                         │     STM32F103 acquisition MCU              │
                         │                                            │
  service access ────────┼─► SD card slot, UART debug header,         │
                         │   MCU SWD header, USB ports                │
                         └────────────────────────────────────────────┘
```

The scenario the model assumes is a **connected appliance installed at a
customer site**: it measures something (temperatures, process data),
uploads it, and is serviced on site by technicians who are not employees
of the manufacturer. The device is out of the manufacturer's physical
control for its whole life, including resale and disposal.

That scenario matters more than any single technology choice: it means
**physical access is part of the normal lifecycle, not an exotic attack**.

## 2. Assets

| Asset | Why it matters | Property needed |
| --- | --- | --- |
| Acquired data (samples, metrics) | Process/quality records the customer relies on | Integrity first, then confidentiality |
| Firmware and OS image | Controls the whole device; a modified image is a persistent compromise | Integrity, authenticity |
| Device identity | Lets a backend trust what a device reports | Authenticity, uniqueness |
| Signing keys (future) | Whoever holds them can ship code to every device | Confidentiality — the single most valuable secret |
| Network credentials (WiFi PSK) | Access to the customer's network, not just this device | Confidentiality |
| Configuration | Sample rate, backpressure thresholds — changes what gets recorded | Integrity |
| Logs | Debugging and forensics after an incident | Integrity, availability |

## 3. Attackers

| Attacker | Access | Realistic goal |
| --- | --- | --- |
| **A1 — network attacker** | Same LAN/WLAN as the device, no physical access | Take over the device, pivot into the customer network |
| **A2 — service technician / insider** | Physical access for minutes to hours; can open the enclosure, pull the SD card, attach a UART or SWD probe | Tamper with records, extract credentials, install modified firmware, clone the device |
| **A3 — second-hand buyer / disposal** | Unlimited physical access to a decommissioned unit | Read the previous owner's data and credentials |
| **A4 — supply chain** | Build host, third-party layers, upstream packages | Get code into the image before it is signed |

Remote attackers (A1) set the priority because they scale. Physical
attackers (A2, A3) set the design, because in this scenario they are
guaranteed to exist.

## 4. Trust boundaries

1. **Network ↔ device** — SSH is the only listening service.
2. **Boot media ↔ boot chain** — everything on the SD card, including the
   first file the kernel is loaded from, is trusted today without being
   checked.
3. **Kernel ↔ userspace** — `/dev/acq0` and sysfs knobs; the driver
   validates what it writes to the MCU.
4. **Process ↔ process** — `device-service` publishes into a devbus
   shared-memory segment that subscribers map.
5. **Linux ↔ MCU** — the SPI register protocol. Frames are echo-verified
   for transport errors, but nothing proves *which* device is answering.
6. **Build host ↔ image** — upstream layers and the local configuration
   (including the WiFi PSK) flow into the image unchecked.

## 5. Findings

Severity = how easily the attacker in the "who" column can do it × what
it gets them. **Critical** means remotely exploitable with no
credentials; **High** means full compromise of an asset with physical
access alone; **Medium** means a real weakness that needs a precondition
or has a bounded impact; **Low** means defence in depth.

| ID | Finding | Who | Severity | Fixed in |
| --- | --- | --- | --- | --- |
| **F1** | `debug-tweaks` is enabled: `root` has an empty password and dropbear accepts root logins with empty passwords. Anyone on the network owns the device. | A1 | **Critical** | [hardening](hardening.md) |
| **F2** | No vulnerability management: nobody knows which known CVEs the image ships. | A1 | **High** | [hardening](hardening.md) (`cve-check`) |
| **F3** | A root login is available on the serial console (`ttyAMA10`, the Pi 5 UART header), also with an empty password. | A2 | **High** | [hardening](hardening.md) (production image has no getty) |
| **F4** | No verified boot. `config.txt`, `cmdline.txt`, the kernel and the device trees on the FAT boot partition can be replaced by anyone who can remove the SD card. | A2, A4 | **High** | [secure-boot](secure-boot.md) |
| **F5** | The root filesystem is read-write and its integrity is never checked. A modified binary survives reboots unnoticed. | A1 (after F1), A2 | **High** | [hardening](hardening.md) (read-only), [integrity-and-encryption](integrity-and-encryption.md) (dm-verity) |
| **F6** | No update mechanism at all, and therefore no anti-rollback. Every other finding here is permanent on devices in the field. | all | **High** | [update-and-provisioning](update-and-provisioning.md) |
| **F7** | The WiFi PSK is stored in plaintext in `/etc/wpa_supplicant/` on an unencrypted rootfs. Pulling the SD card gives the customer's network credentials. | A2, A3 | **Medium** | [integrity-and-encryption](integrity-and-encryption.md) |
| **F8** | No encryption of data at rest. Records and logs of a decommissioned device are readable by whoever gets it. | A3 | **Medium** | [integrity-and-encryption](integrity-and-encryption.md) |
| **F9** | No device authentication on SPI. Any MCU that speaks the register protocol is accepted as the acquisition peripheral, so the data can be spoofed at the source. | A2 | **Medium** | [device-authentication](device-authentication.md) |
| **F10** | The MCU's SWD port is open and flash readout protection is off (RDP level 0): its firmware can be read out and replaced. | A2 | **Medium** | [device-authentication](device-authentication.md) (RDP level 1; limits stated there) |
| **F11** | `device-service` runs as root with no systemd sandboxing, although it only needs `/dev/acq0`, one sysfs attribute and its shared-memory segment. | A1 (after another bug) | **Medium** | [hardening](hardening.md) |
| **F12** | No device identity. Nothing on the device can prove to a backend which unit it is. | A2 | **Medium** | [optee](optee.md), [update-and-provisioning](update-and-provisioning.md) |
| **F13** | devbus subscribers map the shared-memory segment read-write, so a compromised subscriber can alter payloads other subscribers are reading. The publisher already treats indices coming back from subscribers as untrusted; payload integrity between subscribers is not protected, and the file mode (`0660`) is the only boundary. | A1 (after another bug) | **Low** | documented; see §7 |
| **F14** | The build host holds the only copy of the WiFi PSK in a plain file, and there are no signing keys yet, so there is nothing that keeps a future key out of build scripts. | A4 | **Low** | [update-and-provisioning](update-and-provisioning.md) (keys in a PKCS#11 token) |

What is **not** a finding, checked as part of the audit:

- Driver input: `sample_rate` is range-checked before it is sent to the
  MCU, `control` is masked to one bit, and `read()` only hands out whole
  samples through `kfifo_to_user()`. The writable module parameters and
  sysfs attributes are root-only (`0644` / `DEVICE_ATTR_WO`).
- The SPI protocol echo-verifies every frame, so transport corruption is
  detected — but that protects against noise, not against an adversary
  (F9).

## 6. Order of work, and why it is not simply "highest severity first"

The fix order combines severity with **dependency** and **cost**:

1. **F1, F2, F3, F11 — Yocto hardening.** Critical and cheap, and all of
   them are image configuration. A remote root login makes every other
   control irrelevant, so this goes first even though it is the least
   interesting work.
2. **F5 (dm-verity) and F7/F8 (encryption).** Integrity of the rootfs is
   what makes "the image we signed is the image that runs" true after
   the kernel has started.
3. **F4 — secure boot.** It anchors F5: dm-verity's root hash is only
   trustworthy if the kernel and command line that carry it were
   verified. It comes after dm-verity in the work order only because the
   last step on this hardware (burning the key hash into OTP) is
   irreversible and is deliberately not performed — see
   [secure-boot](secure-boot.md).
4. **F12 — device identity in a TEE**, and **F9/F10 — device
   authentication on SPI**, which reuses that identity.
5. **F6, F14 — signed updates and key handling.** Without them every
   fix above is frozen at the version that left the factory.

## 7. Limits of this hardware, stated up front

These are not fixed by any of the work above and are called out wherever
they matter:

- **The Raspberry Pi 5 has no TPM and no secure element.** Any disk
  encryption key has to live somewhere an attacker with the SD card and
  enough time can reach, unless it is bound to a verified boot state that
  this board can only provide after an irreversible OTP programming step.
- **The STM32F103 has no TrustZone, no crypto accelerator and no secure
  storage.** RDP level 1 raises the bar; it is not a security boundary.
  Level 2 is permanent and is not used on the only MCU in the project.
- **OP-TEE has no maintained Raspberry Pi 5 port.** The TEE work runs on
  QEMU (Armv8-A), which is where it is normally developed anyway.
- **F13** is a property of zero-copy shared memory, not a bug: making the
  payload read-only for subscribers needs a separate read-only mapping
  per subscriber, which is a design change to devbus and out of scope
  for this audit.

## 8. After the work

The same fourteen findings, re-audited once every document in this
directory had been done and — where the board allows — checked on the
Raspberry Pi 5 running `device-platform-image-ab`. "Closed" means the
fix was seen working on the device, including the negative test (the
access that should fail was tried and failed). "Partly" means the fix
exists but one link is missing. "Limited by hardware" means what's left
needs something this board doesn't have, or an irreversible step that
isn't taken on the project's only board.

| ID | Before | Now | Evidence | What's left |
| --- | --- | --- | --- | --- |
| **F1** | root, empty password, over SSH | **closed** | [hardening](hardening.md) §5: root with the admin key → `Permission denied (publickey)`; the server offers no password method; forwarding `administratively prohibited` ([record](../../results/security/hardening/first-boot-prod.txt)) | — |
| **F2** | no CVE tracking | **partly** | [hardening](hardening.md) §4: userspace 16 → 4, each with a recorded reason; kernel 3,853 → 3,602 after CNA + compiled-file triage ([summary](../../results/security/cve/kernel-triage-summary.txt)) | The kernel is 94 stable releases behind; the fix is a process (follow stable), not triage |
| **F3** | root login on the UART header | **closed** | [hardening](hardening.md) §5: `serial-getty@ttyAMA10` masked/inactive on the board | HDMI/USB keyboard console not re-audited beyond "no password can match" |
| **F4** | unverified boot partition | **limited by hardware** | [secure-boot](secure-boot.md): boot key in the HSM, signed/counter-signed images verified on the host, a signed `boot.img` **booted the board** (§3.4) | The firmware doesn't check signatures until the key hash is in OTP (irreversible, not done). Since 1.1.0 every A/B slot and every update carries a signed `boot.img` ([secure-boot](secure-boot.md) §3.5) |
| **F5** | rootfs read-write, never checked | **closed** (integrity) / partly (authenticity) | read-only root ([hardening](hardening.md) §5); dm-verity: one byte changed on the card → `EIO` for exactly the predicted block ([case 10](../debugging/case-10-dm-verity-one-byte-on-the-card.md)); verity cost 16 % ([integrity](integrity-and-encryption.md) §5) | The root hash is only as trustworthy as the kernel that carries it — F4 |
| **F6** | no updates, no anti-rollback | **closed** (updater) / limited (card) | [update-and-provisioning](update-and-provisioning.md) §3.1: signed A/B via tryboot on the board; a real bad update rolled back after one power cycle; a good one committed by the health check; 0.9.0 refused by `min-bundle-version` ([record](../../results/security/update/on-target-ab.txt)) | Someone with the SD card can still write an old signed image directly — needs a monotonic counter (OTP bits / RPMB / TPM) |
| **F7** | WiFi PSK plaintext on the card | **partly** | LUKS2 data partition on the board ([integrity](integrity-and-encryption.md) §5); SSH host key and RAUC state already live there | The PSK is still in the rootfs image, and the LUKS key is a stand-in derived from the serial number — **against A3 this is not protection yet** (§3 of that document says where the real key goes). Since 1.1.0 the partition also has integrity (HMAC per sector; a changed byte → `EIO`, [§6](integrity-and-encryption.md)) |
| **F8** | no encryption at rest | **partly** | same as F7: encryption mechanism real and measured (−7 %) | Same key problem; data written by device-service isn't directed to `/data` yet |
| **F9** | any MCU accepted | **partly** | [device-authentication](device-authentication.md) §6: HMAC challenge–response in firmware v1.4, 110/110 valid, wrong key always rejected, no sample lost during 100 back-to-back authentications | Since 1.2.0 it gates `device-service` (and so every update's commit) — [§7](device-authentication.md); still a file key, not in the driver, and no MAC per frame, so an interposer after authentication isn't stopped |
| **F10** | SWD open, RDP 0 | **limited by hardware** | RDP 1 shown to stop the debugger reading the key, and removing it mass-erases ([record](../../results/security/device-auth/rdp-level1-demo.txt)) | F103 RDP 1 has a public bypass (CVE-2020-8004); RDP 2 is permanent and not used; the MCU was left at RDP 0 for development |
| **F11** | device-service root, unconfined | **closed** | exposure 9.4 → 1.8; on the board it runs as user `acq`, `CapEff = 0x804000`, seccomp, own network namespace, no samples lost | — |
| **F12** | no device identity | **limited by hardware** | identity key generated inside an OP-TEE TA (QEMU), certificate from the HSM Device CA via a provisioning station with proof of possession ([optee](optee.md), [provisioning](update-and-provisioning.md) §5) | No TEE on the Pi 5; without RPMB, rolling back TEE storage produced a second identity ([optee](optee.md) §3.2) |
| **F13** | devbus payload writable by subscribers | **open, by design** | unchanged; documented in §7 | Needs per-subscriber read-only mappings — a devbus design change |
| **F14** | no key handling | **closed** | four keys, generated non-extractable in a PKCS#11 token, PIN never in argv ([update-and-provisioning](update-and-provisioning.md) §1); the build only ever holds a development key | SoftHSM is a software stand-in: backup and physical protection are what a real HSM adds |

Seven of the fourteen are fully or mostly closed on the board. Every
"limited by hardware" row comes down to the same missing piece — **a
secret or a counter the attacker with the SD card can't reach**: the OTP
key hash (F4), the OTP device key (F7, F8, F9's pairing key), a monotonic
counter (F6, F12). That is the one sentence to take from this table: on
this board the software side of each control is done, and the root of
trust is the part that is simulated.

### New findings, from doing the work

The audit in §5 was done by reading the image. These came out of
building and running the fixes, and none was on the original list:

| ID | Finding | Status |
| --- | --- | --- |
| **F15** | With a read-only root, the SSH host key and `/etc/machine-id` are regenerated at every boot; the changing host key trains administrators to accept any fingerprint (a MITM's precondition), and the DHCP client ID derived from machine-id changed the IP | closed: host key on `/data`, `ClientIdentifier=mac` — same key and IP across reboots and across A/B slots |
| **F16** | Remote access depended on the newest component (the data partition); one missing BusyBox applet killed SSH on a device with no password login | closed: host key falls back to `/run`; a boot recorder keeps evidence on partition 1 ([update-and-provisioning](update-and-provisioning.md) §3.1) |
| **F17** | A systemd ordering cycle silently deleted the health-check job at every boot — the gate the A/B design relies on did not exist | closed: started by a timer; `journalctl -b \| grep "ordering cycle"` is now part of the checks |
