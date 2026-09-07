SUMMARY = "C++ userspace service consuming /dev/acq0"
DESCRIPTION = "Long-running acquisition service (Config/Logging/Metrics/ \
Watchdog/systemd, see userspace/device-service/README.md at the repo \
root) built and packaged from source unmodified - not copied into this \
layer."
LICENSE = "CLOSED"

DEPENDS = "nlohmann-json spdlog systemd"

inherit cmake pkgconfig systemd

# Points back at the real source under userspace/device-service/ (4
# levels up from this recipe's own directory) instead of copying it into
# the layer - the whole directory is fetched as one SRC_URI entry.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../userspace:"

SRC_URI = "file://device-service"

S = "${WORKDIR}/device-service"

# GTest isn't needed on target - device-service/CMakeLists.txt already
# has this option for exactly this case (see userspace/device-service/
# tests/CMakeLists.txt, which needs GTest::gtest).
EXTRA_OECMAKE = "-DBUILD_TESTING=OFF"

SYSTEMD_SERVICE:${PN} = "device-service.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
	# CMakeLists.txt's install(TARGETS ...) already places the binary at
	# /opt/device-service/ - only the config and the systemd unit (not
	# CMake's concern) need adding here.
	install -d ${D}/opt/device-service
	install -m 0644 ${S}/config.example.json ${D}/opt/device-service/config.json

	install -d ${D}${systemd_system_unitdir}
	install -m 0644 ${S}/systemd/device-service.service ${D}${systemd_system_unitdir}/device-service.service
}

FILES:${PN} += "/opt/device-service/device-service /opt/device-service/config.json ${systemd_system_unitdir}/device-service.service"
CONFFILES:${PN} += "/opt/device-service/config.json"
