#!/bin/bash
# Stage one custom kernel for the Pi 5 Yocto card, run inside the build container.
#   stage_kernel.sh <std|rt>
# Produces /w/stage-<v>/boot/<v>/...  (os_prefix directory for the boot partition)
#      and /w/stage-<v>/lib/modules/<release>/  (only the modules the Pi actually loads)
set -euo pipefail
V=$1
K=/w/kbuild-$V
S=/w/stage-$V
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
REL=$(make -s -C /w/linux O=$K kernelrelease)
rm -rf "$S" /w/modfull-$V
mkdir -p "$S/boot/$V/overlays" "$S/lib/modules"

# out-of-tree acquisition driver, built against this exact kernel
rm -rf /w/acq-$V && cp -r /src/driver/custom-acq /w/acq-$V
make -s -C /w/linux O=$K M=/w/acq-$V modules

make -s -C /w/linux O=$K INSTALL_MOD_PATH=/w/modfull-$V modules_install
FULL=/w/modfull-$V/lib/modules/$REL
mkdir -p "$FULL/extra" && cp /w/acq-$V/custom_acq.ko "$FULL/extra/"
depmod -b /w/modfull-$V "$REL"

# The set to keep: what the stock kernel has loaded on this Pi, plus our driver.
python3 - "$FULL" "$S/lib/modules/$REL" /w/loaded-modules.txt <<'PY'
import os, shutil, sys
full, dst, wanted_file = sys.argv[1:4]
norm = lambda n: n.replace('-', '_')
import re
modname = lambda p: norm(re.sub(r'\.ko(\.xz|\.zst|\.gz)?$', '', os.path.basename(p)))
dep = {}
for line in open(os.path.join(full, 'modules.dep')):
    path, _, deps = line.strip().partition(':')
    dep[modname(path)] = (path, deps.split())
wanted = {norm(w.strip()) for w in open(wanted_file) if w.strip()} | {'custom_acq', 'brcmfmac_cyw', 'brcmfmac_bca', 'brcmfmac_wcc'}
keep, todo, missing = set(), list(wanted), []
while todo:
    n = todo.pop()
    if n in keep:
        continue
    if n not in dep:
        missing.append(n)  # built in to this kernel, or renamed
        continue
    keep.add(n)
    todo += [modname(d) for d in dep[n][1]]
for n in keep:
    rel = dep[n][0]
    os.makedirs(os.path.join(dst, os.path.dirname(rel)), exist_ok=True)
    shutil.copy2(os.path.join(full, rel), os.path.join(dst, rel))
for f in ('modules.order', 'modules.builtin', 'modules.builtin.modinfo'):
    if os.path.exists(os.path.join(full, f)):
        shutil.copy2(os.path.join(full, f), os.path.join(dst, f))
print(f'kept {len(keep)} modules; not modules here (built-in or absent): {sorted(missing)}')
PY
depmod -b "$S" "$REL"

cp "$K/arch/arm64/boot/Image" "$S/boot/$V/kernel_2712.img"
cp "$K/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dtb" "$S/boot/$V/"
for o in overlay_map.dtb vc4-kms-v3d.dtbo vc4-kms-v3d-pi5.dtbo; do
	cp "$K/arch/arm64/boot/dts/overlays/$o" "$S/boot/$V/overlays/"
done
cp /w/custom-acq.dtbo "$S/boot/$V/overlays/"
echo "$REL"
du -sh "$S/boot/$V" "$S/lib/modules/$REL"
