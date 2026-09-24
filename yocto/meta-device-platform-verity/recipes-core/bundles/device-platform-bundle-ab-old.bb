# The same bundle with an older version, to show min-bundle-version refusing
# it on a device built as 1.0.0 (anti-rollback, update-and-provisioning.md §4).
require device-platform-bundle-ab.bb
DEVICE_PLATFORM_BUNDLE_VERSION = "0.9.0"
