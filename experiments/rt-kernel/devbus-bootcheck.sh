#!/bin/sh
# Safety net for kernel experiments: a test kernel that boots but has no
# network gets rebooted. Test kernels are only ever started via tryboot
# (one-shot), so that reboot lands back on the stock kernel.
#
# A boot counts as a test boot if either
#   - the kernel release contains "devbus" (self-built kernels, Yocto card), or
#   - the kernel command line contains devbus_test=1 (vendor kernels, where
#     the release string isn't ours to choose - Raspberry Pi OS card).
# Logs go to the boot partition (FAT, survives the reboot; on the Yocto
# image /var/log is a tmpfs, so it is the only log that would survive).
case "$(uname -r)" in
*devbus*) ;;
*) grep -q 'devbus_test=1' /proc/cmdline 2>/dev/null || exit 0 ;;
esac
BOOTDIR=/boot
[ -d /boot/firmware ] && BOOTDIR=/boot/firmware
LOG=$BOOTDIR/devbus-boot-$(uname -r).log
dump() { { echo "=== $1 $(date)"; ip addr 2>&1; lsmod 2>&1; dmesg 2>&1; journalctl -b --no-pager 2>&1; } > "$LOG"; sync; }
sleep 90
dump "t+90s"
sleep 90
if ! ping -c 3 -W 3 192.168.178.1 >/dev/null 2>&1; then
	dump "no network at t+180s, rebooting"
	systemctl reboot
fi
