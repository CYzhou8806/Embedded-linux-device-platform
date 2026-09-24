# A device identity key in OP-TEE

Closes finding **F12** of the [threat model](threat-model.md) — "nothing on
the device can prove to a backend which unit it is" — with a trusted
application that holds one private key and uses it only to answer
challenges. Code: [`security/optee-ta/device_identity/`](../../security/optee-ta/device_identity/).
Raw output of every run below: [`results/security/optee/`](../../results/security/optee/).

## 1. Why this runs on QEMU and not on the Raspberry Pi 5

OP-TEE has no maintained Raspberry Pi 5 port (the old Raspberry Pi 3 port
was a demonstration without secure memory isolation and is unmaintained).
The TA was therefore built and run on OP-TEE's own reference target,
**QEMU `virt` Armv8-A with `secure=on`**, which is where OP-TEE itself is
developed and tested. Everything in §3 is independent of the board; what
*does* depend on the board is listed in §5.

Stack, built with OP-TEE's manifest `qemu_v8.xml` at release **4.10.0**:
TF-A (BL1/BL2/BL31) → OP-TEE OS as BL32 → U-Boot as BL33 → Linux +
buildroot, with `tee-supplicant` and `libteec` in the normal world.

The build itself was checked first with OP-TEE's test suite:

```
xtest: 143 test cases of which 0 failed (41358 subtests), 11 min under QEMU
```

## 2. How a call reaches the TA

```
 normal world (EL0)   devid ── libteec ── ioctl(/dev/tee0)
 normal world (EL1)             Linux TEE driver (optee)
                                    │ SMC
 EL3                              TF-A BL31 (secure monitor) ── world switch
 secure world (S-EL1)             OP-TEE OS ── loads/verifies the TA, owns its memory
 secure world (S-EL0)             device_identity TA ── key object in secure storage
                                    │ RPC back to normal world for file I/O
 normal world                     tee-supplicant ── /var/lib/tee/*  (encrypted, MACed)
```

Two consequences that are easy to miss:

- **The normal world still stores the TA's data.** OP-TEE's REE FS keeps
  secure storage as files on the Linux filesystem, written through
  `tee-supplicant`. They are encrypted and integrity-protected with keys
  derived from the hardware unique key, but *the normal world can delete,
  corrupt or roll them back* (§3.3, §3.4).
- **Parameters live in shared memory.** A memref passed to the TA points
  into memory the normal world can keep writing while the TA runs. The TA
  copies the nonce once and only uses the copy, so what is checked is what
  is hashed (no double fetch).

## 3. The TA and what the run showed

The TA holds one ECDSA P-256 key and has three real commands:

| Command | Does |
| --- | --- |
| `PROVISION` | Generates the key **inside the TA** (`TEE_GenerateKey`), restricts it to `TEE_USAGE_SIGN` without `TEE_USAGE_EXTRACTABLE`, stores it as a persistent object, returns the public key. Refuses if a key exists. |
| `GET_PUBKEY` | Returns the public key. |
| `SIGN_CHALLENGE` | Signs `SHA-256("device-platform/devid-challenge/v1" ‖ nonce)`, 16–64-byte nonces only. |

The fixed domain string is deliberate: the TA never signs a digest the
caller chose, so root on the normal world — which *can* call the TA — can
use it to prove it is this device, but not to sign an update, a
certificate request, or anything else that happens to be a SHA-256 value.

A verifier on another machine ([`verify.py`](../../security/optee-ta/device_identity/verify.py))
makes the nonce and checks the answer with nothing but the public key
recorded at provisioning. One run, driven headless by
[`run-demo.sh`](../../security/optee-ta/device_identity/run-demo.sh):

| # | Step | Result |
| --- | --- | --- |
| 1 | `GET_PUBKEY` before provisioning | `0xffff0008` ITEM_NOT_FOUND |
| 2 | `PROVISION` | public key `04063dbc…dec9` |
| 3 | `PROVISION` again | `0xffff0003` ACCESS_CONFLICT |
| 4 | Sign the verifier's 32-byte nonce | verifier: **VALID**; same signature against a different nonce: **INVALID** |
| 5 | 8-byte nonce | `0xffff0006` BAD_PARAMETERS |
| 6 | TA tries to read its own private key | **TA panics** (`0xffff3024` TARGET_DEAD to the client) |
| 7 | `GET_PUBKEY` after the panic | same key |
| 8 | Look at `/var/lib/tee` | 37 files owned by user `tee`, contents are ciphertext |
| 9 | **Roll back** storage to a copy taken before step 2 | `GET_PUBKEY` → ITEM_NOT_FOUND; `PROVISION` succeeds and creates a **different** identity |
| 10 | Flip one byte in every storage file | `0xf0100001` CORRUPT_OBJECT |

