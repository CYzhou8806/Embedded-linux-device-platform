#!/usr/bin/env bash
# Ubuntu 24.04 开发主机一次性环境（安全计划 B0 用）：
#   1. Yocto Scarthgap 主机依赖（来源：Yocto 5.0 官方 Ubuntu 依赖列表）
#   2. 本项目自己的依赖（device-service / devbus 本地编译 + 测试 + 画图）
#   3. MCU 工具链（apt 版 arm-none-eabi-gcc + OpenOCD；裸机不需要 usbip）
#   4. 后续安全工作块要用的工具（SoftHSM/PKCS#11、LUKS、OP-TEE on QEMU 构建依赖）
#   5. /opt/yocto 目录（交给当前用户）
#   6. 放开 AppArmor 对非特权 user namespace 的限制（bitbake 需要）
#
# 旧的 setup-linux-mcu-toolchain.sh 是 AlmaLinux/dnf 版本，这台机器已经换成
# Ubuntu 24.04 裸机，那个脚本不再适用。
#
# 用法: sudo bash tools/setup-ubuntu-host.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "请用 sudo 运行: sudo bash $0" >&2
  exit 1
fi
TARGET_USER="${SUDO_USER:?请通过 sudo 运行，而不是直接用 root 登录}"

apt-get update

echo "==> 1/6 Yocto Scarthgap 主机依赖"
apt-get install -y build-essential chrpath cpio debianutils diffstat file \
  gawk gcc git iputils-ping libacl1 lz4 locales python3 python3-git \
  python3-jinja2 python3-pexpect python3-pip python3-subunit socat texinfo \
  unzip wget xz-utils zstd

echo "==> 2/6 本项目依赖"
apt-get install -y cmake ninja-build pkg-config libspdlog-dev \
  nlohmann-json3-dev libgtest-dev libsystemd-dev python3-matplotlib \
  python3-pytest

echo "==> 3/6 MCU 工具链"
apt-get install -y gcc-arm-none-eabi libnewlib-arm-none-eabi openocd

echo "==> 4/6 安全工作块工具（B3/B5/B7）"
apt-get install -y softhsm2 opensc libengine-pkcs11-openssl cryptsetup \
  device-tree-compiler u-boot-tools
# OP-TEE 官方构建依赖（optee.readthedocs.io "Prerequisites"），逐个装：
# 列表偶尔有包在新版 Ubuntu 里改名，单个失败不应拖垮整个脚本。
OPTEE_PKGS=(acpica-tools autoconf automake bc bison ccache cscope curl
  e2tools expect flex gdisk libattr1-dev libcap-ng-dev libfdt-dev
  libglib2.0-dev libgmp3-dev libhidapi-dev libmpc-dev libncurses-dev
  libpixman-1-dev libslirp-dev libssl-dev libtool libusb-1.0-0-dev make
  mtools netcat-openbsd python3-cryptography python3-pyelftools
  python3-serial python-is-python3 repo rsync swig uuid-dev xterm
  zlib1g-dev)
missing=()
for p in "${OPTEE_PKGS[@]}"; do
  apt-get install -y "$p" >/dev/null 2>&1 || missing+=("$p")
done
[[ ${#missing[@]} -gt 0 ]] && echo "!! 以下 OP-TEE 依赖没装上（名字可能变了）: ${missing[*]}"

echo "==> 5/6 locale + /opt/yocto"
locale-gen en_US.UTF-8 >/dev/null
install -d -o "$TARGET_USER" -g "$TARGET_USER" /opt/yocto
install -d -m 700 -o "$TARGET_USER" -g "$TARGET_USER" /opt/yocto/local-config

echo "==> 6/6 AppArmor: 允许非特权 user namespace"
# Ubuntu 23.10+ 默认禁止非特权进程建 user namespace，bitbake 用它给每个任务
# 隔离网络（do_fetch 以外的任务不许联网），不放开会直接报
# "User namespaces are not usable by BitBake, possibly due to AppArmor"。
# 这是对主机的放宽：只在开发机上做，取舍写在 B0 的记录里。
echo 'kernel.apparmor_restrict_unprivileged_userns = 0' \
  > /etc/sysctl.d/60-yocto-userns.conf
sysctl -p /etc/sysctl.d/60-yocto-userns.conf

echo "完成。"
