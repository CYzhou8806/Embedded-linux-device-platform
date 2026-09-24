# Integrity and encryption of storage

Closes findings **F5** (rootfs can be modified), **F7** (WiFi PSK in
plaintext) and **F8** (no encryption at rest) of the
[threat model](threat-model.md). Status per part:

| Part | Status |
| --- | --- |
| dm-verity on the real rootfs image, tamper demo (host, no root needed) | ✅ done — §1 |
| Where the disk-encryption key can live on a Raspberry Pi 5 | ✅ analysed from Raspberry Pi's current sources — §3 |
| dm-verity image for the board: [`device-platform-image-verity`](../../yocto/meta-device-platform-verity/) (initramfs opens the verity root) | ✅ built and checked byte for byte |
| Booting it on the Pi 5; a byte changed on the card → `EIO` for exactly the predicted block | ✅ [case 10](../debugging/case-10-dm-verity-one-byte-on-the-card.md) |
| LUKS data partition on target (created on first boot, holds the SSH host key and RAUC state) | ✅ §5 |
| Cost of dm-verity and LUKS, with and without the Armv8 crypto extensions | ✅ §5 |

## 1. dm-verity, shown on the image this project builds

dm-verity is a device-mapper target that checks every block read from a
read-only filesystem against a Merkle tree of SHA-256 hashes. Only the
**root hash** has to be trusted; everything else — the data and the tree
itself — can sit on the untrusted SD card.

[`security/integrity/verity-tamper-demo.sh`](../../security/integrity/verity-tamper-demo.sh)
runs it on `device-platform-image`'s real ext4 rootfs, with the same
`veritysetup` userspace code the kernel target mirrors, and changes one
byte inside the `device-service` binary.
Full output: [`results/security/dm-verity/host-tamper-demo.txt`](../../results/security/dm-verity/host-tamper-demo.txt).

```
hash tree: 1495040 bytes for 188743680 bytes of data           (0.79 %)
verify (untouched):                   OK
one byte changed in /opt/device-service/device-service
  filesystem block 31968 × 1024 + 100 = offset 32735332 → verity block 7992
  plain ext4 reads the modified binary:  sha256 f4c988ee… → 62eab4ec…
verify (tampered, original root hash): Verification failed at position 32735232
                                       (= 7992 × 4096, the 4 KiB block that was changed)
attacker rebuilds the tree:            root hash 56ea5b58…  ≠  original 73f2d9e5…
```

What this shows:

- **The filesystem itself doesn't notice.** ext4 happily serves a
  modified binary; nothing in a normal read path compares file contents
  to anything.
- **Verification is per block, at read time.** On a device the kernel
  returns `EIO` for exactly that 4 KiB block (or reboots/panics,
  depending on the configured error mode), the first time it is read —
  not at mount time. A 188 MB image doesn't have to be hashed before boot.
- **The tree is cheap.** 1.46 MB for 188 MB of data with 4 KiB blocks.
- **Recomputing the tree is trivial; the root hash is the whole point.**
  An attacker who modifies the image can build a valid tree for it, with
  a different root hash. The protection is only as good as the place the
  root hash comes from — on the kernel command line inside the signed
  `boot.img` ([secure-boot](secure-boot.md) §1). dm-verity without secure
  boot detects corruption, not attackers.

## 2. dm-verity vs dm-crypt vs fs-verity

| | Protects | Granularity | Writable? | Key needed on the device |
| --- | --- | --- | --- | --- |
| **dm-verity** | Integrity of a read-only block device | Block | No | None — only a trusted root hash |
| **dm-crypt / LUKS** | Confidentiality of a block device | Block | Yes | A secret key (the hard part, §3) |
| **fs-verity** | Integrity of individual read-only files on a writable filesystem | File | File becomes immutable | None — per-file digest, optionally signed |

For this device: dm-verity for the root filesystem (it's read-only in
the production image anyway — [hardening](hardening.md)), LUKS for a
separate data partition that holds recorded samples, logs and network
credentials, and nothing secret in the rootfs itself. `dm-crypt` alone
does not give integrity: a modified ciphertext block decrypts to garbage
that the filesystem will read without complaint (unless authenticated
encryption with `dm-integrity` is added).

## 3. Where the encryption key can live on a Raspberry Pi 5

The Pi 5 has no TPM and no secure element. The options, from worst to
best, with what each one is actually worth against the threat model's
attackers:

