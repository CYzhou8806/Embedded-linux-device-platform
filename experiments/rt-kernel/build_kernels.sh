#!/bin/bash
# Build two Raspberry Pi 5 kernels from the SAME source and config, differing
# only in the preemption model: bcm2712_defconfig (CONFIG_PREEMPT) and the
# same plus CONFIG_PREEMPT_RT. Runs the cross build in a container, no
# host toolchain needed.
#
#   bash build_kernels.sh [workdir]        (default: ~/devbus-xbuild)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$HOME/devbus-xbuild}
BRANCH=rpi-6.18.y
mkdir -p "$W"
podman build -q -t devbus-xbuild -f "$HERE/Containerfile" "$HERE"
[ -d "$W/linux" ] || git clone --depth 1 -b $BRANCH https://github.com/raspberrypi/linux.git "$W/linux"
podman run --rm -v "$W":/w:Z -w /w/linux devbus-xbuild bash -c '
set -e
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
for v in std rt; do
	O=/w/kbuild-$v
	mkdir -p $O
	make O=$O bcm2712_defconfig >/dev/null
	scripts/config --file $O/.config --set-str LOCALVERSION "-devbus-$v-iso" -d LOCALVERSION_AUTO -d NO_HZ_IDLE -e NO_HZ_FULL -e RCU_NOCB_CPU -e CONTEXT_TRACKING_USER
	if [ $v = rt ]; then
		scripts/config --file $O/.config -d PREEMPT -d PREEMPT_VOLUNTARY -d PREEMPT_NONE -d PREEMPT_LAZY -e PREEMPT_RT
	fi
	make O=$O olddefconfig >/dev/null
	grep -E "^CONFIG_PREEMPT(_RT)?=|^CONFIG_HZ=|^CONFIG_NO_HZ_FULL=|^CONFIG_RCU_NOCB_CPU=" $O/.config | tr "\n" " "; echo "<- $v"
	make O=$O -j"$(nproc)" Image modules dtbs > /w/kbuild-$v.log 2>&1
	make O=$O -s kernelrelease
done'
echo "next: stage_kernel.sh std|rt inside the container (see README.md)"
