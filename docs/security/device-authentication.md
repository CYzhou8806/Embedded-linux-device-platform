# Authenticating the MCU on the SPI link — design

Addresses findings **F9** (any MCU that speaks the register protocol is
accepted) and **F10** (the MCU's SWD port and flash are open) of the
[threat model](threat-model.md).

**Status: implemented and measured on the board** (§6), with two
deliberate differences from the design below, stated in §6.4.
Code: firmware [`Core/Src/devauth.c`](../../v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/Core/Src/devauth.c)
(v1.4), driver sysfs `auth_challenge` / `auth_response` / `auth_cycles`,
[`tools/pair-mcu.sh`](../../tools/pair-mcu.sh),
[`security/device-auth/mcu-auth-check.py`](../../security/device-auth/mcu-auth-check.py).
Raw output: [`results/security/device-auth/`](../../results/security/device-auth/).

## 1. What this protects against, and what not

The attacker is the service technician or second-hand buyer from the
threat model (A2/A3), who replaces the acquisition board with one that
speaks the same protocol and reports whatever values it likes —
temperatures that were never measured, a process record that was never
real. Today nothing stops that: the driver checks `DEVICE_ID`, which any
clone can return.

| Attack | Stopped by this design? |
| --- | --- |
| A different or counterfeit acquisition board | **Yes** — it can't answer the challenge without the paired key |
| Replaying a recorded authentication exchange | **Yes** — every challenge carries a fresh nonce |
| Reading the key out of the genuine MCU, then cloning it | **Raised bar only** — see §4 |
| An interposer on the SPI bus *after* authentication, altering samples | **No** — that needs a MAC per frame or per batch (§5) |

## 2. The exchange

Challenge–response with a key shared between one Pi and one MCU, over the
existing register protocol (5-byte frames: command byte = write flag + 7-bit
register address, then a 32-bit value; the reply to frame N arrives in
frame N+1):

```
Pi (driver, at probe and after every MCU reset)          MCU
  nonce = 16 random bytes (kernel RNG)
  write REG_AUTH_NONCE0..3            ─────────────►  store nonce
  write REG_AUTH_CTRL = START         ─────────────►  in the main loop, not the SPI ISR:
                                                        mac = HMAC-SHA256(K,
                                                          "acq-auth-v1" ‖ DEVICE_ID ‖
                                                          FW_VERSION ‖ nonce)
  poll REG_STATUS.AUTH_DONE           ◄─────────────  set AUTH_DONE
  read REG_AUTH_MAC0..3 (first 16 bytes of mac)
  recompute and compare in constant time
  only then register /dev/acq0
```

- **New registers** in the free range after `REG_SPI_ERROR_COUNT` (0x0A):
  four nonce words, a control register, four MAC words, and two status
  bits (`AUTH_BUSY`, `AUTH_DONE`) in `REG_STATUS`.
- **Computed outside the ISR.** The SPI interrupt must keep re-arming
  within the frame gap (the lesson of case-01 and case-02: this link is
  sensitive to anything that delays the ISR). The MAC is computed in the
  main loop and signalled through a status bit.
- **`DEVICE_ID` and `FW_VERSION` inside the MAC**, so an answer from one
  firmware can't be presented as coming from another.
- **Truncated to 128 bits**: four register reads instead of eight; 128
  bits of an HMAC is far beyond what online guessing over SPI can reach.
- **Who verifies**: the kernel driver, before it registers `/dev/acq0`.
  Userspace never sees an unauthenticated device, and a failure is one
  `dev_err` and no device node, which is easy to monitor.

## 3. Where the keys live

- **One key per pair, not a fleet key.** Extracting the key from one MCU
  must not help against any other device. The Pi derives it:
  `K = HMAC(pi_device_key, "acq-pairing-v1" ‖ MCU_UID)`, where `MCU_UID`
  is the STM32's 96-bit unique ID.
- **Pi side**: on this board, `pi_device_key` is exactly what Raspberry
  Pi's firmware HMAC service provides (`rpi-fw-crypto hmac` with the OTP
  device key, lockable until reboot — see
  [integrity-and-encryption](integrity-and-encryption.md) §3), or the
  device identity TA on a platform with a TEE ([optee](optee.md)). The
  derived `K` never has to be stored on the Pi at all.
- **MCU side**: `K` is written once into a dedicated flash page during
  pairing — which is a provisioning step
  ([update-and-provisioning](update-and-provisioning.md) §5): read the
  MCU's UID over SWD, derive `K` on the Pi, flash it, set RDP level 1.
- **Re-pairing** after a legitimate board replacement is the same step,
  done by someone authorised — which is the point: replacing the board
  becomes a recorded event instead of an invisible one.

## 4. The honest limit: the STM32F103

The F103 has no secure element, no TrustZone and no hardware crypto. The
key sits in ordinary flash.

- **RDP level 1** stops a debugger from reading flash and is reversible
  (going back to level 0 mass-erases the chip, key included). It is the
  level used here.
