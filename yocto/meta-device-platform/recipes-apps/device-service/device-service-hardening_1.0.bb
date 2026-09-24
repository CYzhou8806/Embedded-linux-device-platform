SUMMARY = "systemd sandbox for device-service (production image only)"
DESCRIPTION = "A drop-in that runs device-service with only the privileges \
it needs. Separate from device-service itself so the development image \
keeps the unrestricted unit. See docs/security/hardening.md (F11)."
LICENSE = "CLOSED"

SRC_URI = "file://hardening.conf"

RDEPENDS:${PN} = "device-service"

do_install() {
	install -d ${D}${systemd_system_unitdir}/device-service.service.d
	install -m 0644 ${WORKDIR}/hardening.conf ${D}${systemd_system_unitdir}/device-service.service.d/hardening.conf
}

FILES:${PN} = "${systemd_system_unitdir}/device-service.service.d"
