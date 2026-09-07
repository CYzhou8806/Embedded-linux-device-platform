SUMMARY = "Device Tree overlay for the custom-acq SPI acquisition peripheral"
DESCRIPTION = "Compiles device-tree/custom-acq-overlay.dts (repo root) into \
custom-acq.dtbo and deploys it where meta-raspberrypi's boot-file assembly \
expects overlays: see device-tree/README.md for what the overlay does \
(disables the stock spidev0 node, adds the custom-acq SPI child node)."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

DEPENDS = "dtc-native"

inherit deploy

# Points back at the real source under device-tree/ (4 levels up from
# this recipe's own directory) instead of copying it into the layer.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../device-tree:"

SRC_URI = "file://custom-acq-overlay.dts"

S = "${WORKDIR}"

do_compile() {
	dtc -@ -I dts -O dtb -o custom-acq.dtbo ${S}/custom-acq-overlay.dts
}

do_install() {
	install -d ${D}/boot
	install -Dm 0644 ${B}/custom-acq.dtbo ${D}/boot/custom-acq.dtbo
}

do_deploy() {
	# Flat in DEPLOYDIR (not a subdirectory) - matches how
	# meta-raspberrypi's make_dtb_boot_files() (rpi-base.inc) resolves
	# each KERNEL_DEVICETREE entry: "basename;overlays/basename" expects
	# the file to sit at ${DEPLOYDIR}/basename.
	install -Dm 0644 ${B}/custom-acq.dtbo ${DEPLOYDIR}/custom-acq.dtbo
}
addtask deploy before do_build after do_install

FILES:${PN} = "/boot/custom-acq.dtbo"
