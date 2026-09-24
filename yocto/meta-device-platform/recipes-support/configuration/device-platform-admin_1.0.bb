SUMMARY = "Unprivileged admin account with key-only SSH access"
DESCRIPTION = "Replaces debug-tweaks' passwordless root in the production \
image: an 'admin' account that can only log in with the SSH key named by \
DEVICE_PLATFORM_ADMIN_PUBKEY and uses sudo for root. See \
docs/security/hardening.md (finding F1)."
LICENSE = "CLOSED"

# A public key isn't secret, but which key is authorised is a per-deployment
# decision, so it lives outside the repo like the WiFi credentials. Falls
# back to a placeholder that authorises nobody, so a fresh checkout still
# builds - it just can't be logged into.
DEVICE_PLATFORM_ADMIN_PUBKEY ?= "${THISDIR}/files/admin_authorized_keys.example"
do_install[file-checksums] += "${DEVICE_PLATFORM_ADMIN_PUBKEY}:True"

SRC_URI = "file://admin.sudoers"

inherit useradd

USERADD_PACKAGES = "${PN}"
# '*' rather than a locked '!': no password can ever match, but dropbear
# still accepts the key. dropbear treats a '!'-prefixed hash as a locked
# account and refuses public-key logins too.
USERADD_PARAM:${PN} = "--create-home --home-dir /home/admin --shell /bin/sh --password '*' admin"

RDEPENDS:${PN} = "sudo"

do_install() {
	install -d -m 0700 -o admin -g admin ${D}/home/admin/.ssh
	install -m 0600 -o admin -g admin ${DEVICE_PLATFORM_ADMIN_PUBKEY} ${D}/home/admin/.ssh/authorized_keys

	install -d -m 0750 ${D}${sysconfdir}/sudoers.d
	install -m 0440 ${WORKDIR}/admin.sudoers ${D}${sysconfdir}/sudoers.d/admin
}

FILES:${PN} = "/home/admin ${sysconfdir}/sudoers.d/admin"
