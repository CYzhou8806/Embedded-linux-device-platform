# meta-device-platform-verity

`device-platform-image-verity`: the production image
(`device-platform-image-prod`) with its root filesystem protected by
dm-verity. See [docs/security/integrity-and-encryption.md](../../docs/security/integrity-and-encryption.md).

**Status: booted on the Raspberry Pi 5 and tampered with** — see
[case 10](../../docs/debugging/case-10-dm-verity-one-byte-on-the-card.md).
What is checked on the build output before writing a card:

- the rootfs partition of the `.wic` is byte-identical to the
  `.ext4.verity` image the root hash was computed over;
- the boot partition's `kernel_2712.img` is the kernel with the initramfs
  bundled into it;
- the kernel has `CONFIG_BLK_DEV_DM`, `CONFIG_DM_VERITY`, `CONFIG_DM_CRYPT`
  built in;
- the root hash in the initramfs (`/usr/share/misc/dm-verity.env`)
  verifies the rootfs partition with `veritysetup verify`.

## device-platform-image-ab

The same verified root, on an A/B layout with RAUC and an encrypted data
partition — see [update-and-provisioning](../../docs/security/update-and-provisioning.md)
§3 and [integrity-and-encryption](../../docs/security/integrity-and-encryption.md)
§5. Built in the same build directory with `DM_VERITY_IMAGE =
"device-platform-image-ab"` (the initramfs carries the root hash of exactly
one image per build), plus `DEVICE_PLATFORM_RAUC_KEYRING`,
`DEVICE_PLATFORM_RELEASE` (the anti-rollback floor) and
`DEVICE_PLATFORM_BUNDLE_VERSION` in `local.conf`.

```bash
bitbake device-platform-image-ab device-platform-bundle-ab
tests/test-rauc-backend.sh      # the A/B state machine, on the host
```

## Why a separate layer

Everything here changes the kernel (a config fragment, and meta-security's
initramfs bundled into it). The main build's kernel is the unmodified
meta-raspberrypi kernel that the RT experiments use as "stock", so this
layer only goes into a **separate build directory**. Keeping it in
`meta-device-platform` behind a variable didn't work: BitBake hashes the
unexpanded text of `SRC_URI`, so even a `SRC_URI:append` that expands to
nothing in the main build changed the kernel's task hashes and rebuilt it.

## Build

A second build directory next to the main one, sharing its downloads and
sstate:

```bash
source /opt/yocto/poky/oe-init-build-env /opt/yocto/build-verity
# conf/bblayers.conf: the main build's layers + /opt/yocto/meta-security
#                     + this layer
# conf/local.conf:    the main build's local.conf, plus:
#   DL_DIR = "/opt/yocto/build/downloads"
#   SSTATE_DIR = "/opt/yocto/build/sstate-cache"
#   IMAGE_CLASSES += "dm-verity-img"
#   DM_VERITY_IMAGE = "device-platform-image-verity"
#   DM_VERITY_IMAGE_TYPE = "ext4"
#   INITRAMFS_IMAGE = "dm-verity-image-initramfs"
#   INITRAMFS_IMAGE_BUNDLE = "1"
bitbake device-platform-image-verity
```

## Two things that break it silently

- **A label on the rootfs partition.** `wic` writes `--label` into the
  filesystem it copies, which changes the ext4 superblock *after* the hash
  tree was built. The image builds fine and fails verification on the
  first boot. The `.wks` sets only `--fstype` (partition type 83).
- **Anything that makes the partition differ from the `.verity` file** —
  the reason the rootfs is written with `rawcopy` rather than the normal
  `rootfs` plugin, which would recreate the filesystem.
