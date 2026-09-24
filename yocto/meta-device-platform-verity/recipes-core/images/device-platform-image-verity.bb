SUMMARY = "device-platform-image-prod with a dm-verity protected root filesystem"
DESCRIPTION = "Needs a build directory configured for dm-verity \
(yocto/meta-device-platform-verity/README.md): this layer and meta-security \
in the layers, and the kernel bundling meta-security's \
dm-verity-image-initramfs, which opens the verity device before switching \
to the root filesystem."

require recipes-core/images/device-platform-image-prod.bb

# The rootfs partition is the .ext4.verity image itself (data + hash tree);
# its root hash reaches the initramfs through dm-verity.env.
WKS_FILE = "device-platform-verity.wks.in"
IMAGE_FSTYPES:append = " wic wic.bmap"
IMAGE_TYPEDEP:wic:append = " ${DM_VERITY_IMAGE_TYPE}.verity"

# Boot the kernel with the initramfs bundled into it, not the plain Image.
RPI_EXTRA_IMAGE_BOOT_FILES = "${KERNEL_IMAGETYPE}-${INITRAMFS_LINK_NAME}.bin;${SDIMG_KERNELIMAGE}"
