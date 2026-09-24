SUMMARY = "device-platform with A/B updates, dm-verity roots and an encrypted data partition"
DESCRIPTION = "device-platform-image-verity plus: RAUC with the Raspberry Pi \
5 firmware's autoboot.txt/tryboot A/B, a LUKS2 data partition for state that \
must survive the read-only root, device-service as an unprivileged user, and \
only the kernel modules this board uses. Build directory: DM_VERITY_IMAGE = \
this image. See yocto/meta-device-platform-verity/README.md."

require recipes-core/images/device-platform-image-verity.bb

# Attack surface: install the modules the running system actually loads
# (lsmod on the board, 2026-09-24), not all 1808 the kernel builds - and not
# Bluetooth or the camera/codec stack, which were loaded only because the
# hardware exists. custom-acq comes from its own recipe; dm, crypt, loop,
# squashfs and the crypto extensions are built into the kernel here.
# 6.12: the RP1 south-bridge MFD driver is built in (CONFIG_MFD_RP1=y), so
# there is no kernel-module-rp1 any more.
DEVICE_PLATFORM_MODULES = " \
    brcmfmac brcmfmac-wcc brcmutil cfg80211 rfkill \
    ipv6 fuse sch-fq-codel nfnetlink \
    ghash-ce gf128mul sha1-ce \
    v3d gpu-sched drm-shmem-helper \
    rp1-adc rp1-mailbox rp1-pio nvmem-rmem gpio-keys \
    raspberrypi-gpiomem raspberrypi-hwmon \
    spi-bcm2835 spi-dw spi-dw-mmio spidev uio uio-pdrv-genirq \
"
IMAGE_INSTALL:remove = "kernel-modules"
IMAGE_INSTALL:append = " \
    ${@' '.join('kernel-module-' + m for m in d.getVar('DEVICE_PLATFORM_MODULES').split())} \
    device-platform-ab \
"
# rauc-mark-good would mark every booted slot good unconditionally, before
# anything checked it - the health check (device-platform-healthcheck) is
# what decides here.
BAD_RECOMMENDATIONS += "rauc-mark-good"

EXTRA_IMAGEDEPENDS += "ramoops-pi5-overlay"
KERNEL_DEVICETREE:append = " overlays/ramoops-pi5.dtbo"

WKS_FILE = "device-platform-ab.wks.in"
IMAGE_FSTYPES:append = " wic wic.bmap"
do_image_wic[depends] += "dosfstools-native:do_populate_sysroot mtools-native:do_populate_sysroot"

# One command line per boot partition (config.txt picks it with
# [boot_partition=N], see conf/layer.conf).
RPI_EXTRA_IMAGE_BOOT_FILES:append = " ab/cmdline-a.txt;cmdline-a.txt ab/cmdline-b.txt;cmdline-b.txt"

do_image_wic[prefuncs] += "make_ab_boot_files"
make_ab_boot_files() {
	# Partition 1: nothing but autoboot.txt. Slot A committed, B to try.
	printf '[all]\ntryboot_a_b=1\nboot_partition=2\n[tryboot]\nboot_partition=3\n' > ${WORKDIR}/autoboot.txt
	rm -f ${IMGDEPLOYDIR}/autoboot.vfat
	mkfs.vfat -C -n AUTOBOOT ${IMGDEPLOYDIR}/autoboot.vfat 8192
	mcopy -i ${IMGDEPLOYDIR}/autoboot.vfat ${WORKDIR}/autoboot.txt ::autoboot.txt

	install -d ${DEPLOY_DIR_IMAGE}/ab
	base=$(cat ${DEPLOY_DIR_IMAGE}/${BOOTFILES_DIR_NAME}/cmdline.txt)
	echo "$base" | sed 's|root=/dev/mmcblk0p2|root=/dev/mmcblk0p5 rauc.slot=A|' > ${DEPLOY_DIR_IMAGE}/ab/cmdline-a.txt
	echo "$base" | sed 's|root=/dev/mmcblk0p2|root=/dev/mmcblk0p6 rauc.slot=B|' > ${DEPLOY_DIR_IMAGE}/ab/cmdline-b.txt
	grep -q rauc.slot=A ${DEPLOY_DIR_IMAGE}/ab/cmdline-a.txt || bbfatal "cmdline.txt has no root=/dev/mmcblk0p2 to replace"
}

ROOTFS_POSTPROCESS_COMMAND += "ab_rootfs_tweaks;"
ab_rootfs_tweaks() {
	# Mount point for the data partition (the root is read-only at runtime).
	install -d ${IMAGE_ROOTFS}/data
	# Floor for the clock of a board without an RTC battery
	# (device-platform-clock-floor): the device never believes it is
	# earlier than the image it runs. Written as content, because the
	# reproducible-build rootfs timestamps reset every file's mtime to 2018.
	date -u +%s > ${IMAGE_ROOTFS}${sysconfdir}/device-platform-build-time
	# SSH host key on /data: stable across reboots, generated on the device.
	sed -i 's|^DROPBEAR_RSAKEY_DIR=.*|DROPBEAR_RSAKEY_DIR=/data/dropbear|' ${IMAGE_ROOTFS}${sysconfdir}/default/dropbear
	grep -q '^DROPBEAR_RSAKEY_DIR=/data/dropbear' ${IMAGE_ROOTFS}${sysconfdir}/default/dropbear || \
		echo 'DROPBEAR_RSAKEY_DIR=/data/dropbear' >> ${IMAGE_ROOTFS}${sysconfdir}/default/dropbear
}

# The boot partition as a separate vfat image - the "boot" slot of the RAUC
# bundle (recipes-core/bundles/device-platform-bundle-ab.bb). Taken from
# partition 2 of the finished .wic, so the bundle carries exactly what the
# image boots.
do_image_wic[postfuncs] += "extract_boot_vfat"
python extract_boot_vfat() {
    import struct, os
    deploy = d.getVar('IMGDEPLOYDIR')
    wic = os.path.join(deploy, d.getVar('IMAGE_NAME') + '.wic')
    with open(wic, 'rb') as f:
        mbr = f.read(512)
        start, size = struct.unpack_from('<II', mbr, 0x1BE + 16 * 1 + 8)  # partition 2
        f.seek(start * 512)
        data = f.read(size * 512)
    out = os.path.join(deploy, d.getVar('IMAGE_NAME') + '.boot.vfat')
    with open(out, 'wb') as f:
        f.write(data)
    link = os.path.join(deploy, d.getVar('IMAGE_LINK_NAME') + '.boot.vfat')
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(os.path.basename(out), link)
}