| Where the key is | Stops A3 (pulled SD card) | Stops root on a running device | Stops kernel compromise |
| --- | --- | --- | --- |
| Key file in the rootfs | No | No | No |
| Derived from the SoC serial / MAC | No — both are readable from any running system or printed on the board | No | No |
| Typed in by a person at boot | Yes | No | No — unattended device, so not an option here |
| **OTP device key + firmware HMAC, locked after use** (below) | **Yes, with secure boot** | **Yes, until reboot** | **No** |
| TPM sealed to PCRs / secure element / TEE-held key | Yes | Yes | Mostly — depends on the TEE |

Raspberry Pi's current firmware (2025–2026 releases, `rpi-fw-crypto` in
[`raspberrypi/utils`](https://github.com/raspberrypi/utils/tree/master/rpifwcrypto))
provides the fourth row:

- A 256-bit **device-unique private key in OTP** (an ECDSA P-256 scalar),
  which the firmware can generate itself (`genkey`), so the key never
  exists outside the chip. *Writing OTP is permanent; it is not done on
  this project's only board.*
- **`hmac` and `sign`** operations that use the key without returning it.
  Raspberry Pi's own suggestion for disk encryption is exactly this:
  `HMAC(device key, serial + eMMC CID)` as the LUKS passphrase.
- **Per-key locks until the next reboot** (`set-key-status … HMAC_LOCKED
  READ_LOCKED`), and `lock_device_private_key=1` in `config.txt`.

The resulting boot flow:

```
signed boot.img ─► kernel + initramfs (verified by the firmware)
                     initramfs: passphrase = rpi-fw-crypto hmac(key 1, serial ‖ CID)
                                cryptsetup open /dev/mmcblk0p3 data
                                rpi-fw-crypto set-key-status 1 HMAC_LOCKED|READ_LOCKED
                                switch_root  ── from here on, even root can't derive the passphrase again
```

Its limits, which have to be stated rather than hidden:

- `lock_device_private_key=1` is a line in `config.txt`. **Without secure
  boot it is worthless**: an attacker with the SD card deletes the line.
  With secure boot, `config.txt` is inside the signed `boot.img`.
- Raspberry Pi's documentation says it directly: *"It is not possible to
  prevent code running in ARM supervisor mode (e.g. kernel code) from
  accessing OTP hardware directly."* A kernel exploit gets the key. That
  is the difference from a TrustZone TEE, which hides its keys from the
  normal-world kernel too ([optee](optee.md) §5).
- After `cryptsetup open`, the volume key is in kernel memory for as long
  as the volume is open — the case for every dm-crypt setup, TPM or not.

## 4. Not done yet (needs the board)

- ~~Boot the verity image and tamper with the card~~ — done, see
  [case 10](../debugging/case-10-dm-verity-one-byte-on-the-card.md). What it
  planned:
  [`device-platform-image-verity`](../../yocto/meta-device-platform-verity/)
  (already built: meta-security's `dm-verity-img` class, its initramfs
  bundled into a kernel with dm-verity built in, and the root hash in the
  initramfs — every piece checked against the others on the build host);
  modify a block on the card; show the kernel's `EIO` and the boot
  outcome. This is the on-target half of §1 and will become a
  `docs/debugging/case-10-*.md` write-up. Two traps already found while
  building it are in that layer's README (a partition label written into
  the verified filesystem; a conditional `SRC_URI:append` still rebuilding
  the main build's kernel).
- The root hash currently travels inside the initramfs, which is inside
  the kernel image on the unverified boot partition. It becomes
  trustworthy only once that kernel is part of a signed `boot.img`
  ([secure-boot](secure-boot.md)) — until then this image detects
  corruption, not an attacker.
- A LUKS data partition, and its cost: `cryptsetup benchmark` on the Pi 5
  (the Cortex-A76 has the Armv8 crypto extensions, so AES-XTS should be
  far from the bottleneck) plus read/write throughput with and without
  encryption, measured the way the rest of this project measures —
  repeated runs, not a single number.

## 5. On the board: the data partition, and what integrity and encryption cost

[`device-platform-image-ab`](../../yocto/meta-device-platform-verity/recipes-core/images/device-platform-image-ab.bb)
has a LUKS2 data partition (aes-xts-plain64, 512-bit key) that
[`device-platform-data`](../../yocto/meta-device-platform-verity/recipes-support/device-platform-ab/files/device-platform-data)
creates on first boot and opens on every later one; the SSH host key and
RAUC's slot state live there, which is what made them survive the
read-only root ([hardening](hardening.md) §5). **Its key is the insecure
stand-in described in the script** — derived from the board's serial
number — because the OTP device key the design uses (§3) is not
programmed on this board. What is real here: the boot ordering, the
persistence, and the cost.

Sequential reads of 150 MiB, page cache dropped before each run, three runs
each (they agreed within ±1.5 %)
([raw](../../results/security/dm-verity/read-throughput-ab-image.txt),
[baseline](../../results/security/dm-verity/read-throughput-sha256-generic.txt)):

| | raw partition | through the mapping | cost |
| --- | --- | --- | --- |
| dm-verity, `sha256-generic` (crypto extensions as modules) | 85.4 MiB/s | 62.2 MiB/s | −27 % |
| dm-verity, `sha256-ce` (built in) | 82.9 MiB/s | 69.7 MiB/s | **−16 %** |
| LUKS2, `xts-aes-ce` | 83.2 MiB/s | 77.2 MiB/s | **−7 %** |

- **Why "built in" matters for dm-verity only.** The verity target is set
  up in the initramfs, before any module can load, and binds the SHA-256
  implementation that exists at that moment — `sha256-generic` in the
  earlier image, although `sha256-ce` was loaded a few seconds later.
  Building `CONFIG_CRYPTO_SHA2_ARM64_CE` into the kernel raised verity
  reads by 12 % and cut its overhead from 27 % to 16 %. LUKS is opened
  later, after modules load, and gets `xts-aes-ce` either way.
- These are reads from an SD card that is itself the bottleneck (~83 MiB/s);
  the numbers say what integrity and encryption cost *on this storage*,
  not what the CPU could do.

## 6. Authenticated encryption for the data partition

§2 says it plainly: dm-crypt alone gives confidentiality, not integrity —
a changed ciphertext sector decrypts to garbage that the filesystem reads
without complaint. Since `device-platform-image-ab` 1.1.0 the data
partition is formatted with `--integrity hmac-sha256`: every 512-byte
sector gets a 32-byte HMAC stored by dm-integrity underneath dm-crypt
(`data_dif` below `data`), and the kernel combines both as
`authenc(hmac(sha256-ce),xts-aes-ce)`. The XTS key and the HMAC key both
come from the LUKS volume key (768 bits = 512 + 256).

**Tamper test** ([record](../../results/security/dm-verity/luks2-integrity-tamper.txt)):
one byte changed on the raw partition, underneath both layers, in free
space near its end; the original byte read first and restored afterwards.

```
read the end of /dev/mapper/data                    -> OK
one byte on /dev/mmcblk0p7 changed (fe -> ff)
read again                                          -> dd: Input/output error
  device-mapper: crypt: dm-1: INTEGRITY AEAD ERROR, sector 1687560
  audit: module=crypt op=integrity-aead dev=254:1 sector=1687560 res=0
  Buffer I/O error on dev dm-2, logical block 210945    (1687560 / 8 = 210945)
byte restored                                       -> OK again
```

Like dm-verity, it has no memory: restoring the byte makes the sector
valid again. Unlike dm-verity, the tag is a keyed MAC, not a public hash
tree — an attacker without the key can't produce a valid sector, but
**can replay an old one**: dm-integrity authenticates each sector with its
own number, not with a version, so an older (sector, tag) pair written
back verifies. Rollback protection for data needs a counter, the same
lesson as [optee](optee.md) §3.2.

**Cost** ([record](../../results/security/dm-verity/luks2-integrity-throughput.txt),
kernel 6.12, three runs each):

| | read (150 MiB, cache dropped) | write (100 MiB + sync) |
| --- | --- | --- |
| raw SD card partition | 85.4 MiB/s | 30–43 MiB/s |
| plain dm-crypt (XTS) | 77.2 MiB/s (−7 %, §5) | 36–42 MiB/s (≈ raw) |
| **LUKS2 + integrity** | **64.9 MiB/s (−24 %)** | **16–21 MiB/s (≈ −50 %)** |

- Reads pay for the HMAC and for reading tags interleaved with data.
- Writes pay double: dm-integrity's default journal writes every sector
  twice so that data and tag can't get out of step on power loss. The
  alternatives (`--integrity-no-journal`, or a bitmap mode) trade that
  crash consistency for speed. For a device that mostly appends
  measurements and loses power without warning, the journal is the right
  default; the 50 % is the price.
- The write rows are not the same path (raw/dm-crypt are block devices,
  integrity is measured through ext4 on `/data`), so they give the size of
  the cost, not a precise percentage. The baselines were measured on the
  inactive slot B root partition, which the next update overwrote anyway.
- First boot takes longer: formatting with integrity writes the whole
  partition once to initialise the tags (1 GB here; about a minute).

## 7. Choosing what "corrupted" does: restart, and fall back

Case 10 ran with dm-verity's default error mode: a corrupted block is an
`EIO`, and the system keeps running on a root it can't fully read. From
1.1.0 on, the initramfs opens the root with `--restart-on-corruption`
([`dmverity-errmode`](../../yocto/meta-device-platform-verity/recipes-core/initrdscripts/files/dmverity-errmode);
the mode comes from `dmverity.error=` on the kernel command line — inside
the signed ramdisk — and defaults to `restart`):

```
0 286720 verity 1 179:5 179:5 1024 4096 143360 35841 sha256 <root hash> <salt> 1 restart_on_corruption
```

On its own that is worse than `EIO` for a corrupted *committed* slot: the
device reboots into the same corruption forever. It becomes the right
choice together with A/B, for the case it's meant for — **an update that
installed corrupted or tampered content**. Tested on the board
([record](../../results/security/update/on-target-signed-ab.txt)):

```
install 1.1.0 into slot A (not yet confirmed)
predicted on the host: systemd's first block = fs block 61756, byte +64 = 0x06
on the card: p5 + 61756*1024 = ".ELF", +64 = 06                -> matches
change that byte to 0xff, reboot "0 tryboot" into A
-> 14 s later the device is booting slot B again: partition=3, tryboot=0,
   bootloader boot count 4 (A, B tryboot, A tryboot, B), A never confirmed
byte restored; sha256 of p5 == the bundle's rootfs image
```

The kernel restarted as soon as PID 1's first block was read; the
one-shot tryboot was then used up, and the firmware booted the committed
slot. No health check, watchdog or network was involved. What this first
run could not capture is the kernel's own `data block … is corrupted`
line: the failed attempt's log lived in RAM for a few seconds. That is
what the follow-up below adds.

Two more notes:

- **How the error mode almost didn't ship**: the replacement script was first installed
  with a `do_install:append`, and the built initramfs still contained the
  upstream script — meta-security's bbappend (layer priority 8) ran after
  this layer's (7) and installed its file over ours. The build succeeded;
  only unpacking the initramfs showed it. It's a `do_install[postfuncs]`
  now.
- A corrupted **committed** slot still reboot-loops. Breaking that needs
  a boot counter the firmware honours (the Pi 5 bootloader has none for
  `autoboot.txt` partitions today) or `panic_on_corruption` plus a
  recovery partition. Stated as a limit, not solved.

**Follow-up: the evidence, with pstore.** Since 1.2.1 the image reserves
256 KiB of RAM for ramoops, and the kernel log of the previous boot is in
`/sys/fs/pstore` after any reboot. That needed its own overlay: the kernel
tree's `ramoops` overlay writes `reg` with one address cell, BCM2712's
`reserved-memory` has two, and `ramoops-pi4` (right layout) declares the
Pi 4's SoC. With the generic one, `dtoverlay=ramoops` was accepted and
silently produced nothing — no node, no pstore — which only reading
`/proc/device-tree` back showed
([`ramoops-pi5-overlay.dts`](../../yocto/meta-device-platform-verity/recipes-bsp/ramoops-pi5-overlay/files/ramoops-pi5-overlay.dts)).
The same test again, with 1.2.1 in both slots
([full log](../../results/security/dm-verity/pstore-verity-restart-console.txt)):

```
predicted on the host: systemd's first block = 61755; card matches; byte changed; tryboot into A
back on B 38 s later; /sys/fs/pstore/console-ramoops-0 holds the failed attempt:
  [3.148742] Run /init as init process
  [4.440277] device-mapper: verity: sha256 using shash "sha256-ce"
  [4.905102] device-mapper: verity: 179:5: data block 61755 is corrupted
  [4.939623] reboot: Restarting system with command 'dm-verity device corrupted'
```

The block the kernel rejected is the one computed on the build host, and
the reboot followed 34 ms later. The boot recorder copies the pstore
contents to partition 1, so the reason for an unexpected reboot survives
even when nobody logs in afterwards. (A first attempt with only one slot
updated showed the other boot's log instead: the attempt ran the older
image, which didn't have working ramoops — the evidence has to be in the
image that fails, not the one that recovers.)
