# tools/

嵌入式开发环境相关的通用脚本，跟具体某个 MCU 项目版本无关。目前项目
里只有 v1.3（`v1-spi-slave-handshake/v1.3/MCU_v1-MCU-device-control/`）
是用 CMake 组织的；v1/v2 不是，所以这里的脚本目前只服务于 v1.3 及以后
用 CMake 组织的版本。

开发环境是：Windows 主机 + Hyper-V 里的 AlmaLinux VM，调试器（ST-Link /
CMSIS-DAP 等）插在 Windows 上，通过 usbipd-win + USB/IP 直通进 VM，VM 里
跑 arm-none-eabi-gcc + OpenOCD 编译烧录调试。

## 装系统后跑一次（配置类）

- **`setup-windows.ps1`**：Windows 主机执行（管理员 PowerShell）。装
  usbipd-win、开放防火墙 TCP 3240、把 Hyper-V 虚拟网卡的网络分类改成
  Private（usbipd-win 对 Public 分类的连接有已知的静默拦截问题）。重复
  执行是安全的，出问题了也可以当修复脚本再跑一遍。

  ```powershell
  powershell -ExecutionPolicy Bypass -File setup-windows.ps1
  ```

- **`setup-linux-mcu-toolchain.sh`**：Linux VM 执行（`sudo bash`）。装
  arm-none-eabi-gcc / OpenOCD（xPack 预编译版）/ cmake / ninja；AlmaLinux
  官方仓库不提供 usbip 客户端和 vhci-hcd 内核模块，所以从内核源码现场
  编译；再加一条 udev 规则给调试器（默认按 CMSIS-DAP c251:f001 配置，
  换设备要改 VID:PID）。

  ```bash
  sudo bash setup-linux-mcu-toolchain.sh
  ```

## 日常用（干活类）

- **`debug-connect.sh`**：Linux VM 执行（`sudo bash`）。调试前先跑这个，
  自检（内核模块、usbip 客户端）+ 自愈 + 把调试器直通进 VM。已经连接的
  话直接跳过，不会重复 attach 报错。

  ```bash
  sudo bash debug-connect.sh                    # 用脚本内默认的 IP/busid
  sudo bash debug-connect.sh 172.29.191.1 2-7    # 或手动指定
  ```

  **BUSID 会变**：脚本里写死的默认 BUSID 只是"上次见过的样子"，不是固定
  值。它是 Windows 给 USB 物理端口编的号，重新插拔调试器或者换个 USB 口
  （不需要重启电脑）就可能变（实测出现过 `2-7` 变成 `2-8`）。如果报错是
  `Device not found`，去 Windows 跑 `usbipd list` 查 `c251:f001` 那一行
  现在的 BUSID，传给脚本第二个参数覆盖默认值；脚本自己也会在这种报错时
  打印同样的排查步骤。

  ```powershell
  usbipd list   # 在 Windows 上查当前 BUSID / STATE
  ```

- **`build-flash.sh`**：编译烧录模板脚本，**不是直接在这里跑的**。用
  法是复制到某个具体 MCU 项目的根目录（跟 `CMakePresets.json` 同级），
  在那个目录里执行:

  ```bash
  cp tools/build-flash.sh v1-spi-slave-handshake/v1.4/某新项目/
  cd v1-spi-slave-handshake/v1.4/某新项目/
  bash build-flash.sh [Debug|Release]
  ```

  会清理旧的 `build/<配置>` 目录、重新 CMake 配置+编译、然后用 OpenOCD
  烧录。默认按 CMSIS-DAP + STM32F1 配置，换调试器/芯片型号用环境变量
  覆盖，不用改脚本本身：

  ```bash
  OPENOCD_INTERFACE_CFG=interface/stlink.cfg OPENOCD_TARGET_CFG=target/stm32f4x.cfg \
    bash build-flash.sh Release
  ```

  前提是项目本身用 CMake + CMakePresets.json 组织（preset 名要叫
  `Debug`/`Release`）。v1/v2 不满足这个前提，用不了。

## 日常使用顺序

```
sudo bash tools/debug-connect.sh
cd 具体MCU项目目录
bash build-flash.sh
```

需要单步调试的话再去 VS Code 里跑对应项目 `.vscode/launch.json` 里配
好的 Debug 配置。
