#!/bin/sh
# Repeats the three measurements the kernel comparison hinges on, so the
# conclusion doesn't rest on one run per kernel (Plan.md V7's lesson).
#
#   sh run_rt_repeats.sh [repeats] [seconds]     (BusyBox sh compatible)
#
# Appends to results/<release>/repeats.csv and cyclictest_load_rN.txt.
set -eu
N=${1:-2}
SECS=${2:-60}
HERE=$(cd "$(dirname "$0")" && pwd)
B="$HERE/devbus-bench"
OUT="$HERE/results/$(uname -r)"
mkdir -p "$OUT"
COUNT=$((SECS * 1000))
TUNED="--prio 80 --pub-cpu 2 --sub-cpu 3 --mlock 1 --dma-latency 0"
POL=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
OLD_GOV=$(cat $POL)

i=1
while [ "$i" -le "$N" ]; do
	"$HERE/stress-ng" --cpu 2 --vm 1 --vm-bytes 128M --switch 1 --timer 1 --timeout $((3 * SECS + 60))s \
		--quiet >/dev/null 2>&1 &
	LOAD=$!
	sleep 3
	"$HERE/cyclictest" -m -p 90 -i 1000 -l "$COUNT" -a 3 -t 1 -q -h 3000 > "$OUT/cyclictest_load_r$i.txt" 2>&1
	{
		printf 'r%s,default-load,' "$i"; "$B" latency --size 64 --rate 1000 --count "$COUNT"
		echo performance > $POL
		printf 'r%s,tuned-futex-load,' "$i"; "$B" latency --size 64 --rate 1000 --count "$COUNT" $TUNED
		echo "$OLD_GOV" > $POL
	} | tee -a "$OUT/repeats.csv"
	kill $LOAD 2>/dev/null || true
	wait 2>/dev/null || true
	sleep 2
	i=$((i + 1))
done
