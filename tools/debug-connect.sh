#!/usr/bin/env bash
# 日常一键连接调试器：每次想调试就跑这个。
# 会自己检查/修复常见问题（内核模块没加载、之前挂过一次没断开等），
# 然后把 Windows 上通过 usbipd 共享的调试器直通进这台 VM。
#
# 用法: sudo bash debug-connect.sh [Windows主机IP] [busid]
# 不传参数则用下面的默认值。
set -euo pipefail

WIN_IP="${1:-172.29.191.1}"
# 这个默认值只是"上次见过的样子"，不是固定不变的。BUSID 是 Windows 给每个
# USB 物理端口编的号，跟拔插顺序/端口有关——同一根线插在同一个口，正常不会
# 变；但重新插拔过、换了个 USB 口，或者接了新设备占了序号，BUSID 就可能变
# （实测出现过 2-7 变成 2-8 的情况，跟 VM/Windows 重不重启无关）。如果下面
# attach 报 "Device not found"，先去 Windows 跑 `usbipd list`，找 VID:PID
# = c251:f001 那一行现在的 BUSID，传进来覆盖默认值：
#   sudo bash debug-connect.sh 172.29.191.1 <新BUSID>
BUSID="${2:-2-7}"

if [[ $EUID -ne 0 ]]; then
  echo "请用 sudo 运行: sudo bash $0" >&2
  exit 1
fi

export PATH="/usr/local/sbin:/usr/local/bin:$PATH"

echo "==> 检查 usbip 客户端是否已安装"
if ! command -v usbip >/dev/null 2>&1; then
  echo "没找到 usbip 命令，请先跑一次 setup-linux-mcu-toolchain.sh。" >&2
  exit 1
fi

echo "==> 检查 vhci-hcd 内核模块"
if ! lsmod | grep -q '^vhci_hcd'; then
  echo "    未加载，尝试加载..."
  if ! modprobe vhci-hcd 2>/dev/null; then
    echo "modprobe vhci-hcd 失败，说明模块没装好，请重新跑 setup-linux-mcu-toolchain.sh。" >&2
    exit 1
  fi
fi
echo "    OK"

echo "==> 检查是否已经连接过（避免重复 attach 报错）"
if usbip port | grep -q "$BUSID"; then
  echo "    已经连接着，直接显示当前状态："
  usbip port
  lsusb
  exit 0
fi

echo "==> 检查能否连上 Windows 端 usbipd (${WIN_IP}:3240)"
if ! timeout 3 bash -c "echo > /dev/tcp/${WIN_IP}/3240" 2>/dev/null; then
  cat >&2 <<EOF
连不上 ${WIN_IP}:3240，可能原因（去 Windows 上检查）：
  1. usbipd 服务没启动: Get-Service usbipd
  2. 设备没有 bind: usbipd list / usbipd bind --busid <BUSID>
  3. 虚拟网卡还是 Public 分类，需要改成 Private（去跑一遍 setup-windows.ps1 会自动修）
  4. Windows 主机 IP 变了，不是 ${WIN_IP} 了（跑 ipconfig 确认）
EOF
  exit 1
fi
echo "    OK"

echo "==> attach 设备"
if ! usbip attach -r "$WIN_IP" -b "$BUSID"; then
  cat >&2 <<EOF

attach 失败。如果报错是 "Device not found"，大概率是 BUSID="$BUSID" 这个
编号跟 Windows 现在的实际编号对不上了（BUSID 会因为重新插拔/换端口而变，
不需要重启电脑就可能发生）。去 Windows 跑:
  usbipd list
找 VID:PID = c251:f001 那一行现在的 BUSID 和 STATE：
  - STATE 是 Not shared: 先 usbipd bind --busid <那个BUSID>
  - STATE 已经是 Shared: 直接拿这个 BUSID 重跑本脚本:
      sudo bash $0 ${WIN_IP} <那个BUSID>
EOF
  exit 1
fi
sleep 1

echo "==> 当前状态"
usbip port
lsusb

echo ""
echo "完成。现在可以在 VS Code 里跑 Debug (OpenOCD + CMSIS-DAP) 了。"
