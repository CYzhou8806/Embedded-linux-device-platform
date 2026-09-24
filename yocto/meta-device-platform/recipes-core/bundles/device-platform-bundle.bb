SUMMARY = "Signed RAUC update bundle for device-platform-image-prod"
DESCRIPTION = "Carries the production root filesystem as a RAUC bundle. \
Built and signed here with a DEVELOPMENT key only; release bundles are \
re-signed with the key in the HSM by a separate step (security/signing/ \
sign-release-bundle.sh). See docs/security/update-and-provisioning.md."
LICENSE = "MIT"

inherit bundle

# Devices only accept bundles whose compatible string matches their
# system.conf, so an image for another product can't be installed by
# mistake - that's a safety check, not a security one (the signature is).
RAUC_BUNDLE_COMPATIBLE = "device-platform-rpi5"

# The version is what anti-rollback compares. It has to be set by the
# release process and only ever increase.
DEVICE_PLATFORM_RELEASE ?= "0.0.0-dev"
RAUC_BUNDLE_VERSION = "${DEVICE_PLATFORM_RELEASE}"
RAUC_BUNDLE_DESCRIPTION = "device-platform production image"

# verity format: the payload is protected by a dm-verity hash tree whose
# root hash is in the signed manifest, so the device can check the bundle
# block by block while installing instead of trusting the whole file up
# front.
RAUC_BUNDLE_FORMAT = "verity"

RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs = "device-platform-image-prod"
RAUC_SLOT_rootfs[fstype] = "ext4"

# Development signing material, outside the repo (local.conf):
#   DEVICE_PLATFORM_RAUC_DEV_DIR = "/opt/yocto/local-config/rauc-dev"
# A production device's keyring does not contain the development CA, so a
# bundle signed here can't be installed on one until it is re-signed.
DEVICE_PLATFORM_RAUC_DEV_DIR ??= ""
RAUC_KEY_FILE = "${DEVICE_PLATFORM_RAUC_DEV_DIR}/dev.key.pem"
RAUC_CERT_FILE = "${DEVICE_PLATFORM_RAUC_DEV_DIR}/dev.cert.pem"
RAUC_KEYRING_FILE = "${DEVICE_PLATFORM_RAUC_DEV_DIR}/dev-ca.cert.pem"
