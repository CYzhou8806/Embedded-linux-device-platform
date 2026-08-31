#!/usr/bin/env bash
# 通用 CMake + OpenOCD 一键编译烧录脚本。
#
# 使用方法：把这个文件复制到某个具体 MCU 项目的根目录（跟 CMakePresets.json
# 同级，比如 v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/ 这一层），
# 然后在那个目录里执行:
#   bash build-flash.sh [Debug|Release]
#
# 前提条件（适用于用 CMake + CMakePresets.json 组织的项目；早期版本如果不是
# 用 CMake 构建的，这个脚本不适用，需要单独写）：
#   - 项目根目录下有 CMakePresets.json，里面有名为 Debug/Release 的 preset
#   - 调试器已经直通进这台机器（先跑一遍 tools/debug-connect.sh）
#   - 工具链已装好（先跑一遍 tools/setup-linux-mcu-toolchain.sh）
#
# 如果目标芯片或调试器型号不是 STM32F1 + CMSIS-DAP，改下面两个变量即可，
# 或者用环境变量覆盖，例如:
#   OPENOCD_TARGET_CFG=target/stm32f4x.cfg bash build-flash.sh
set -euo pipefail

BUILD_TYPE="${1:-Debug}"
OPENOCD_INTERFACE_CFG="${OPENOCD_INTERFACE_CFG:-interface/cmsis-dap.cfg}"
OPENOCD_TARGET_CFG="${OPENOCD_TARGET_CFG:-target/stm32f1x.cfg}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [[ ! -f CMakePresets.json ]]; then
  echo "当前目录没有 CMakePresets.json，这个脚本要放在具体 MCU 项目的根目录里跑。" >&2
  exit 1
fi

if [[ -f /etc/profile.d/embedded-toolchain.sh ]]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/embedded-toolchain.sh
fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1 || ! command -v openocd >/dev/null 2>&1; then
  echo "工具链没找到，请先跑一遍 tools/setup-linux-mcu-toolchain.sh" >&2
  exit 1
fi

echo "==> 清理旧的构建产物 (build/${BUILD_TYPE})"
rm -rf "build/${BUILD_TYPE}"

echo "==> CMake 配置 (${BUILD_TYPE})"
cmake --preset "${BUILD_TYPE}"

echo "==> 编译"
cmake --build --preset "${BUILD_TYPE}"

ELF="$(find "build/${BUILD_TYPE}" -maxdepth 1 -name '*.elf' | head -n1)"
if [[ -z "$ELF" ]]; then
  echo "在 build/${BUILD_TYPE}/ 下没找到 .elf 产物" >&2
  exit 1
fi

echo "==> 烧录 (OpenOCD, ${OPENOCD_INTERFACE_CFG} + ${OPENOCD_TARGET_CFG})"
openocd -f "${OPENOCD_INTERFACE_CFG}" -f "${OPENOCD_TARGET_CFG}" \
  -c "program ${ELF} verify reset exit"

echo ""
echo "烧录完成: ${ELF}"
