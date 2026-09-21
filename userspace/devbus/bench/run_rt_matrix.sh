#!/bin/sh
# Runs the real-time comparison matrix for ONE booted kernel, on the target.
# Boot each kernel under test (see docs/devbus-experiments.md, "Pi 5:
# PREEMPT_RT"), run this once per boot, collect the results directories.
#
#   sh run_rt_matrix.sh [seconds_per_run]      (BusyBox sh compatible)
#
# Expects devbus-bench, cyclictest and stress-ng next to this script, and
# isolcpus=2,3 on the kernel command line (so cores 2-3 are free for the
# "tuned" runs and everything else - including the load - stays on 0-1).
set -eu

SECS=${1:-60}
HERE=$(cd "$(dirname "$0")" && pwd)
B="$HERE/devbus-bench"
REL=$(uname -r)
OUT="$HERE/results/$REL"
mkdir -p "$OUT"
COUNT=$((SECS * 1000))

GOV=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
OLD_GOV=$(cat $GOV 2>/dev/null || echo ondemand)

{
	echo "# kernel: $(uname -a)"
	echo "# cmdline: $(cat /proc/cmdline)"
	echo "# realtime: $(cat /sys/kernel/realtime 2>/dev/null || echo 0)"
	echo "# isolated: $(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
	echo "# governor(default): $OLD_GOV"
	echo "# date: $(date -Iseconds 2>/dev/null || date)"
	echo "# sched_rt_runtime_us: $(cat /proc/sys/kernel/sched_rt_runtime_us)"
} > "$OUT/host.txt"

LOAD_PID=""
start_load() {
	# CPU, memory-bandwidth, context-switch and timer pressure - the same
	# mix of stressors used for the V7 scheduling matrix.
	"$HERE/stress-ng" --cpu 2 --vm 1 --vm-bytes 128M --switch 1 --timer 1 --timeout $((SECS + 30))s \
		--quiet >/dev/null 2>&1 &
	LOAD_PID=$!
	sleep 3
}
stop_load() {
	[ -n "$LOAD_PID" ] && kill "$LOAD_PID" 2>/dev/null || true
	wait 2>/dev/null || true
	LOAD_PID=""
	sleep 2
}
# Write the policy, not the per-CPU alias: on the Pi 5 all four cores share
# policy0, and writing cpuN/cpufreq/scaling_governor fails with EIO.
set_gov() {
	for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do echo "$1" > "$g"; done
	echo "# governor -> $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor)" >> "$OUT/host.txt"
}

TUNED="--prio 80 --pub-cpu 2 --sub-cpu 3 --mlock 1 --dma-latency 0"

echo "== [$REL] cyclictest: the kernel's own wake-up latency (1 kHz timer, SCHED_FIFO 90, core 3)"
"$HERE/cyclictest" -m -p 90 -i 1000 -l "$COUNT" -a 3 -t 1 -q -h 3000 > "$OUT/cyclictest_idle.txt" 2>&1
start_load
"$HERE/cyclictest" -m -p 90 -i 1000 -l "$COUNT" -a 3 -t 1 -q -h 3000 > "$OUT/cyclictest_load.txt" 2>&1
stop_load

echo "== [$REL] devbus one-way latency, 64 B at 1 kHz, ${SECS}s per config"
{
	"$B" latency --header only | sed 's/^#latency,/#config,latency,/'
	printf 'default-idle,'; "$B" latency --size 64 --rate 1000 --count "$COUNT"
	start_load
	printf 'default-load,'; "$B" latency --size 64 --rate 1000 --count "$COUNT"
	set_gov performance
	printf 'tuned-futex-load,'; "$B" latency --size 64 --rate 1000 --count "$COUNT" $TUNED
	printf 'tuned-spin-load,'; "$B" latency --size 64 --rate 1000 --count "$COUNT" --wait spin $TUNED
	# A SCHED_FIFO task that never sleeps runs into RT throttling: by default
	# the kernel takes the CPU away from all RT tasks for 50 ms of every
	# second (sched_rt_runtime_us=950000 of 1000000). On an isolated core
	# that safety net has nothing to protect, so turn it off for this run.
	OLD_RT=$(cat /proc/sys/kernel/sched_rt_runtime_us)
	echo -1 > /proc/sys/kernel/sched_rt_runtime_us
	printf 'tuned-spin-load-nothrottle,'; "$B" latency --size 64 --rate 1000 --count "$COUNT" --wait spin $TUNED
	echo "$OLD_RT" > /proc/sys/kernel/sched_rt_runtime_us
	set_gov "$OLD_GOV"
	stop_load
} | tee "$OUT/latency_matrix.csv"

echo "== [$REL] transport vs payload size (idle, default config)"
{
	"$B" latency --header only
	for size in 64 65536 1048576 4194304; do
		rate=1000; count=5000
		if [ "$size" -ge 1048576 ]; then rate=100; count=500; fi
		for t in devbus-loan devbus-copy uds; do
			"$B" latency --transport "$t" --size "$size" --rate "$rate" --count "$count"
		done
	done
} | tee "$OUT/latency_size.csv"

echo "== [$REL] slow-subscriber isolation (tuned + load)"
{
	"$B" isolation --header only
	start_load
	for p in drop-oldest block; do
		"$B" isolation --slow-policy "$p" --rate 5000 --seconds 5 $TUNED
	done
	stop_load
} | tee "$OUT/isolation.csv"

echo "results in $OUT"
