#!/usr/bin/env bash
# Host-side build only (no flashing step, unlike tools/build-flash.sh which
# is for the MCU firmware) — configure + build via the same CMakePresets.json
# convention used elsewhere in this repo.
#
# Usage: bash build.sh [Debug|Release]
set -euo pipefail

BUILD_TYPE="${1:-Debug}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "==> CMake 配置 (${BUILD_TYPE})"
cmake --preset "${BUILD_TYPE}"

echo "==> 编译"
cmake --build --preset "${BUILD_TYPE}"

echo ""
echo "构建完成: build/${BUILD_TYPE}/device-service"
