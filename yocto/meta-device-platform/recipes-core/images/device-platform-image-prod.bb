SUMMARY = "Production variant of device-platform-image"
DESCRIPTION = "Same software as device-platform-image, without the \
development conveniences: no passwordless root, no serial login, key-only \
SSH for an unprivileged admin, a read-only root filesystem and a sandboxed \
device-service. See docs/security/hardening.md for each change and the \
finding it closes."

# Everything the development image installs - the two images differ only in
# how the device can be reached and changed, never in what it runs. That
# keeps a bug found on one reproducible on the other.
require recipes-core/images/device-platform-image.bb

# F1: debug-tweaks = empty root password + root may log in + empty passwords
# accepted by the SSH server.
EXTRA_IMAGE_FEATURES:remove = "debug-tweaks"

IMAGE_INSTALL:append = " \
    device-platform-admin \
    device-service-hardening \
    "

# F5 (first half): nothing on the device may change the root filesystem.
# Integrity checking of it is dm-verity's job (integrity-and-encryption.md).
IMAGE_FEATURES += "read-only-rootfs"

# A plain ext4 image of the root filesystem, as the rootfs slot of the
# signed update bundle (recipes-core/bundles/device-platform-bundle.bb).
IMAGE_FSTYPES:append = " ext4"

ROOTFS_POSTPROCESS_COMMAND += "harden_dropbear; mask_serial_gettys;"

# F1: key-only SSH. -w: no root login. -s: no password logins at all.
# -j/-k: no local/remote port forwarding, so a stolen admin key can't be
# used to tunnel into the customer's network through the device.
harden_dropbear() {
	if [ -e ${IMAGE_ROOTFS}${sysconfdir}/default/dropbear ]; then
		sed -i '/^DROPBEAR_EXTRA_ARGS=/d' ${IMAGE_ROOTFS}${sysconfdir}/default/dropbear
	fi
	echo 'DROPBEAR_EXTRA_ARGS="-w -s -j -k"' >> ${IMAGE_ROOTFS}${sysconfdir}/default/dropbear
}

# F3: no login prompt on the UART header. SERIAL_CONSOLES is a MACHINE
# setting shared with the development image, and systemd-serialgetty is a
# hard dependency of systemd here, so the gettys are masked in this image
# only rather than removed from the build.
# SERIAL_CONSOLES is "baud;tty baud;tty ..." - split in Python, because the
# ';' would be a command separator if it reached the shell unquoted.
mask_serial_gettys() {
	for tty in ${@' '.join(c.split(';')[-1] for c in (d.getVar('SERIAL_CONSOLES') or '').split())}; do
		ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/serial-getty@$tty.service
	done
}
