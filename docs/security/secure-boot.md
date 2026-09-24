# Secure boot on the Raspberry Pi 5

Closes finding **F4** of the [threat model](threat-model.md) — as far as it
can be closed without an irreversible step on the only board in the
project. Everything up to that step was done and verified; the step itself
is replaced by the checklist in §5.

Sources: Raspberry Pi's [`usbboot`](https://github.com/raspberrypi/usbboot)
(`docs/secure-boot.md`, `secure-boot-recovery5/`, `secure-boot-example/`)
and [`rpi-eeprom`](https://github.com/raspberrypi/rpi-eeprom), checked out
on 2026-09-23. Details of this chain change between SoC generations and
firmware releases, so these were read instead of relying on older write-ups.

## 1. The chain on BCM2712

```
 BCM2712 BootROM (mask ROM, holds Raspberry Pi's public keys)   ← root of trust
   │  verifies bootsys: signed by Raspberry Pi AND counter-signed by the customer
   │  customer public key comes from the EEPROM, accepted only if
   │  SHA-256(key) == customer key hash in OTP
   ▼
 bootsys (2nd stage, SPI EEPROM)
   │  checks every firmware dependency against a hash list built into it
   │  verifies bootconf.txt against bootconf.sig (customer key)
   ▼
 bootmain
   │  loads boot.img + boot.sig from SD / USB / NVMe / network
   │  SHA-256(boot.img) must match, RSA-2048 PKCS#1 v1.5 signature must verify
   │  with the customer key — otherwise this boot mode is abandoned
   ▼
 boot.img  = a FAT ramdisk: config.txt, cmdline.txt, DTBs + overlays,
             GPU firmware, kernel, initramfs. In secure mode the firmware
             loads NOTHING from outside it.
   ▼
 Linux kernel + initramfs          ── end of what the firmware verifies ──
   ▼
 root filesystem                   ← needs dm-verity, with its root hash on the
                                     kernel command line inside boot.img
```

Three things about this chain are easy to get wrong:

- **The OTP holds a hash, not the key.** OTP is small and one-time; the
  2048-bit key lives in the EEPROM, and OTP only pins which key is
  acceptable. Replacing the EEPROM key without the matching private key
  therefore gets an attacker nothing.
- **The customer signs the Raspberry Pi firmware too (BCM2712 only).** On
  the Pi 4, the ROM only checks Raspberry Pi's signature on `bootsys`. On
  the Pi 5 it also requires the customer's counter-signature, so no
  bootloader update — not even a genuine one from Raspberry Pi — can be
  installed without the product owner signing it.
- **Everything the kernel uses has to be inside `boot.img`.** A signed
  kernel with an unsigned `cmdline.txt` next to it would let an attacker
  add `init=/bin/sh`. Packing the whole boot partition into one signed
  ramdisk is how this platform avoids signing files one by one.

Where the chain stops matters as much: **the firmware verifies up to the
kernel and initramfs, nothing after.** Extending it to the root filesystem
is dm-verity's job ([integrity-and-encryption](integrity-and-encryption.md)),
and it only holds if the verity root hash is inside the signed `boot.img`.

## 2. Pi 5 differs from Pi 4 in a way that changes the plan

On the Pi 4 (BCM2711) a signed EEPROM could be flashed and tested
**before** programming OTP. On the Pi 5 that is not possible: a signed
EEPROM image does not run at all until the key hash is in OTP (the board
simply doesn't boot; older C1-stepping boards blink an error code). So
the Pi 5 has no "enforcing but reversible" test mode for the bootloader.

What *can* be tested reversibly on a Pi 5 is that a `boot.img` is
well-formed and boots: put `boot.img` and `boot.sig` on the boot partition
and `boot_ramdisk=1` in its `config.txt`. The bootloader then boots from
the ramdisk — but it only enforces the signature in secure mode, so this
proves the image, not the signature check.

## 3. What was done (all reversible, nothing flashed)

### 3.1 Signing key in a (soft) HSM

The signing key is an RSA-2048 key generated **inside** a SoftHSM2 token
([`security/signing/hsm-init.sh`](../../security/signing/hsm-init.sh)).
It has never existed as a file:

```
Private Key Object; RSA
  label:      rpi5-boot-rsa2048
  Access:     sensitive, always sensitive, never extractable, local
```

`never extractable` and `local` are attributes the token enforces: no
PKCS#11 call can return the key, and it was generated on the token rather
than imported. Only the public key is exported, as PEM.

Raspberry Pi's tools support this directly through an **HSM wrapper**: a
program called with `-a rsa2048-sha256 FILE` that prints a hex PKCS#1 v1.5
signature. [`security/signing/pkcs11-hsm-wrapper`](../../security/signing/pkcs11-hsm-wrapper)
implements it with `pkcs11-tool` (`CKM_SHA256_RSA_PKCS`, computed by the
token), reads the PIN from a `0600` file instead of the command line, and
appends every signature to an audit log. Pointing `PKCS11_MODULE` at a
hardware HSM's library is the only change a real one would need.

### 3.2 Signing and verifying a boot image

```
$ rpi-eeprom-digest -H pkcs11-hsm-wrapper -i boot.img -o boot.sig
$ cat boot.sig
902be6c4…1959                      ← SHA-256 of boot.img
ts: 1790197620
rsa2048: 0c95a983…                 ← signature made by the token

$ rpi-eeprom-digest -k public.pem -i boot.img -v boot.sig
Verified OK

# one byte changed at offset 100000:
$ rpi-eeprom-digest -k public.pem -i boot-tampered.img -v boot.sig
rsa routines:ossl_rsa_verify:bad signature
Verification failure
```

### 3.3 Counter-signed EEPROM image for secure mode

`update-pieeprom.sh -f -H pkcs11-hsm-wrapper -p public.pem` produced the
EEPROM image a production line would flash: Raspberry Pi's `bootsys`
counter-signed by the token, `bootconf.txt` signed, and the public key
embedded. It is **not flashed** (§2: without OTP the board would not boot).

The image was then taken apart again to check it against the key:

```
SHA-256(public key in the token, bootloader format)  fe0144747838fcf8…4475d7b5
SHA-256(pubkey.bin extracted from pieeprom.bin)       fe0144747838fcf8…4475d7b5
```

These are equal, and that equality is the most important check before
programming OTP (§5): the hash `program_pubkey=1` burns is computed from
the key in the EEPROM image being flashed, so an image built with the
wrong key produces a board that accepts only that wrong key, for ever.

### 3.4 On the board: a signed `boot.img` booting the device

The one part of the chain that *can* be tested on a Pi 5 without OTP
(§2): does the signed ramdisk boot the real system? Done through the
firmware's one-shot **tryboot**, so that any failure is undone by the
next power cycle. The ramdisk
([`make-signed-boot-img.sh`](../../security/signing/make-signed-boot-img.sh),
built as a normal user with `mkfs.vfat -C` + mtools, signed through the
HSM wrapper) holds the kernel, all device trees and overlays, `config.txt`
and a `cmdline.txt` carrying a marker, `bootimg=signed-test`.

It took three attempts, and the first diagnosis was wrong:

1. **No network after the reboot.** Without a console the cause was a
   guess: the board is a D0-stepping Rev 1.1 (`d04171`) and the first
   ramdisk lacked `overlays/bcm2712d0.dtbo`. Adding it changed nothing —
   the guess was wrong.
2. **An HDMI screen** showed the bootloader's diagnostics
   ([photo](../../results/security/secure-boot/tryboot-error6-screen.jpeg)):
   ```
   Read tryboot.txt bytes 2548
   Loading tryboot.img ...
   Error 6 loading tryboot.img
   ```
   **In tryboot mode the bootloader loads `tryboot.img`, not `boot.img`**
   — consistent with tryboot never touching the normal boot files. It
   *is* documented, on the `autoboot.txt` page rather than the secure-boot
   pages this work had read: `tryboot_a_b=1` exists precisely to "load the
   normal `config.txt` and `boot.img` files instead of `tryboot.txt` and
   `tryboot.img`". The firmware never got as far as the ramdisk's
   contents in either failed attempt.
3. Renamed to `tryboot.img`/`tryboot.sig`: **the device booted from the
   signed ramdisk** — `bootimg=signed-test` in `/proc/cmdline`, the
   bootloader's `tryboot` flag set, root mounted from `/dev/mmcblk0p2` as
   the ramdisk's command line says. But `device-service` hung in
   `activating`: `spi0.0` did not exist, and the firmware had added its
   `bcm2708_fb` parameters — **no dtoverlay had been applied at all**. The
   explanation that fit: in tryboot mode the second configuration pass
   reads `tryboot.txt` *from the ramdisk*, which contained only
   `config.txt`. With a copy of `config.txt` as `tryboot.txt` inside the
   ramdisk: `spi0.0` present, `device-service` active, the MCU answering
   (`0xac00acc0`). A normal reboot afterwards returned to the dm-verity
   system, and the tryboot files were removed.

Both findings are now built into the script (`--tryboot`). The second
one is the more dangerous kind: the system boots and looks healthy, and
a peripheral is simply gone.

What this proves, and what not: the signed image is well-formed and boots
this device completely. Without the key hash in OTP the firmware does
**not** check the signature, so this is not evidence that an unsigned or
modified image would be refused — that part can only be shown on a board
that has been locked.

## 4. The step not taken: `program_pubkey=1`

Setting `program_pubkey=1` in the `rpiboot` `config.txt` and flashing
writes the key hash into OTP. From then on:

- only `bootsys` counter-signed with this key runs, and only `boot.img`
  images signed with it boot;
- the EEPROM config must be signed; downgrading to a bootloader without
  secure-boot support is impossible;
- the ROM no longer loads `recovery.bin` from SD/eMMC, so bootloader
  recovery is only possible through `rpiboot` with a counter-signed
  `recovery.bin`.

It **cannot be undone and a different key cannot be programmed later.**
On a project with one Raspberry Pi 5 that is also used for kernel and
latency work (self-built kernels booted through `tryboot`, which would
all need signing), programming it would end the rest of the project on
this board. `program_jtag_lock` (permanently disabling VideoCore JTAG) is
likewise not used; it only takes effect once the key hash is programmed.

## 5. Checklist before programming OTP

What a production line (or the first engineering board) should prove
before `program_pubkey=1` is ever set. Items marked ✅ were done here.

**Key**
- ✅ The key is RSA-2048 (the only size BCM2712 supports) and was generated
  on the HSM, non-extractable.
- ⬜ A backup exists that is recoverable without the person who made it
  (HSM backup/cloning, or key shares) — losing the key means no more
  updates for every locked device. SoftHSM stands in here; this is the
  item a real HSM exists for.
- ⬜ Separate development and production keys. Development boards are
  never locked, or locked to the development key only.

**Images**
- ✅ `boot.img` signature verifies with the public key, and a modified
  image fails.
- ✅ The public key embedded in the EEPROM image hashes to the value that
  will be burned.
- ✅ The signed `boot.img` boots on the target with `boot_ramdisk=1`
  (§3.4; through tryboot — which needs the files named `tryboot.img` and
  a `tryboot.txt` inside the ramdisk).
- ⬜ The bootloader's UART log shows the expected `Customer key hash`
  (with `BOOT_UART=1`, as set in `boot.conf`).
- ⬜ A counter-signed `recovery.bin` exists and has been tested, because
  after locking it is the only recovery path.

**Process**
- ⬜ First lock one board, run the full field-update path on it (sign a new
  image, deploy it, roll back to a previous signed image), and only then
  lock more.
- ⬜ Record per device, from `rpiboot`'s metadata output
  (`CUSTOMER_KEY_HASH`, `SECURE_BOOT_PROVISION`, serial, MAC): which key it
  was locked to, when, by which station.
- ⬜ `ENABLE_SELF_UPDATE=0` stays set, so the bootloader can't be updated
  other than through the signed path.

## 6. The same problem on other platforms

The Pi 5's "one signed ramdisk" design is unusual. The two designs a BSP
engineer meets most often:

**Arm Trusted Firmware-A (most Armv8-A SoCs).** BL1 in ROM → BL2 (trusted
boot firmware) → BL31 (EL3 runtime/secure monitor), BL32 (the secure-world
OS, e.g. OP-TEE) and BL33 (the normal-world bootloader, usually U-Boot) →
Linux. Each image is authenticated through X.509 certificates in a FIP
(Firmware Image Package) according to the TBBR chain of trust; the root
of trust is the hash of the ROT public key in fuses. Anti-rollback uses
non-volatile counters, also in fuses, compared against a counter in each
certificate.

**U-Boot verified boot (FIT images).** The kernel, DTB and initramfs are
packed into a FIT image; U-Boot holds the public key in its own control
DTB and verifies before booting. The important detail is to sign
**configurations**, not only images: signing each image separately
still allows mixing a validly signed kernel with a different validly
signed DTB. U-Boot itself then has to be verified by the stage before it
(TF-A or the SoC ROM), or the chain has a hole at its start.

**Zynq-7000.** The BootROM authenticates the FSBL with RSA-2048 (the hash
of the primary public key is in eFUSE) and can decrypt it with AES-256.
The AES key lives in either **eFUSE** (permanent, survives anything) or
**battery-backed RAM** (can be erased or replaced, and is lost if the
battery dies — which is also the point: an anti-tamper circuit can wipe
it). Choosing between them is choosing between "can never be changed" and
"can be destroyed on purpose".

Compared with those, the Pi 5 trades flexibility for simplicity: one
customer key, one signed file, no multi-stage certificate chain, and no
generic anti-rollback counter for `boot.img` described in the sources
above — that has to be handled by the update system
([update-and-provisioning](update-and-provisioning.md)).

### 3.5 Signed `boot.img` in both A/B slots

§3.4 booted one signed ramdisk once. `device-platform-image-ab` 1.1.0 and
later carry one in **each boot slot**, and the RAUC bundle's `boot` image
is the same signed partition
([`make-signed-ab-release.sh`](../../security/signing/make-signed-ab-release.sh),
[record](../../results/security/update/on-target-signed-ab.txt)):

```
p2 / p3   boot.img  (FAT ramdisk, 62.8 MB: kernel with the dm-verity initramfs
                     and root hash, DTBs, overlays, config.txt + tryboot.txt,
                     cmdline-a.txt / cmdline-b.txt)
          boot.sig  (RSA-2048, HSM key 01)
          config.txt: boot_ramdisk=1
```

The build never touches the boot key: bitbake produces the image, the
signing host takes partition 2's contents, packs and signs the ramdisk
through the HSM, writes it into p2 and p3 of a copy of the image, and
builds the bundle from it (development-signed, then release-signed).
Checked before anything was flashed: the rootfs partition is
byte-identical to the verity image, the root hash verifies it, **the same
root hash appears in the signed kernel** (the initramfs is embedded
uncompressed), and the signature verifies with the public key.

On the board:

- **Normal boot from p2**: p2 holds nothing but `boot.img`, `boot.sig`,
  `config.txt`; the kernel is 6.12.93 with `root=/dev/mmcblk0p5 rauc.slot=A`
  from inside the ramdisk.
- **Update 1.1.1 → B, tryboot**: `partition=3 tryboot=1`,
  `root=/dev/mmcblk0p6 rauc.slot=B`. One `config.txt` inside the ramdisk
  serves both slots with `[boot_partition=2]` / `[boot_partition=3]` — the
  filter applies to the second configuration pass inside the ramdisk too,
  which no document said. (If it hadn't, the ramdisk's default
  `cmdline.txt` points at slot A, and a B boot would have failed verity and
  fallen back — that default was chosen for that case.) Health check →
  committed, `[all] boot_partition=3`.

That closes the gap in the threat model's F4 row as far as this board
allows: the chain firmware → signed ramdisk → kernel + root hash →
verified root now exists in every slot and through every update. The
first link — the firmware *enforcing* the signature — is still the OTP
step (§4).
