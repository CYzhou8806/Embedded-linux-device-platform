#!/usr/bin/env bash
# 一键配置 AlmaLinux/RHEL9 系 VM 的 MCU 开发工具链：
#   arm-none-eabi-gcc + OpenOCD (xPack) + CMake/Ninja
#   + usbip 客户端 (userspace + vhci-hcd 内核模块，从源码编译)
#   + CMSIS-DAP 调试器 udev 权限
# 用法: sudo bash setup-linux-mcu-toolchain.sh
set -euo pipefail

ARM_GCC_VER="15.2.1-1.1"
OPENOCD_VER="0.12.0-7"
ARM_GCC_URL="https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v${ARM_GCC_VER}/xpack-arm-none-eabi-gcc-${ARM_GCC_VER}-linux-x64.tar.gz"
OPENOCD_URL="https://github.com/xpack-dev-tools/openocd-xpack/releases/download/v${OPENOCD_VER}/xpack-openocd-${OPENOCD_VER}-linux-x64.tar.gz"

if [[ $EUID -ne 0 ]]; then
  echo "请用 sudo 运行: sudo bash $0" >&2
  exit 1
fi

echo "==> 1/6 安装基础依赖包"
dnf install -y epel-release
dnf config-manager --set-enabled crb
dnf install -y cmake ninja-build git tar gzip \
  autoconf automake libtool systemd-devel pkgconf-pkg-config \
  bison flex elfutils-libelf-devel openssl-devel zlib-devel libzstd-devel \
  "kernel-devel-$(uname -r)"

echo "==> 2/6 下载安装 xPack arm-none-eabi-gcc + OpenOCD"
mkdir -p /opt/xpack
TMPDIR=$(mktemp -d)
curl -L "$ARM_GCC_URL" -o "$TMPDIR/arm-gcc.tar.gz"
curl -L "$OPENOCD_URL" -o "$TMPDIR/openocd.tar.gz"
tar -xzf "$TMPDIR/arm-gcc.tar.gz" -C /opt/xpack
tar -xzf "$TMPDIR/openocd.tar.gz" -C /opt/xpack

cat > /etc/profile.d/embedded-toolchain.sh <<EOF
export PATH="/opt/xpack/xpack-arm-none-eabi-gcc-${ARM_GCC_VER}/bin:/opt/xpack/xpack-openocd-${OPENOCD_VER}/bin:\$PATH"
EOF
# shellcheck disable=SC1091
source /etc/profile.d/embedded-toolchain.sh

echo "==> 3/6 编译安装 usbip 用户态客户端 (usbip/usbipd)"
git clone --depth 1 --filter=blob:none --sparse https://github.com/torvalds/linux.git "$TMPDIR/linux-usbip-tools"
git -C "$TMPDIR/linux-usbip-tools" sparse-checkout set tools/usb/usbip
pushd "$TMPDIR/linux-usbip-tools/tools/usb/usbip" >/dev/null
./autogen.sh
./configure
make -j"$(nproc)"
make install
ldconfig
popd >/dev/null

echo "==> 4/6 编译 vhci-hcd / usbip-core 内核模块（RHEL9 内核未随包提供）"
git clone --depth 1 --branch v5.14 --filter=blob:none --sparse https://github.com/torvalds/linux.git "$TMPDIR/linux-usbip-drv"
git -C "$TMPDIR/linux-usbip-drv" sparse-checkout set drivers/usb/usbip
cat > "$TMPDIR/linux-usbip-drv/drivers/usb/usbip/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
obj-m += usbip-core.o
usbip-core-y := usbip_common.o usbip_event.o

obj-m += vhci-hcd.o
vhci-hcd-y := vhci_sysfs.o vhci_tx.o vhci_rx.o vhci_hcd.o
EOF
make -C "/lib/modules/$(uname -r)/build" M="$TMPDIR/linux-usbip-drv/drivers/usb/usbip" modules

MODDEST="/lib/modules/$(uname -r)/extra/usbip"
mkdir -p "$MODDEST"
cp "$TMPDIR/linux-usbip-drv/drivers/usb/usbip/usbip-core.ko" \
   "$TMPDIR/linux-usbip-drv/drivers/usb/usbip/vhci-hcd.ko" "$MODDEST/"
depmod -a
echo "vhci-hcd" > /etc/modules-load.d/usbip.conf
modprobe vhci-hcd

echo "==> 5/6 添加 CMSIS-DAP (Keil c251:f001) 调试器 udev 权限"
cat > /etc/udev/rules.d/99-cmsis-dap.rules <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="c251", ATTR{idProduct}=="f001", MODE="0666"
KERNEL=="hidraw*", ATTRS{idVendor}=="c251", ATTRS{idProduct}=="f001", MODE="0666"
EOF
udevadm control --reload-rules
udevadm trigger

echo "==> 6/6 清理临时文件"
rm -rf "$TMPDIR"

echo ""
echo "完成。重新登录 shell（或 source /etc/profile.d/embedded-toolchain.sh）后即可使用:"
echo "  arm-none-eabi-gcc --version"
echo "  openocd --version"
echo "  usbip version"
echo "  lsmod | grep vhci_hcd"
echo ""
echo "USB/IP 直通用法（Windows 端每次重启电脑要重新 usbipd bind, VM 端每次重启都要重新 attach):"
echo "  sudo usbip attach -r <Windows主机IP> -b <busid>   # busid 从 Windows 端 usbipd list 拿"
echo ""
echo "若目标机的 Windows vEthernet 网卡分类是 Public, usbipd 会出现静默连接失败,"
echo "需要在 Windows 管理员 PowerShell 里把它改成 Private:"
echo "  Set-NetConnectionProfile -InterfaceIndex <N> -NetworkCategory Private"
