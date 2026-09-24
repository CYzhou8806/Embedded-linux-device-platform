SUMMARY = "ramoops Device Tree overlay for the Raspberry Pi 5 (BCM2712 cell layout)"
DESCRIPTION = "See files/ramoops-pi5-overlay.dts for why the kernel tree's ramoops \
overlays don't apply on a Pi 5. Deployed like custom-acq-overlay."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

DEPENDS = "dtc-native"
inherit deploy
SRC_URI = "file://ramoops-pi5-overlay.dts"
S = "${WORKDIR}"

do_compile() {
	dtc -@ -I dts -O dtb -o ramoops-pi5.dtbo ${S}/ramoops-pi5-overlay.dts
}
do_install() {
	install -Dm 0644 ${B}/ramoops-pi5.dtbo ${D}/boot/ramoops-pi5.dtbo
}
do_deploy() {
	install -Dm 0644 ${B}/ramoops-pi5.dtbo ${DEPLOYDIR}/ramoops-pi5.dtbo
}
addtask deploy before do_build after do_install
FILES:${PN} = "/boot/ramoops-pi5.dtbo"
