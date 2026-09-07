SUMMARY = "Kernel driver for the custom STM32 SPI acquisition peripheral"
DESCRIPTION = "Out-of-tree SPI driver for the v1-spi-slave-handshake MCU \
firmware: register read/write via sysfs, a threaded GPIO IRQ draining the \
MCU's hardware FIFO into a kernel kfifo, and /dev/acq0 exposing it to \
userspace. Source lives in driver/custom-acq/ at the repo root, not \
copied into this layer - see FILESEXTRAPATHS below."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

# Points back at the real source under driver/custom-acq/ (4 levels up
# from this recipe's own directory) instead of copying it into the
# layer, so there is exactly one copy to keep in sync.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../driver/custom-acq:"

SRC_URI = "file://custom_acq.c \
           file://Makefile \
          "

S = "${WORKDIR}"

# driver/custom-acq/Makefile uses its own KDIR variable (for the plain
# on-target `make` workflow documented in device-tree/README.md, falling
# back to `uname -r` when unset) rather than the KERNEL_SRC variable
# module.bbclass passes by convention (see
# meta-skeleton/recipes-kernel/hello-mod's Makefile for that convention).
# Passing KDIR explicitly here - instead of changing the shared Makefile -
# keeps the Yocto-specific glue self-contained in this layer.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

# Belt-and-braces: the module *should* auto-load via udev/kmod matching
# custom_acq.c's MODULE_DEVICE_TABLE(of, ...) against the DT overlay's
# "edp,custom-acq" compatible string once custom-acq-overlay is active,
# but explicitly listing it here means it loads at boot regardless of
# whether that auto-detection path works in this minimal image.
do_install:append() {
	install -d ${D}${sysconfdir}/modules-load.d
	echo "custom_acq" > ${D}${sysconfdir}/modules-load.d/custom-acq.conf
}

FILES:${PN} += "${sysconfdir}/modules-load.d/custom-acq.conf"

RPROVIDES:${PN} += "kernel-module-custom-acq"
