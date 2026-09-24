#!/usr/bin/env bash
# dm-verity on the real rootfs image, without a board and without root:
# build the hash tree, verify, change one byte of a real binary, verify
# again. The on-target equivalent (kernel returns EIO for that block) is the
# same check done by dm-verity at read time.
#
#   verity-tamper-demo.sh <rootfs.ext[234]> [path-in-image] [workdir]
set -euo pipefail
img_src="$1"
target="${2:-/opt/device-service/device-service}"
work="${3:-$(mktemp -d)}"
debugfs=/usr/sbin/debugfs
mkdir -p "$work"; cd "$work"

cp "$img_src" rootfs.img
echo "== image: $(basename "$img_src") ($(stat -c %s rootfs.img) bytes)"

echo "== 1. build the hash tree"
veritysetup format --data-block-size=4096 --hash-block-size=4096 \
	rootfs.img rootfs.hashtree | tee format.txt
root_hash="$(awk '/^Root hash/{print $3}' format.txt)"
echo "hash tree: $(stat -c %s rootfs.hashtree) bytes for $(stat -c %s rootfs.img) bytes of data"

echo "== 2. verify the untouched image"
veritysetup verify rootfs.img rootfs.hashtree "$root_hash" && echo "verify: OK"

echo "== 3. change one byte inside $target"
fs_bs="$($debugfs -R stats rootfs.img 2>/dev/null | awk -F: '/^Block size/{gsub(/ /,"",$2); print $2}')"
first_block="$($debugfs -R "blocks $target" rootfs.img 2>/dev/null | awk '{print $2}')"
offset=$((first_block * fs_bs + 100))
echo "filesystem block $first_block (block size $fs_bs) -> byte offset $offset -> verity block $((offset / 4096))"
echo "before: $($debugfs -R "cat $target" rootfs.img 2>/dev/null | sha256sum | cut -c1-16)…"
cp rootfs.img rootfs-tampered.img
printf '\x90' | dd of=rootfs-tampered.img bs=1 seek="$offset" conv=notrunc status=none
echo "after:  $($debugfs -R "cat $target" rootfs-tampered.img 2>/dev/null | sha256sum | cut -c1-16)…  <- plain ext4 serves the modified binary"

echo "== 4. verify the tampered image against the original root hash"
if veritysetup verify rootfs-tampered.img rootfs.hashtree "$root_hash"; then
	echo "verify: OK  (UNEXPECTED)"; exit 1
else
	echo "verify: FAILED, as it must ($(( offset / 4096 * 4096 )) = start of the modified 4 KiB block)"
fi

echo "== 5. an attacker who also rebuilds the hash tree gets a different root hash"
veritysetup format --data-block-size=4096 --hash-block-size=4096 \
	rootfs-tampered.img attacker.hashtree | awk '/^Root hash/{print "attacker root hash: " $3}'
echo "original root hash: $root_hash"
echo "-> the attack only works if the root hash on the kernel command line can be changed too,"
echo "   which is why it has to live inside the signed boot.img (docs/security/secure-boot.md)"
