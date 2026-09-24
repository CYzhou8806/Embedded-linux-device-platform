#!/usr/bin/env bash
# Pack a directory into a Raspberry Pi boot.img (FAT ramdisk) and sign it with
# the boot key in the HSM, producing boot.img + boot.sig.
#
#   make-signed-boot-img.sh [--tryboot] <bootfs-dir> <out-dir>
#
# --tryboot: for a one-shot test through the firmware's tryboot (with
# "boot_ramdisk=1" in tryboot.txt on the boot partition). Both of these were
# found on the board, not in the documentation we had:
#   - in tryboot mode the bootloader loads tryboot.img / tryboot.sig, not
#     boot.img ("Error 6 loading tryboot.img" on the HDMI diagnostics)
#   - inside the ramdisk, the second configuration pass also reads
#     tryboot.txt; without it no dtoverlay is applied - the system boots,
#     but the SPI device (and vc4-kms) silently disappear
#
# Raspberry Pi's rpi-make-boot-image does the same with a loop mount and
# needs root; this uses mkfs.vfat -C and mtools instead, so it runs as a
# normal user. The signature is made through the HSM wrapper - the key never
# leaves the token (docs/security/secure-boot.md).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/hsm-env.sh"
tryboot=0
[[ "${1:-}" == "--tryboot" ]] && { tryboot=1; shift; }
src="$(realpath "$1")"; out="$(realpath -m "$2")"
: "${RPI_EEPROM:=$HOME/rpi-secureboot/rpi-eeprom}"
mkdir -p "$out"
name=boot; (( tryboot )) && name=tryboot
img="$out/$name.img"; sig="$out/$name.sig"
rm -f "$img" "$sig"

# FAT size: contents + 10 %, in KiB, at least 8 MiB.
need_kib=$(( $(du -sk --apparent-size "$src" | cut -f1) * 110 / 100 + 1024 ))
(( need_kib < 8192 )) && need_kib=8192
mkfs.vfat -C -n BOOTIMG "$img" "$need_kib" >/dev/null
mcopy -s -i "$img" "$src"/* ::
if (( tryboot )) && [[ ! -e "$src/tryboot.txt" ]]; then
	mcopy -i "$img" "$src/config.txt" ::tryboot.txt
fi
echo "$name.img: $(stat -c %s "$img") bytes"
mdir -/ -b -i "$img" :: | sed "s|^::/|  |"

PATH="$here:$PATH" "$RPI_EEPROM/rpi-eeprom-digest" -H pkcs11-hsm-wrapper -i "$img" -o "$sig"
"$RPI_EEPROM/rpi-eeprom-digest" -k "$SIGNING_HOME/public.pem" -i "$img" -v "$sig"
