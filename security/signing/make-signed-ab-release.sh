#!/usr/bin/env bash
# Secure boot and A/B updates together: put a signed boot.img into both boot
# slots of device-platform-image-ab, and build the matching RAUC bundle.
#
#   make-signed-ab-release.sh <version> <out-dir>
#
# Runs on the signing host after `bitbake device-platform-image-ab` (build
# directory /opt/yocto/build-verity, environment sourced for rauc-native).
# The build never sees the boot key; this step does, through the HSM.
#
#   bitbake output                         this script
#   p2/p3: FAT with kernel+initramfs,  ->  p2/p3: FAT with only
#          DTBs, overlays, config.txt,            boot.img  (FAT ramdisk: kernel with the
#          cmdline-a/b.txt                                   verity root hash, DTBs, overlays,
#                                                            config.txt/tryboot.txt, cmdline*)
#                                                 boot.sig  (RSA-2048, HSM key 01)
#                                                 config.txt: boot_ramdisk=1
#
# Outputs: <out>/device-platform-image-ab-signed.img (to flash),
#          <out>/boot-signed.vfat (the RAUC "boot" slot image),
#          <out>/update-<version>-signed.raucb (release-signed bundle).
# docs/security/secure-boot.md §3.5, update-and-provisioning.md §3.2.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
version="$1"; out="$(realpath -m "$2")"
: "${DEPLOY:=/opt/yocto/build-verity/tmp/deploy/images/raspberrypi5}"
: "${RAUC_DEV_DIR:=/opt/yocto/local-config/rauc-dev}"
img=device-platform-image-ab-raspberrypi5
wic="$DEPLOY/$img.rootfs.wic"
bootvfat="$DEPLOY/$img.rootfs.boot.vfat"
verity="$(readlink -f "$DEPLOY/$img.rootfs.ext4.verity")"
envf="${VERITY_ENV:-/opt/yocto/build-verity/tmp/work-shared/raspberrypi5/dm-verity/device-platform-image-ab.ext4.verity.env}"
work="$out/work"
rm -rf "$work"; mkdir -p "$work/ramdisk" "$work/outer" "$out"

echo "== 0. release gate: systemd ordering cycles and missing commands in the rootfs"
# Both failure classes have reached the board from clean builds (case 11,
# and update 1.3.0 on 2026-10-02); both are visible offline. Nothing is
# signed if this fails.
: "${ROOTFS_DIR:=$(ls -d /opt/yocto/build-verity/tmp/work/raspberrypi5-poky-linux/device-platform-image-ab/*/rootfs)}"
python3 "$here/../../tools/check-rootfs-units.py" "$ROOTFS_DIR" | sed 's/^/  /'
[[ ${PIPESTATUS[0]} -eq 0 ]] || { echo "release gate failed - not signing"; exit 1; }