- **RDP level 1 is not a security boundary on this family.** Published
  research (Schink & Obermaier, [*Exception(al) Failure — Breaking the
  STM32F1 Read-Out Protection*](https://blog.zapb.de/stm32f1-exceptional-failure/),
  2020, CVE-2020-8004) extracts flash from RDP-1-protected STM32F1 parts
  through the debug interface: debug reads of flash are blocked, but the
  CPU's exception entry still fetches vectors from flash over the ICode bus. Someone with
  the board, the paper and a debug probe can get the key.
- **RDP level 2** disables the debug port permanently and is irreversible;
  it is not used on the project's only MCU (see the red lines in the
  project plan).

So this design turns "swap the board" from *anyone with a clone* into
*someone who extracts the key from the genuine MCU first* — a higher bar,
not a guarantee. The real fixes are hardware: an external secure element
(e.g. an ATECC608-class device holding `K` and computing the MAC itself),
or an MCU with TrustZone-M and secure storage (Cortex-M33 class).

## 5. What it costs, and what to measure

The F103 computes SHA-256 in software. The numbers to take on the board,
with the project's usual method (repeated runs, the logic analyzer on a
GPIO marker around the computation):

- cycles for one HMAC-SHA256 over the ~48-byte message, at 72 MHz;
- whether the SPI ISR still re-arms in time while the main loop computes
  it (`REG_SPI_REARM_FAIL` stays 0);
- the added time to probe.

Authentication at probe is a one-off cost. Protecting every sample against
an interposer is a different budget: at 1000 samples/s the per-sample
cost would have to fit next to the existing ~1 ms end-to-end latency
([performance](../performance.md)) — which is why §1 lists it as out of
scope until the per-MAC cost is known, and why a MAC over *batches* of
samples is the likely answer if it is ever needed.

## 6. On the board

### 6.1 Correctness

SHA-256 and HMAC-SHA256 are written in portable C with no HAL, so the same
file was first checked on the development host against Python's `hashlib`
and `hmac` ([`tests/devauth_test.py`](../../v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/tests/devauth_test.py)):
the RFC 4231 test vectors, every message length from 0 to 129 bytes (all
padding boundaries), and the device-auth message format — **321 cases,
0 mismatches**.

On the device, the verifier makes the nonce and checks each answer with
the key; each answer is also checked against a random wrong key:

```
10/10 valid (MCU idle), 100/100 valid (during acquisition)
every answer rejected under a wrong key; answers distinct per nonce
before pairing (erased key page): auth_response -> ENOKEY ("Required key not available")
```

### 6.2 Cost, and its effect on acquisition

| | cycles | at 72 MHz |
| --- | --- | --- |
| MAC, MCU idle (10 rounds) | 35 062 – 35 116 | **487 µs** |
| MAC, during 1000 Hz acquisition (100 rounds) | 40 411 – 41 727 | **561 – 580 µs** |

Cycles come from the Cortex-M3's DWT counter around `devauth_compute()`;
72 MHz was confirmed from `RCC_CFGR` (`0x001d040a`: PLL from HSE, ×9, AHB ÷1).
The 15–19 % increase under load is the design working: the MAC runs in
the main loop, the SPI and timer interrupts preempt it, and the counter
includes their time. Authentication yields to acquisition, not the other
way round:

```
100 authentications, back to back, during acquisition:
  spi_rearm_fail 0 -> 0, spi_error_count 0 -> 0, kfifo_overflow 0 -> 0, policy_dropped 0 -> 0
  device-service: rate=1000.2/s gap_count=0
```

Firmware size: 7.3 KB → 9.3 KB of flash.

### 6.3 Read-out protection, level 1

([record](../../results/security/device-auth/rdp-level1-demo.txt))

- **Before**: the debugger read the key straight out of the last flash
  page (`0x0807f800: 61b413d4 a88f1b26 …`) — finding F10, demonstrated.
- **Level 1 set**: the same read fails (`Failed to read memory`). The
  device kept working and authenticating — **after a power-on reset of the
  MCU**: with the debugger connected when protection is set, flash stays
  inaccessible to the CPU too until POR (RM0008); a reset button isn't
  enough.
- **Attaching a debug probe to a protected chip stopped it again.** Under
  RDP 1 a probe can't read the key, but it can take the device down.
- **Removing level 1 mass-erased the chip**: key page and firmware both
  `ffffffff` — the "downgrade means erase" design, seen. Firmware and key
  were restored with `pair-mcu.sh` (same UID → same key).

The limit from §4 stands: this family's level 1 has a published bypass
(CVE-2020-8004) that reads flash without downgrading.

### 6.4 Differences from the design, and why

- **The verifier is not in the kernel driver yet.** The driver only moves
  bytes (`auth_challenge` / `auth_response`); the check runs in
  `mcu-auth-check.py`. Gating `/dev/acq0` at probe needs the key — or the
  firmware HMAC service that derives it — reachable from the kernel,
  which on this board means the OTP key that isn't programmed.