### 3.1 The key is not extractable — not even by the TA

Step 6 is a command that exists only to test this: it calls
`TEE_GetObjectBufferAttribute(TEE_ATTR_ECC_PRIVATE_VALUE)` on the stored
key. The GlobalPlatform API requires a panic when a protected attribute is
read from an object without `TEE_USAGE_EXTRACTABLE`; OP-TEE's log shows
`TA panicked with code 0xffff0006`, the TA is torn down, and the key is
still there when it is reloaded (step 7). The property is enforced by the
TEE core, not by the TA's own discipline.

### 3.2 Only one identity per device — as long as storage can't be rolled back

Step 3 shows the TA refusing to replace its key. Step 9 shows why that
check is **not enough**: restoring an older copy of `/var/lib/tee` —
something any root process in the normal world can do — makes OP-TEE
accept the older, key-less state as valid, and the "only once" rule then
happily creates a second identity.

OP-TEE says so itself, at boot, in the secure-world log:

```
I/TC: WARNING (insecure configuration): Failed to get monotonic counter for REE FS, using 0
```

This is the behaviour OP-TEE documents for its REE file system without
RPMB: each file is encrypted and authenticated, but there is nothing the
normal world can't reset that records *which version* is current. With
`CFG_RPMB_FS=y` and `CFG_REE_FS_INTEGRITY_RPMB`, the hash of the
directory file is kept in an eMMC RPMB partition, whose write counter is
maintained by the eMMC and whose writes need a key the normal world
doesn't have — and the rollback in step 9 would be detected. QEMU has no
eMMC, but `tee-supplicant` can emulate one.

**The same run with RPMB (emulated).** OP-TEE core rebuilt with
`CFG_RPMB_FS=y CFG_RPMB_TESTKEY=y CFG_RPMB_WRITE_KEY=y` (27 s; the TA and
the normal world unchanged), same demo script
([record](../../results/security/optee/rpmb-emulated/)):

| | REE FS only | REE FS + RPMB (emulated) |
| --- | --- | --- |
| boot log | `Failed to get monotonic counter for REE FS, using 0` | gone; instead `RPMB: Using test key`, `Auth key not yet written` → key written → `Found working RPMB device` |
| steps 1–8 (provision, refuse re-provision, sign, key not extractable, survives TA panic) | as designed | identical |
| **step 9: old copy of `/var/lib/tee` put back** | accepted; `provision` creates a **second identity** | **refused**: `TEEC_OpenSession: 0xf0100001` (`TEE_ERROR_CORRUPT_OBJECT`); no second identity |
| step 10: one byte flipped in every file | refused (`CORRUPT_OBJECT`) | refused (`CORRUPT_OBJECT`) |

The secure-world log shows *where* it failed: OP-TEE reads the hash of
the directory file from RPMB (`fh->filename=/dirfile.db.hash`), the
restored `dirf.db` doesn't match it, and the whole store is refused — so
completely that the TA can't even be loaded any more
(`ldelf_syscall_open_bin … (Secure Storage TA) res=0xf0100001`). The
rollback is detected and the device **fails closed**: the attacker gets
no second identity, but the device loses its identity and every other
object in secure storage until it is re-provisioned. Detection turns a
forgery into denial of service, the same trade as §3.3.

Why this is a demonstration of the mechanism and not of the property:

- **The "RPMB" is a data structure inside `tee-supplicant`**, a
  normal-world process. What it protects against is exactly the attacker
  who rewrites files in `/var/lib/tee` — but that same attacker (root in
  the normal world) can restart `tee-supplicant` and get a blank "RPMB".
  On a real eMMC the counter and the authentication key live in the
  flash controller, out of reach of the normal world.
