#!/usr/bin/env bash
# 纯软件触发的 MCU 硬件复位——通过调试器（SWD）直接给 MCU 发复位信号，
# 不经过 SPI，所以不依赖 SPI 协议/MCU 固件当前状态是否正常。
#
# 用途：每次 Pi 侧硬件测试结束后（不管测试成功还是失败）跑一下这个脚本，
# 保证 MCU 一定回到刚上电的干净状态——不用再摸板子上的物理复位键。
#
# 前提条件：
#   - 调试器已经直通进这台机器（先跑一遍 tools/debug-connect.sh）
#   - 工具链已装好（先跑一遍 tools/setup-linux-mcu-toolchain.sh）
#
# 如果目标芯片或调试器型号不是 STM32F1 + CMSIS-DAP，跟 build-flash.sh 一样
# 可以用环境变量覆盖:
#   OPENOCD_TARGET_CFG=target/stm32f4x.cfg bash mcu-reset.sh
set -euo pipefail

OPENOCD_INTERFACE_CFG="${OPENOCD_INTERFACE_CFG:-interface/cmsis-dap.cfg}"
OPENOCD_TARGET_CFG="${OPENOCD_TARGET_CFG:-target/stm32f1x.cfg}"

if [[ -f /etc/profile.d/embedded-toolchain.sh ]]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/embedded-toolchain.sh
fi
if ! command -v openocd >/dev/null 2>&1; then
  echo "openocd 没找到，请先跑一遍 tools/setup-linux-mcu-toolchain.sh" >&2
  exit 1
fi

echo "==> 复位 MCU (OpenOCD, ${OPENOCD_INTERFACE_CFG} + ${OPENOCD_TARGET_CFG})"
openocd -f "${OPENOCD_INTERFACE_CFG}" -f "${OPENOCD_TARGET_CFG}" \
  -c "init" -c "reset run" -c "exit"

echo "复位完成。"