echo "== 1. ramdisk contents, from the boot partition bitbake built"
mcopy -s -n -i "$bootvfat" ::/* "$work/ramdisk/"
# The Pi 5 loads its firmware from the EEPROM; start*.elf/fixup*/bootcode.bin
# on the card are for older boards.
rm -f "$work/ramdisk"/start*.elf "$work/ramdisk"/fixup*.dat "$work/ramdisk"/bootcode.bin \
	"$work/ramdisk"/*.stamp
# In tryboot mode the second configuration pass inside the ramdisk reads
# tryboot.txt (found on the board, secure-boot.md §3.4). With tryboot_a_b=1
# it should read config.txt; both are present so neither case loses the
# dtoverlays.
cp "$work/ramdisk/config.txt" "$work/ramdisk/tryboot.txt"
# config.txt picks cmdline-a/-b.txt with [boot_partition=N]. Whether that
# filter applies inside a ramdisk is not documented; if it doesn't, the
# default cmdline.txt is used - bitbake's says root=/dev/mmcblk0p2, which is
# a FAT partition here and would not boot at all. Slot A as the default
# keeps the device bootable; a B boot that falls back to it fails the
# verity check (B's root hash) and the one-shot tryboot returns to A.
cp "$work/ramdisk/cmdline-a.txt" "$work/ramdisk/cmdline.txt"
cmp "$work/ramdisk/kernel_2712.img" "$(readlink -f "$DEPLOY/Image-initramfs-raspberrypi5.bin")" \
	&& echo "kernel_2712.img == Image-initramfs (kernel with the verity initramfs)"
grep -H . "$work/ramdisk"/cmdline*.txt | sed 's/^/  /'

echo "== 2. boot.img + boot.sig (HSM key 01)"
"$here/make-signed-boot-img.sh" "$work/ramdisk" "$work/outer" | sed 's/^/  /'

echo "== 3. outer boot partition: boot.img, boot.sig, config.txt"
printf '# Signed-ramdisk boot: everything else is inside boot.img.\nboot_ramdisk=1\n' \
	> "$work/outer/config.txt"
cp "$work/outer/config.txt" "$work/outer/tryboot.txt"
size_kib=$(( $(stat -L -c %s "$bootvfat") / 1024 ))
rm -f "$out/boot-signed.vfat"
mkfs.vfat -C -n BOOTSIGNED "$out/boot-signed.vfat" "$size_kib" >/dev/null
mcopy -i "$out/boot-signed.vfat" "$work/outer"/boot.img "$work/outer"/boot.sig \
	"$work/outer"/config.txt "$work/outer"/tryboot.txt ::
mdir -b -i "$out/boot-signed.vfat" :: | sed 's/^/  /'

echo "== 4. flashable image: p2 and p3 replaced"
cp --sparse=always "$wic" "$out/$img-signed.img"
part() {  # part <n> -> "start_bytes size_bytes" (p5+ are logical: sfdisk reads the EBRs)
	sfdisk --json "$wic" | python3 -c '
import json, sys
n = int(sys.argv[1])
for p in json.load(sys.stdin)["partitiontable"]["partitions"]:
    if p["node"].endswith(str(n)) and not p["node"][-len(str(n))-1].isdigit():
        print(p["start"] * 512, p["size"] * 512)' "$1"
}
for p in 2 3; do
	read -r start size < <(part $p)
	(( $(stat -c %s "$out/boot-signed.vfat") <= size )) || { echo "boot-signed.vfat larger than p$p"; exit 1; }
	dd if="$out/boot-signed.vfat" of="$out/$img-signed.img" bs=1M \
		seek="$start" oflag=seek_bytes conv=notrunc,sparse status=none
	echo "  p$p at byte $start ($size bytes): written"
done

echo "== 5. checks"
read -r start size < <(part 5)
if cmp -n "$(stat -c %s "$verity")" "$verity" <(tail -c +$((start + 1)) "$out/$img-signed.img" | head -c "$(stat -c %s "$verity")"); then
	echo "  p5 == $(basename "$verity")"
else
	echo "  p5 differs from the verity image"; exit 1
fi
if [[ -n "$envf" && -r "$envf" ]]; then
	# shellcheck disable=SC1090
	. "$envf"
	veritysetup verify --data-block-size="$DATA_BLOCK_SIZE" --hash-offset="$DATA_SIZE" \
		"$verity" "$verity" "$ROOT_HASH" && echo "  root hash $ROOT_HASH verifies the rootfs image"
fi
"${RPI_EEPROM:-$HOME/rpi-secureboot/rpi-eeprom}/rpi-eeprom-digest" -k "$SIGNING_HOME/public.pem" \
	-i "$work/outer/boot.img" -v "$work/outer/boot.sig" && echo "  boot.sig verifies with the boot public key"

echo "== 6. RAUC bundle $version (development-signed, then release-signed)"
b="$work/bundle"; mkdir -p "$b"
cp --reflink=auto "$verity" "$b/rootfs-verity.img"
cp --reflink=auto "$out/boot-signed.vfat" "$b/boot-signed.vfat"
cat > "$b/manifest.raucm" <<EOF
[update]
compatible=device-platform-rpi5
version=$version
description=device-platform A/B update, signed boot.img

[bundle]
format=verity

[image.rootfs]
filename=rootfs-verity.img

[image.boot]
filename=boot-signed.vfat
EOF
rm -f "$work/dev.raucb"
oe-run-native rauc-native rauc bundle --cert="$RAUC_DEV_DIR/dev.cert.pem" \
	--key="$RAUC_DEV_DIR/dev.key.pem" "$b" "$work/dev.raucb" 2>&1 | grep -v 'Getting sysroot' | tail -n 1
"$here/sign-release-bundle.sh" "$work/dev.raucb" "$out/update-$version-signed.raucb" 2>&1 \
	| grep -v 'Getting sysroot' | tail -n 1
ls -la "$out"
