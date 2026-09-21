#!/bin/bash
# Boot one kernel on the Pi through tryboot, run the devbus RT matrix plus
# repeats on it, and copy the results back. Every test boot is one-shot:
# the Pi returns to its stock kernel on the next reboot no matter what.
#
#   run_on_pi.sh <std|rt|stock> [ssh-host] [bin-dir]
#
# bin-dir holds the aarch64 devbus-bench, stress-ng, cyclictest and the
# run_rt_*.sh scripts (see README.md). Results land in
# results/devbus/pi5-yocto/<kernel release>/.
set -euo pipefail
WHICH=$1
PI=${2:-RaspberryPi5}
BIN=${3:-$HOME/devbus-xbuild/out}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RESULTS="$REPO/results/devbus/pi5-yocto"
cp "$REPO/userspace/devbus/bench/run_rt_matrix.sh" "$REPO/userspace/devbus/bench/run_rt_repeats.sh" "$BIN/"

if [ "$WHICH" = stock ]; then
	TRY="printf '\n[all]\ncmdline=cmdline_iso.txt\n'"
else
	TRY="printf '\n[all]\nos_prefix=$WHICH/\n'"
fi
ssh "$PI" "cd /boot && { cat config.txt; $TRY; } > tryboot.txt && sync"
ssh "$PI" 'systemctl reboot --reboot-argument="0 tryboot"' >/dev/null 2>&1 || true
sleep 20
for _ in $(seq 1 60); do
	up=$(timeout 5 ssh -o BatchMode=yes -o ConnectTimeout=4 "$PI" 'cut -d. -f1 /proc/uptime' 2>/dev/null || true)
	[ -n "$up" ] && [ "$up" -lt 300 ] && break
	sleep 6
done
REL=$(ssh "$PI" uname -r)
echo "booted $REL"
case "$WHICH:$REL" in
	std:*devbus-std*|rt:*devbus-rt*|stock:*v8-16k*) ;;
	*) echo "expected $WHICH kernel, got $REL - did the test kernel fail to boot? see /boot/devbus-boot-*.log"; exit 1 ;;
esac
# Isolation readback: the command line is a request, not a confirmation (case 09).
ssh "$PI" 'echo "isolated=$(cat /sys/devices/system/cpu/isolated) nohz_full=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo n/a)"; dmesg | grep -i "unknown kernel command line" || true'

ssh "$PI" 'systemctl stop device-service 2>/dev/null; mkdir -p /tmp/devbus'
tar cf - -C "$BIN" devbus-bench stress-ng cyclictest run_rt_matrix.sh run_rt_repeats.sh | ssh "$PI" 'tar xf - -C /tmp/devbus'
# let the boot-check service's t+90s log dump pass before measuring
ssh "$PI" 'cd /tmp/devbus; while [ "$(cut -d. -f1 /proc/uptime)" -lt 120 ]; do sleep 5; done
	sh run_rt_matrix.sh 60 > matrix.log 2>&1 && sh run_rt_repeats.sh 2 60 > repeats.log 2>&1
	dmesg > "results/$(uname -r)/dmesg.txt"'
mkdir -p "$RESULTS"
ssh "$PI" 'cd /tmp/devbus/results && tar cf - .' | tar xf - -C "$RESULTS"
echo "done: $RESULTS/$REL"
