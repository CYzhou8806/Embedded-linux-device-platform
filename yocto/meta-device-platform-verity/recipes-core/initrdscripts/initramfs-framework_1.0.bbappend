# Replace meta-security's dm-verity initramfs module with a copy that sets
# the verity error mode (files/dmverity-errmode).
# Not a do_install:append: meta-security's bbappend has the higher layer
# priority (8 vs 7), so its append runs after ours and installed the
# upstream file over this one - found by extracting the built initramfs.
# A postfunc runs after the whole do_install, appends included.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = " file://dmverity-errmode"
do_install[postfuncs] += "device_platform_dmverity_errmode"
device_platform_dmverity_errmode() {
	if [ -e ${D}/init.d/80-dmverity ]; then
		install -m 0755 ${WORKDIR}/dmverity-errmode ${D}/init.d/80-dmverity
	fi
}
