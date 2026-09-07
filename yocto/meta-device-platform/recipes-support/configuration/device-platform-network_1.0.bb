SUMMARY = "WiFi + SSH configuration for the device-platform image"
DESCRIPTION = "Ties together wpa_supplicant (association), systemd-networkd \
(DHCP once associated), and a real wpa_supplicant.conf - see \
DEVICE_PLATFORM_WPA_CONF."
LICENSE = "CLOSED"

# The real SSID/password can't go in this git-tracked recipe. Override
# DEVICE_PLATFORM_WPA_CONF in local.conf (outside the repo) to point at
# a real file - see /opt/yocto/local-config/wpa_supplicant.conf, which
# is NOT tracked anywhere in this repo. Falls back to the checked-in
# placeholder template if not overridden, so a fresh checkout still
# builds (just without working WiFi until someone sets the real path).
DEVICE_PLATFORM_WPA_CONF ?= "${THISDIR}/files/wpa_supplicant.conf.example"

SRC_URI = "file://wlan0.network \
           file://wpa_supplicant-wlan0.service \
           file://99-device-platform.preset \
          "

RDEPENDS:${PN} = "wpa-supplicant"

inherit systemd

SYSTEMD_SERVICE:${PN} = "wpa_supplicant-wlan0.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
	install -d ${D}${systemd_unitdir}/network
	install -m 0644 ${WORKDIR}/wlan0.network ${D}${systemd_unitdir}/network/wlan0.network

	install -d ${D}${systemd_system_unitdir}
	install -m 0644 ${WORKDIR}/wpa_supplicant-wlan0.service ${D}${systemd_system_unitdir}/wpa_supplicant-wlan0.service

	# systemd-networkd itself ships as part of the systemd package (this
	# recipe doesn't own that service), so it's enabled via a preset
	# rather than SYSTEMD_AUTO_ENABLE (which only applies to services
	# this recipe packages).
	install -d ${D}${sysconfdir}/systemd/system-preset
	install -m 0644 ${WORKDIR}/99-device-platform.preset ${D}${sysconfdir}/systemd/system-preset/99-device-platform.preset

	install -d ${D}${sysconfdir}/wpa_supplicant
	install -m 0600 ${DEVICE_PLATFORM_WPA_CONF} ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf
}

FILES:${PN} = "${systemd_unitdir}/network/wlan0.network \
               ${systemd_system_unitdir}/wpa_supplicant-wlan0.service \
               ${sysconfdir}/systemd/system-preset/99-device-platform.preset \
               ${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf \
              "
CONFFILES:${PN} += "${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf"
