#!/bin/bash
# Raspberry Pi OS counterpart of run_on_pi.sh: boot one of the card's vendor
# kernels through tryboot, run the devbus RT matrix plus repeats on it, and
# copy the results back. Every test boot is one-shot, so the card returns to
# its default kernel on the next reboot no matter what.
#
#   run_on_pios.sh <stock|v8|rt> [ssh-host] [bin-dir]
#
#   stock  kernel_2712.img  6.18.39+rpt-rpi-2712    PREEMPT,    16K pages
#   v8     kernel8.img      6.18.39+rpt-rpi-v8      PREEMPT,     4K pages
#   rt     kernel8_rt.img   6.18.39+rpt-rpi-v8-rt   PREEMPT_RT,  4K pages
#
# The RT flavour only exists as a v8 build, so `v8` - not `stock` - is the
# honest non-RT half of the comparison: it differs from `rt` in CONFIG_PREEMPT_RT
# alone, while `stock` would also change the page size. `stock` is measured as
# "what this card actually ships with", not as the RT control.
#
# All three boot with cmdline_iso.txt (isolcpus=2,3 ... devbus_test=1 panic=10).
# config.txt is never modified.
set -euo pipefail
WHICH=$1
PI=${2:-PiOS}
BIN=${3:-$HOME/devbus-xbuild/out}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RESULTS="$REPO/results/devbus/pi5-raspios"
REMOTE=devbus

case "$WHICH" in
	stock) KERNEL_LINE=""; EXPECT="rpt-rpi-2712" ;;
	v8)    KERNEL_LINE="kernel=kernel8.img"; EXPECT="rpt-rpi-v8" ;;
	rt)    KERNEL_LINE="kernel=kernel8_rt.img"; EXPECT="rpt-rpi-v8-rt" ;;
	*) echo "usage: $0 <stock|v8|rt>"; exit 2 ;;
esac

ssh "$PI" "mkdir -p ~/$REMOTE"
scp -q "$REPO/userspace/devbus/bench/run_rt_matrix.sh" "$REPO/userspace/devbus/bench/run_rt_repeats.sh" "$PI:~/$REMOTE/"
ssh "$PI" "chmod +x ~/$REMOTE/*.sh"

# tryboot.txt = config.txt + our overrides. One-shot: any reboot ignores it.
ssh "$PI" "sudo sh -c 'cd /boot/firmware && { cat config.txt; printf \"\n[all]\ncmdline=cmdline_iso.txt\n$KERNEL_LINE\n\"; } > tryboot.txt && sync'"
ssh "$PI" 'sudo systemctl reboot --reboot-argument="0 tryboot"' >/dev/null 2>&1 || true
sleep 25
for _ in $(seq 1 60); do
	up=$(timeout 5 ssh -o BatchMode=yes -o ConnectTimeout=4 "$PI" 'cut -d. -f1 /proc/uptime' 2>/dev/null || true)
	[ -n "$up" ] && [ "$up" -lt 300 ] && break
	sleep 6
done
REL=$(ssh "$PI" uname -r)
echo "booted $REL"
case "$REL" in
	*"$EXPECT"*) ;;
	*) echo "expected a kernel matching '$EXPECT', got $REL - did it fail to boot? see /boot/firmware/devbus-boot-*.log"; exit 1 ;;
esac
# The command line is a request, not a confirmation (case 09). These vendor
# kernels are built without CONFIG_NO_HZ_FULL / CONFIG_RCU_NOCB_CPU, so
# nohz_full= and rcu_nocbs= are silently ignored here - isolcpus is not.
ssh "$PI" 'echo "isolated=$(cat /sys/devices/system/cpu/isolated) nohz_full=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo NOT-COMPILED-IN)"; sudo dmesg | grep -i "unknown kernel command line" || true'

ssh "$PI" "cd ~/$REMOTE && sudo sh run_rt_matrix.sh ${SECS:-60}"
ssh "$PI" "cd ~/$REMOTE && sudo sh run_rt_repeats.sh ${REPEATS:-2} ${SECS:-60}"

mkdir -p "$RESULTS"
scp -qr "$PI:~/$REMOTE/results/$REL" "$RESULTS/"
echo "results in $RESULTS/$REL"
