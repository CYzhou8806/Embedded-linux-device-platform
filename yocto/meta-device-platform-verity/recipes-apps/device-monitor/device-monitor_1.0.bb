SUMMARY = "Read-only remote monitoring of device-service over mutual TLS"
DESCRIPTION = "Serves device-service's status as JSON and Prometheus \
metrics over TLS 1.3 with client certificates. A separate, sandboxed \
process with network access and no hardware access; see \
docs/security/remote-monitoring.md. Lives in the verity layer because it \
relies on the A/B image's acq group and /data partition."
LICENSE = "CLOSED"

DEPENDS = "openssl nlohmann-json"

inherit cmake systemd

# Source stays in userspace/device-monitor/ (four levels up), like
# device-service's recipe - no copy in the layer.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../userspace:"
SRC_URI = "file://device-monitor"
S = "${WORKDIR}/device-monitor"

EXTRA_OECMAKE = "-DBUILD_TESTING=OFF"

SYSTEMD_SERVICE:${PN} = "device-monitor.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
	install -m 0644 ${S}/config.example.json ${D}/opt/device-monitor/config.json
	install -d ${D}${systemd_system_unitdir}
	install -m 0644 ${S}/systemd/device-monitor.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} += "/opt/device-monitor ${systemd_system_unitdir}/device-monitor.service"
CONFFILES:${PN} += "/opt/device-monitor/config.json"
RDEPENDS:${PN} = "device-service"
