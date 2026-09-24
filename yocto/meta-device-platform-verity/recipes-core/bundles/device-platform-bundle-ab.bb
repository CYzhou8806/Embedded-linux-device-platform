SUMMARY = "RAUC bundle for device-platform-image-ab: boot partition + dm-verity root"
DESCRIPTION = "Both parts of a slot go together: the boot partition carries \
the kernel whose initramfs holds the new root's verity root hash. Signed \
here with the development key; re-signed for release with \
security/signing/sign-release-bundle.sh."
LICENSE = "MIT"

inherit bundle

RAUC_BUNDLE_COMPATIBLE = "device-platform-rpi5"
DEVICE_PLATFORM_BUNDLE_VERSION ??= "1.0.1"
RAUC_BUNDLE_VERSION = "${DEVICE_PLATFORM_BUNDLE_VERSION}"
RAUC_BUNDLE_DESCRIPTION = "device-platform A/B update"
RAUC_BUNDLE_FORMAT = "verity"

RAUC_BUNDLE_SLOTS = "rootfs boot"
RAUC_SLOT_rootfs = "device-platform-image-ab"
RAUC_SLOT_rootfs[fstype] = "ext4.verity"
# RAUC picks the image type from the file extension and doesn't know
# ".verity"; ".img" = raw, written byte for byte - which is what a verity
# image (data + hash tree) has to be.
RAUC_SLOT_rootfs[rename] = "rootfs-verity.img"
RAUC_SLOT_boot = "device-platform-image-ab"
RAUC_SLOT_boot[fstype] = "boot.vfat"
RAUC_SLOT_boot[type] = "image"

DEVICE_PLATFORM_RAUC_DEV_DIR ??= ""
RAUC_KEY_FILE = "${DEVICE_PLATFORM_RAUC_DEV_DIR}/dev.key.pem"
RAUC_CERT_FILE = "${DEVICE_PLATFORM_RAUC_DEV_DIR}/dev.cert.pem"
RAUC_KEYRING_FILE = "${DEVICE_PLATFORM_RAUC_DEV_DIR}/dev-ca.cert.pem"