- **The pairing master is a file, not the Pi's OTP device key.**
  `pair-mcu.sh` derives `K = HMAC(master, "acq-pairing-v1" ‖ UID)` exactly
  as designed, but `master` is a random 0600 file on the signing host.
  Programming the Pi's OTP key is irreversible and is not done on the
  project's only board.

### 6.5 What went wrong while testing

Two failures during this work were not in the code under test, and the
time they cost is the reason to write them down:

- **A loose jumper on MOSI.** After the firmware was flashed, only
  register `0x00` could be read. Reading the MCU's receive buffer over SWD
  showed every frame arriving as zeros — "read `DEVICE_ID`" — so only the
  one register whose address is zero "worked". Re-seating the wire fixed
  it; reflashing the old firmware, resetting the MCU and rebooting the Pi
  had not.
- **The test harness itself.** Afterwards every sysfs write still failed —
  with the original driver too. An ftrace of the SPI transfers
  (`events/spi/spi_transfer_*`) showed the driver sending `1` where `1000`
  had been written: `echo 1000 | sudo tee <attr>` on this BusyBox system
  did not hand sysfs the whole value in one write. With
  `sudo sh -c "printf 1000 > <attr>"` everything worked. The link and the
  driver had been fine all along.

## 7. Moving the check into the driver — design, and why it isn't done

§6.4 left the verifier in a userspace tool. The obvious next step is
to challenge the MCU in `probe()` and not register `/dev/acq0` unless it
answers correctly. The mechanics are easy — the driver already has
`auth_challenge` / `auth_response`, and `get_random_bytes()` gives it a
nonce. The hard part is the one this whole design turns on: **where does
the kernel get the key from?**

| Where the key comes from | What it would take | What it's worth on this board |
| --- | --- | --- |
| Compiled into the module | nothing | **nothing** — the module is a file on the rootfs, the key is readable by anyone with the card, and it is the same for every device |
| Loaded by userspace into the kernel keyring (`keyctl add logon …`), read by the driver with `request_key()` | a key type, a boot-time loader | the same trust as today — the key still comes from a file that root (or anyone with the card) can read; it moves the check, not the secret |
| Derived in the kernel from the Pi's OTP device key through the firmware's HMAC service (`rpi-fw-crypto`) — what §3 designs | a mailbox call from the driver; **the OTP key programmed** | real: per-device, never in a file, lockable until reboot — but programming OTP is irreversible and not done on the only board |
| Held by a TEE; the driver asks a TA through the kernel TEE client API (`tee_client_open_session()`, as the OP-TEE RNG and fTPM drivers do) | a TA, a TEE on the platform | the right answer on SoCs with TrustZone firmware; the Pi 5 has no OP-TEE port ([optee](optee.md) §1) |

Two more properties decide it:

- **A probe-time check is a boot-time check.** The MCU can be swapped
  while the system runs; a check that only happens at `probe()` says
  nothing about the board attached an hour later. It would have to be
  repeated — at each `open()`, or periodically — and even then only a
  per-frame MAC (§5) covers the data *between* checks.
- **Failing closed in the kernel has a cost.** An MCU that fails
  authentication — wrong key after a legitimate repair, a firmware
  update that lost the key page — leaves the device with no `/dev/acq0`,
  which on this device means no product. That is a policy decision (and
  a service process for re-pairing), not a driver detail.

**Decision: not implemented.** With the key sources this board can offer
without an irreversible step, a check in the driver would look stronger
than the userspace one while having the same root of trust — a file.
The honest version of the kernel gate is row 3, and it is one
`program_pubkey`-class decision away.

What *was* worth doing, and was cheap: make the existing check a gate
rather than a tool. Since 1.2.0,
[`device-platform-mcu-auth`](../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/device-platform-mcu-auth)
runs as `ExecStartPre=+` of `device-service` — BusyBox and the `openssl`
CLI, no Python: a 16-byte nonce from the kernel RNG, the challenge through
the driver's sysfs attributes, and the MAC recomputed with
`openssl dgst -mac HMAC`. The pairing key lives on the encrypted data
partition (`/data/devauth/mcu.key`, root 0600) — still a file, as the table
above says, but no longer on the signing host
([record](../../results/security/update/on-target-signed-ab.txt)):

```
right key : mcu-auth: OK - MCU 0xac00acc0 fw 0x00010400 answered the challenge (38729 cycles)
random key: mcu-auth: FAIL - wrong answer from MCU 0xac00acc0 (not the paired device, or not paired)
no key    : mcu-auth: FAIL - no pairing key at …
```

A failure stops `device-service` from starting; the health check then
never commits the slot, so an update that breaks pairing rolls back by
itself. It runs at every service start, not only at boot — an MCU swapped
while the system runs is caught at the next restart of the service, and
not before (per-frame MACs, §5, remain the answer to that).
