# Case 10: dm-verity on the Raspberry Pi 5 — One Byte on the Card, Predicted to the Block on the Host

**Platform:** Raspberry Pi 5, `device-platform-image-verity` (the production image with a dm-verity root filesystem, [`yocto/meta-device-platform-verity/`](../../yocto/meta-device-platform-verity/)), Yocto Scarthgap, kernel `6.6.63-v8-16k` with `CONFIG_DM_VERITY=y`
**Occurred:** security work, integrity of the root filesystem ([docs/security/integrity-and-encryption.md](../security/integrity-and-encryption.md), finding F5 of the [threat model](../security/threat-model.md))

This case is a verification rather than a bug hunt: the question was
whether a protection that looked right on paper — and on the build host —
actually does what it claims on the board. It includes the one defect that
would have made the image fail on its first boot, which only a byte-level
comparison caught.

## What was expected

dm-verity checks every block read from the root filesystem against a
Merkle tree whose root hash was computed at build time. So a single
changed byte on the SD card should make exactly one data block
unreadable, the first time it is read, and nothing else.

On the build host, the same check had already been run on the image with
`veritysetup verify` ([host demo](../../results/security/dm-verity/host-tamper-demo.txt)).
On the board, the root is opened by meta-security's initramfs, which is
bundled into the kernel and carries the root hash.

## Before the first boot: the image that would not have verified

The image built cleanly. Four checks on the build output, before writing
any card:

1. the rootfs partition of the `.wic` is byte-identical to the
   `.ext4.verity` file the hash tree was computed over;
2. the boot partition's `kernel_2712.img` is the kernel with the initramfs
   bundled into it;
3. the kernel has `BLK_DEV_DM`, `DM_VERITY`, `DM_CRYPT` built in;
4. the root hash inside the initramfs verifies the rootfs partition.

Check 1 failed after a cosmetic change — adding `--label root` to the
`.wks` line so the partition would have a name:

```
cmp: differ: byte 1073
```

Byte 1073 is inside the ext4 superblock. `wic` writes the label into the
filesystem it copies with `rawcopy`, *after* the hash tree was built from
it. The build reported success, the image looked normal, and it would
have failed verification on its first boot. meta-security's own `.wks`
files carry a comment saying exactly this ("We must not alter the label");
the fix was to set only `--fstype` (which changes the partition type, not
the filesystem) and to keep check 1 as part of the procedure.

The other surprise of the build was in a different place: putting the
kernel config fragment into the main layer behind a variable rebuilt the
*main* build's kernel too, although the variable was off there — BitBake
hashes the unexpanded text of `SRC_URI`. The fragment moved to a separate
layer that only the dm-verity build directory uses.

## On the board

The image booted. `/` is mounted from `/dev/mapper/rootfs`, not from the
partition, and the kernel log shows the verity target setting up:

```
/dev/mapper/rootfs on / type ext4 (ro,relatime)
device-mapper: verity: sha256 using implementation "sha256-generic"
```

The test file had to be one the running system never reads, so a failure
could not take anything else down: the btrfs module (the device has no
btrfs). On the host, from the same image, its tenth block is filesystem
block **75372**, byte offset 77 180 928 into the partition.

On the device, as root, straight to the raw partition *underneath*
dm-verity ([full output](../../results/security/dm-verity/on-target-tamper-demo.txt)):

```
raw byte at /dev/mmcblk0p2 + 77180928: 0x13
write 0xff                                   -> raw byte now: 0xff   (the write is allowed)
drop caches, read the file through the verity root:
  sha256sum: can't read '.../btrfs.ko.xz': Input/output error
  device-mapper: verity: 179:2: data block 75372 is corrupted
  audit: type=1339 ... module=verity op=verify-data dev=179:2 sector=75372 res=0
write the original 0x13 back, drop caches   -> sha256 1b441078…, same as before
```

**Block 75372 — the number computed on the build host from the image
file, reported by the kernel on the board.** The image's data block size
is 1024 bytes (meta-security's default), the same as the filesystem's,
which is why the two numbers are identical.

(The kernel lines appear twice in the saved log: the second listing, after
the restore, repeats the first because the log wasn't cleared in between —
the timestamps are the same. No new verity error followed the restore.)

## What this shows

- **dm-verity is detection at read time, not write protection.** Root on
  the device wrote the raw partition without any objection. What it can't
  do is make the kernel *serve* the modified block.
- **It has no memory.** Writing the original byte back made the block
  valid again — nothing records that the card was ever modified. Evidence
  of tampering exists only in the kernel log and the audit record of that
  boot.
- **The error mode decides what "detected" means.** The table was set up
  with the default: return `EIO`. A modified `device-service` binary would
  therefore give a service that crashes and restarts forever on a system
  that otherwise keeps running. A product would choose
  `restart_on_corruption` or `panic_on_corruption` deliberately, and pair
  it with an A/B fallback ([update-and-provisioning](../security/update-and-provisioning.md))
  so a corrupted slot is abandoned rather than rebooted into forever.
- **The protection is only as trustworthy as the root hash's origin.**
  Here the hash travels inside the initramfs, inside a kernel on the
  unverified FAT boot partition. Someone with the card can rebuild the
  tree, put the new root hash into a new initramfs, and boot that. This
  image detects corruption and on-device tampering; against an attacker
  with the card it needs the kernel to be part of a signed `boot.img`
  ([secure-boot](../security/secure-boot.md)).

## Takeaways

- **Compare the artifact you ship with the thing the proof was computed
  over, byte for byte.** A successful build and a normal-looking image
  said nothing about the label written into the superblock; `cmp` did.
- **Predict the result before running it on the board.** Computing the
  block number on the host first turned "it returned an I/O error" into
  "it rejected exactly the block that was changed" — a much stronger
  statement, for no extra effort.
- **Reversible tests on the only card**: read the original byte first,
  restore it, and confirm the file's hash afterwards.

Found along the way, not yet acted on: verity hashes with
`sha256-generic`, the portable C implementation. The Cortex-A76 has the
Armv8 SHA-2 instructions, and `CONFIG_CRYPTO_SHA2_ARM64_CE=m`: by the
time the system is up, `sha2_ce` is loaded and `/proc/crypto` lists
`sha256-ce` — but the verity target was created in the initramfs, before
the module existed, bound the generic implementation, and keeps it.
Building the module in is the next change to measure; this project
measures before it claims a speed-up.
