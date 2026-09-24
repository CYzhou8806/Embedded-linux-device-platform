# device-platform: system.conf from this layer, keyring = the Update CA
# certificate (public; the key stays in the HSM), from a path outside the
# repo set in local.conf:
#   DEVICE_PLATFORM_RAUC_KEYRING = "/home/.../.device-platform-signing/update/update-ca.cert.pem"
DEVICE_PLATFORM_RAUC_KEYRING ??= ""
DEVICE_PLATFORM_RELEASE ??= "0.0.0"
# The certificate's directory goes on the search path so it is fetched by
# its plain name, like any file of the recipe.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${@os.path.dirname(d.getVar('DEVICE_PLATFORM_RAUC_KEYRING') or '/nonexistent/x')}:"
RAUC_KEYRING_FILE = "${@os.path.basename(d.getVar('DEVICE_PLATFORM_RAUC_KEYRING') or 'update-ca.cert.pem')}"
do_install[file-checksums] += "${DEVICE_PLATFORM_RAUC_KEYRING}:True"

do_install:append() {
	sed -i -e "s/@MIN_BUNDLE_VERSION@/${DEVICE_PLATFORM_RELEASE}/" \
	       -e "s/@KEYRING@/${RAUC_KEYRING_FILE}/" ${D}${sysconfdir}/rauc/system.conf
	# A keyring with check-crl=true but no CRL in it rejects every bundle
	# (measured, update-and-provisioning.md §6) - refuse to build that.
	grep -q "BEGIN X509 CRL" ${D}${sysconfdir}/rauc/${RAUC_KEYRING_FILE} || \
		bbfatal "check-crl=true but ${RAUC_KEYRING_FILE} contains no CRL"
}
