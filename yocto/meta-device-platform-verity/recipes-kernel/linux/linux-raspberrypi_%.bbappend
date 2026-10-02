# Built-in device-mapper targets for the dm-verity image. This layer is only
# in the dm-verity build directory's bblayers.conf, never in the main build:
# the main build's kernel is the "stock" kernel the RT experiments compare
# against, and even a conditional SRC_URI:append changes its task hashes
# (BitBake hashes the unexpanded variable text).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = " file://dm-builtin.cfg file://hardening.cfg file://hardening-kspp.cfg"

# kernel-base recommends kernel-image, so anything that pulls in a kernel
# module package (cryptsetup's recommendations do, in the initramfs) also
# installs the 28 MB kernel Image into /boot. The firmware loads the kernel
# from the boot partition; the copy in the rootfs was never used, and the
# copy in the initramfs was bundled into the kernel it came from - 82 MB of
# kernel_2712.img instead of 54.
RRECOMMENDS:${KERNEL_PACKAGE_NAME}-base = ""
