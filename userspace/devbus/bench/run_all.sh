#!/usr/bin/env bash
# Runs every devbus experiment and writes one CSV per experiment to
# results/devbus/<platform>/. Same script on the dev host and on the Pi.
#
#   bash bench/run_all.sh [path/to/devbus-bench] [platform]
#
# <platform> names the results directory and defaults to the hostname.
# The ones committed here are dev-host, pi5-yocto and pi5-raspios -- the
# platform, not the machine, is what the numbers are comparable across
# (see platforms/README.md).
#
# Takes a few minutes. For less noisy numbers on a real target, pin CPU
# frequency (performance governor) and keep the machine otherwise idle.
set -euo pipefail

BENCH=${1:-./build/devbus-bench}
HERE=$(cd "$(dirname "$0")" && pwd)
PLATFORM=${2:-$(hostname)}
OUT="$HERE/../../../results/devbus/$PLATFORM"
mkdir -p "$OUT"

{
	echo "# host: $(hostname)  kernel: $(uname -r)  cpus: $(nproc)  date: $(date -Iseconds)"
	grep -m1 'model name' /proc/cpuinfo 2>/dev/null || true
	cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null | sed 's/^/# governor: /' || true
} > "$OUT/host.txt"

echo "== latency: transport x payload size (futex wait, Block policy, no drops)"
{
	"$BENCH" latency --header only
	for size in 64 4096 65536 1048576 4194304; do
		rate=1000; count=3000
		if [ "$size" -ge 1048576 ]; then rate=100; count=500; fi
		for t in devbus-loan devbus-copy uds; do
			"$BENCH" latency --transport "$t" --size "$size" --rate "$rate" --count "$count"
		done
	done
} | tee "$OUT/latency.csv"

echo "== wait strategy (64 B, devbus-loan)"
{
	"$BENCH" latency --header only
	for w in spin yield futex; do
		for rate in 1000 10000; do
			"$BENCH" latency --transport devbus-loan --size 64 --wait "$w" --rate "$rate" --count $((rate * 3))
		done
	done
} | tee "$OUT/wait.csv"

echo "== in-process queue: device-service RingBuffer (mutex+condvar) vs devbus ring"
{
	"$BENCH" queue --header only
	for impl in mutex-cv devbus-futex devbus-spin; do
		"$BENCH" queue --impl "$impl" --count 2000000 --rate 0   # throughput
		"$BENCH" queue --impl "$impl" --count 30000 --rate 10000 # latency at a steady 10 kHz
	done
} | tee "$OUT/queue.csv"

echo "== slow-subscriber isolation (publisher 5 kHz, slow subscriber needs 1 ms/sample)"
{
	"$BENCH" isolation --header only
	for p in drop-oldest drop-newest block; do
		"$BENCH" isolation --slow-policy "$p" --rate 5000 --seconds 3
	done
} | tee "$OUT/isolation.csv"

echo "results in $OUT"
