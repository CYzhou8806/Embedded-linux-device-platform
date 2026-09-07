SUMMARY = "Raspberry Pi 5 image running the full custom-acq acquisition stack"
DESCRIPTION = "Plan.md V6: core-image-minimal plus the custom-acq kernel \
driver, its Device Tree overlay, device-service (systemd-managed), and \
WiFi/SSH so it's actually reachable. See yocto/meta-device-platform's \
four recipe categories for what each piece is."

require recipes-core/images/core-image-minimal.bb

IMAGE_INSTALL:append = " \
    custom-acq-driver \
    custom-acq-overlay \
    device-service \
    device-platform-network \
    kernel-modules \
    linux-firmware-rpidistro-bcm43455 \
    linux-firmware-rpidistro-bcm43456 \
    "

# core-image-minimal installs packagegroup-core-boot only, which does NOT
# pull in packagegroup-base - that's the packagegroup that actually
# consumes raspberrypi5.conf's MACHINE_EXTRA_RRECOMMENDS (WiFi/BT
# firmware) and installs kernel-modules. Found the hard way: wlan0 never
# appeared on real hardware (no brcmfmac module on the target at all,
# despite the firmware being built) because core-image-minimal simply
# never asked for it. kernel-modules pulls in every module the kernel
# build produced (brcmfmac + its dependencies included) rather than
# hand-picking just the WiFi ones - simpler and matches what
# packagegroup-base itself would have done had it been in this image's
# default install chain.

# Same debug-tweaks default as the V6 first-phase stock image (root,
# blank password, console login) - kept for this round too since we
# don't have a proper user-management story yet. ssh-server-dropbear
# adds a lightweight SSH server (see Plan.md's V6 plan file for why
# dropbear over openssh - it's already a ready-made recipe in oe-core).
EXTRA_IMAGE_FEATURES += "debug-tweaks ssh-server-dropbear"

# custom-acq-overlay's do_deploy has to run before the boot partition is
# assembled, and its output (custom-acq.dtbo) has to be named in
# KERNEL_DEVICETREE for meta-raspberrypi's IMAGE_BOOT_FILES machinery
# (rpi-base.inc's make_dtb_boot_files()) to copy it into overlays/ on
# the boot partition.
EXTRA_IMAGEDEPENDS += "custom-acq-overlay"
KERNEL_DEVICETREE:append = " overlays/custom-acq.dtbo"

# The actual "dtoverlay=custom-acq" config.txt line lives in this
# layer's conf/layer.conf, not here - rpi-bootfiles (which generates
# config.txt) is a shared recipe, not specific to this image, so a
# variable set only in this recipe has no effect on it. See layer.conf's
# RPI_EXTRA_CONFIG comment.
