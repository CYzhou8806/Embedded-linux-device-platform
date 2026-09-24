SUMMARY = "A/B updates, encrypted data partition and unprivileged device-service for device-platform-image-ab"
DESCRIPTION = "RAUC custom backend for the Raspberry Pi 5 firmware's \
autoboot.txt/tryboot A/B, the health check that commits a slot, the LUKS \
data partition (/data) that keeps state across the read-only root, and the \
udev rule + drop-in that run device-service as user acq. See \
docs/security/update-and-provisioning.md and integrity-and-encryption.md."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://rauc-tryboot-backend \
    file://device-platform-data \
    file://device-platform-data.service \
    file://dropbear-data.conf \
    file://dropbearkey-data.conf \
    file://device-platform-healthcheck \
    file://device-platform-healthcheck.service \
    file://device-platform-healthcheck.timer \
    file://device-platform-update \
    file://99-custom-acq.rules \
    file://device-service-user.conf \
    file://dropbear-hostkey \
    file://device-platform-bootlog \
    file://device-platform-bootlog.service \
    file://device-platform-bootlog.timer \
    file://90-device-platform-hardening.conf \
    file://device-platform-clock-floor \
    file://device-platform-clock-floor.service \
    file://device-platform-clock-save.service \
    file://device-platform-clock-save.timer \
    file://device-platform-mcu-auth \
    file://device-service-mcu-auth.conf \
"

inherit systemd useradd

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system acq"
USERADD_PARAM:${PN} = "--system --no-create-home --home-dir /nonexistent --shell /sbin/nologin --gid acq acq"

SYSTEMD_SERVICE:${PN} = "device-platform-data.service device-platform-healthcheck.timer device-platform-bootlog.timer \
    device-platform-clock-floor.service device-platform-clock-save.timer"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} = "rauc cryptsetup e2fsprogs-mke2fs util-linux-mount device-service openssl-bin"

do_install() {
	install -d ${D}${libdir}/device-platform ${D}${sbindir}
	install -m 0755 ${WORKDIR}/rauc-tryboot-backend ${D}${libdir}/device-platform/
	install -m 0755 ${WORKDIR}/device-platform-data ${D}${sbindir}/
	install -m 0755 ${WORKDIR}/device-platform-healthcheck ${D}${sbindir}/
	install -m 0755 ${WORKDIR}/device-platform-update ${D}${sbindir}/
	install -m 0755 ${WORKDIR}/dropbear-hostkey ${D}${sbindir}/
	install -m 0755 ${WORKDIR}/device-platform-bootlog ${D}${sbindir}/
	install -m 0755 ${WORKDIR}/device-platform-clock-floor ${D}${sbindir}/
	install -m 0755 ${WORKDIR}/device-platform-mcu-auth ${D}${sbindir}/

	install -d ${D}${systemd_system_unitdir}/dropbearkey.service.d \
	           ${D}${systemd_system_unitdir}/dropbear@.service.d \
	           ${D}${systemd_system_unitdir}/device-service.service.d
	install -m 0644 ${WORKDIR}/device-platform-data.service ${D}${systemd_system_unitdir}/
	install -m 0644 ${WORKDIR}/device-platform-healthcheck.service ${WORKDIR}/device-platform-healthcheck.timer ${D}${systemd_system_unitdir}/
	install -m 0644 ${WORKDIR}/device-platform-bootlog.service ${WORKDIR}/device-platform-bootlog.timer ${D}${systemd_system_unitdir}/
	install -m 0644 ${WORKDIR}/device-platform-clock-floor.service ${WORKDIR}/device-platform-clock-save.service \
		${WORKDIR}/device-platform-clock-save.timer ${D}${systemd_system_unitdir}/
	install -m 0644 ${WORKDIR}/dropbearkey-data.conf ${D}${systemd_system_unitdir}/dropbearkey.service.d/data.conf
	install -m 0644 ${WORKDIR}/dropbear-data.conf ${D}${systemd_system_unitdir}/dropbear@.service.d/data.conf
	install -m 0644 ${WORKDIR}/device-service-user.conf ${D}${systemd_system_unitdir}/device-service.service.d/user.conf
	install -m 0644 ${WORKDIR}/device-service-mcu-auth.conf ${D}${systemd_system_unitdir}/device-service.service.d/mcu-auth.conf

	install -d ${D}${sysconfdir}/sysctl.d
	install -m 0644 ${WORKDIR}/90-device-platform-hardening.conf ${D}${sysconfdir}/sysctl.d/

	install -d ${D}${sysconfdir}/udev/rules.d
	install -m 0644 ${WORKDIR}/99-custom-acq.rules ${D}${sysconfdir}/udev/rules.d/
}

FILES:${PN} += "${libdir}/device-platform ${systemd_system_unitdir}"