- **`CFG_RPMB_TESTKEY=y`**: the RPMB key is a fixed test key, not derived
  from a hardware-unique key, and the debug log prints it. With
  `CFG_RPMB_WRITE_KEY=y` OP-TEE programs that key into any RPMB it finds
  unprogrammed — on a production line that has to happen exactly once, in
  a trusted environment, because RPMB keys are one-time programmable.
- The Raspberry Pi 5 boots from an SD card, which has no RPMB at all.
  The design needs eMMC (or UFS) to become real.

The general lesson is that **any policy a TEE enforces through storage is
only as strong as that storage's rollback protection** — the same
argument as anti-rollback for firmware updates
([update-and-provisioning](update-and-provisioning.md)).

### 3.3 Corruption is detected — and turns into denial of service

Step 10 flips one byte in each storage file. OP-TEE refuses the data
(`TEE_ERROR_CORRUPT_OBJECT`, and the session can't even be opened), and
its REE FS code removes files it finds corrupt (`ree_fs_open_primitive:
Remove corrupt file` in the secure-world log — the same line also appears
once during boot, before any tampering, so it is not unique to this test). Integrity holds: nothing forged gets
through. Availability doesn't: anyone who can write `/var/lib/tee` can
destroy the device's identity. A product has to plan for that —
re-provisioning, or a backend that can tell "identity lost" from "device
replaced".

## 4. What a TA is and isn't good for

Good fits, because the secret never needs to leave: device identity and
attestation keys, key derivation for disk encryption, a monotonic
counter, verifying something with a key the normal world must not
replace.

Poor fits: anything large or complex (more code in the TCB), anything
that needs drivers the secure world doesn't have, and anything where the
*decision* is made in the normal world anyway — a TA that returns "valid"
to a normal-world program that can ignore the answer protects nothing.

What a TEE does not protect against: bugs in the TA or in OP-TEE itself
(both are part of the trusted computing base), side channels on shared
caches, a compromised secure-boot chain (whoever controls BL31/BL32
controls the TEE), and — as §3.2/§3.3 show — the normal world's control
over storage.

## 5. What would change on real hardware

| On QEMU | On a real SoC |
| --- | --- |
| Hardware unique key is a **constant (all zeros)** — OP-TEE's `CFG_INSECURE` stub, `tee_otp_get_hw_unique_key()` | HUK from fuses/OTP, readable only by the secure world. All secure-storage keys are derived from it. On QEMU, anyone with the storage files and OP-TEE's source can decrypt them. |
| Secure memory separation is emulated | TZASC/TZC-400 marks DRAM regions secure; the bus rejects normal-world accesses. TZPC does the same for peripherals. |
| REE FS only, rollback possible (§3.2) | RPMB on eMMC, or another replay-protected store. |
| Secure boot absent | BL1 in ROM verifies BL2, which verifies BL31/BL32/BL33 (TBBR chain) — see [secure-boot](secure-boot.md). Without it, whoever replaces BL32 owns every key the TEE holds. |

**The same idea on the Raspberry Pi 5, without a TEE.** The Pi 5's
firmware can hold a device-unique ECDSA P-256 key in OTP and sign or HMAC
with it without handing it out (`rpi-fw-crypto sign|hmac`), with the
operations lockable until the next reboot. It is the same interface as
this TA — "use the key, never see it" — with one fundamental difference,
stated in Raspberry Pi's own documentation: code running in the Arm
kernel can read the OTP directly. A TrustZone TEE hides its keys from the
normal-world kernel as well; the Pi's firmware service can only hide them
from userspace. See [integrity-and-encryption](integrity-and-encryption.md)
for how that is used for disk encryption.

## 6. Reproducing

```bash
mkdir ~/optee && cd ~/optee
repo init -u https://github.com/OP-TEE/manifest.git -m qemu_v8.xml -b refs/tags/4.10.0
repo sync -j4 --no-clone-bundle
ln -s <this repo>/security/optee-ta/device_identity optee_examples/device_identity
cd build && make toolchains && make -j$(nproc) all
make check-only                          # xtest
<this repo>/security/optee-ta/device_identity/run-demo.sh
```

On Ubuntu 24.04 without `libgnutls28-dev`, U-Boot's `mkeficapsule` host
tool fails to build; it isn't needed here, and adding a config fragment
with `CONFIG_TOOLS_MKEFICAPSULE=n` to `UBOOT_DEFCONFIG_FILES` on the
`make` command line avoids it.
