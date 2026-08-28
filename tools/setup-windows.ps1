# setup-windows.ps1
# Windows 主机一次性配置：把 usbipd-win 装好，让调试器可以共享给 Linux VM。
# 重复运行是安全的（装系统后跑一次；以后如果连接又出问题，也可以重新跑一遍当修复用）。
# 用法：以【管理员】身份打开 PowerShell，执行:
#   powershell -ExecutionPolicy Bypass -File setup-windows.ps1

$ErrorActionPreference = "Stop"

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Write-Host "请用管理员身份重新运行 PowerShell 再执行本脚本。" -ForegroundColor Red
    exit 1
}

Write-Host "==> 1/4 安装 usbipd-win（已装则跳过）"
if (-not (Get-Command usbipd -ErrorAction SilentlyContinue)) {
    winget install --id dorssel.usbipd -e --accept-source-agreements --accept-package-agreements
} else {
    Write-Host "usbipd 已安装，跳过。"
}

Write-Host "==> 2/4 确保 usbipd 服务在运行"
Start-Service usbipd -ErrorAction SilentlyContinue
Set-Service usbipd -StartupType Automatic

Write-Host "==> 3/4 开放防火墙 TCP 3240（重复执行不会重复添加）"
if (-not (Get-NetFirewallRule -DisplayName "usbipd" -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName "usbipd" -Direction Inbound -Protocol TCP -LocalPort 3240 -Action Allow | Out-Null
} else {
    Write-Host "防火墙规则已存在，跳过。"
}

Write-Host "==> 4/4 把面向 Hyper-V VM 的虚拟网卡网络分类改成 Private"
Write-Host "    （usbipd-win 对 Public 分类的连接有已知的静默拦截问题，必须是 Private/Domain）"
$fixed = @()
Get-NetConnectionProfile | Where-Object {
    $_.InterfaceAlias -like "vEthernet*" -and $_.NetworkCategory -eq "Public"
} | ForEach-Object {
    Set-NetConnectionProfile -InterfaceIndex $_.InterfaceIndex -NetworkCategory Private
    $fixed += $_.InterfaceAlias
}
if ($fixed.Count -gt 0) {
    Write-Host "    已改为 Private: $($fixed -join ', ')"
} else {
    Write-Host "    没有发现需要修改的 vEthernet 网卡（可能已经是 Private，或者还没创建虚拟交换机）。"
}

Write-Host ""
Write-Host "基础配置完成。接下来是每次插拔调试器都要做的手动步骤（无法脚本化，因为要人工确认设备）："
Write-Host "  1. 插入调试器"
Write-Host "  2. usbipd list                     # 找到调试器那一行的 BUSID"
Write-Host "  3. usbipd bind --busid <BUSID>      # 绑定一次即可，重启电脑一般不用重新 bind"
Write-Host ""
Write-Host "绑定之后，去 VM 里跑 debug-connect.sh 就能连上了。"
