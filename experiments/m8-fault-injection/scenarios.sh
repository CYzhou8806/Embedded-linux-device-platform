#!/bin/sh
# Plan.md V2/M8 fault-injection matrix. Runs ON THE BOARD (BusyBox sh, no
# trace-cmd: tracefs is driven directly), as root, with device-service
# running. One directory per scenario under $OUT:
#
#   trace.txt     the ftrace buffer: custom_acq tracepoints, the inject/
#                 remove markers, and device-service's own state
#                 transitions (trace_marker, where it is allowed to write)
#   status-*.json device-ctl status before and after
#   evidence/     any fault-*.json the supervisor wrote meanwhile
#
# analyze.py (on the host) turns each trace into "which layer went wrong
# first". Every knob is put back afterwards, also on Ctrl-C.
#
#   sh scenarios.sh [OUT] [scenario...]    default: all, in the order below
set -u
OUT=${1:-/tmp/m8}; shift 2>/dev/null || true
T=/sys/kernel/tracing
P=/sys/module/custom_acq/parameters
S=/sys/bus/spi/devices/spi0.0
CTL=/opt/device-service/device-ctl
EVIDENCE=/var/lib/device-service
SCENARIOS=${*:-"baseline drain_delay drop spi_error stall slow_consumer overload clock_jump"}

[ -d "$T/events/custom_acq" ] || { echo "no custom_acq tracepoints - driver too old?" >&2; exit 1; }
RATE0=$(cat $S/sample_rate)

restore() {
	for k in fault_drain_delay_us fault_drop_every fault_spi_error_every fault_stall_ms; do
		echo 0 > $P/$k
	done
	echo "$RATE0" > $S/sample_rate 2>/dev/null
	pid=$(pidof device-service) && kill -CONT $pid 2>/dev/null
	echo 0 > $T/tracing_on
}
trap 'restore; exit 1' INT TERM

trace_start() {
	echo 0 > $T/tracing_on
	echo > $T/trace
	echo 8192 > $T/buffer_size_kb
	echo mono > $T/trace_clock        # same clock as irq_ts_ns and steady_clock
	# custom_acq events and markers only. sched_switch and irq_handler_*
	# were on in the first board run (2026-10-02): ~430 000 events per
	# scenario overwrote half of the 8 MB ring, including the inject
	# marker the analysis is anchored on - 6 of 7 scenarios unusable.
	echo 1 > $T/events/custom_acq/enable
	echo 1 > $T/tracing_on
}

trace_stop() {
	echo 0 > $T/tracing_on
	cat $T/trace > "$1/trace.txt"
	echo 0 > $T/events/enable
}

# $1 name, $2 inject command, $3 remove command, $4 seconds under fault
run() {
	d=$OUT/$1; mkdir -p "$d/evidence"
	echo "== $1"
	ls $EVIDENCE/fault-*.json 2>/dev/null > "$d/.evidence-before"
	$CTL status > "$d/status-before.json" 2>&1
	trace_start
	sleep 1                               # clean lead-in for the analyzer's baseline
	echo "m8: inject $1" > $T/trace_marker
	sh -c "$2"
	sleep "$4"
	echo "m8: remove $1" > $T/trace_marker
	sh -c "$3"
	sleep 3                               # let the supervisor finish reacting
	trace_stop "$d"
	$CTL status > "$d/status-after.json" 2>&1
	for f in $(ls $EVIDENCE/fault-*.json 2>/dev/null); do
		grep -qx "$f" "$d/.evidence-before" || cp "$f" "$d/evidence/"
	done
	# A scenario that latched a Fault would poison the next one.
	grep -q '"state":"Fault"' "$d/status-after.json" && $CTL reset >/dev/null && sleep 3
}

for s in $SCENARIOS; do
	case $s in
	baseline)      run baseline      'true' 'true' 2 ;;
	drain_delay)   run drain_delay   "echo 400 > $P/fault_drain_delay_us" "echo 0 > $P/fault_drain_delay_us" 3 ;;
	drop)          run drop          "echo 50 > $P/fault_drop_every" "echo 0 > $P/fault_drop_every" 3 ;;
	spi_error)     run spi_error     "echo 200 > $P/fault_spi_error_every" "echo 0 > $P/fault_spi_error_every" 3 ;;
	# Longer than device-service's liveness_timeout_ms (3000): the watchdog
	# must fire, the supervisor must recover.
	stall)         run stall         "echo 4500 > $P/fault_stall_ms" 'true' 6 ;;
	# The reader stops; the kfifo (128 samples) fills in ~130 ms at 1 kHz.
	slow_consumer) run slow_consumer 'kill -STOP $(pidof device-service)' 'kill -CONT $(pidof device-service)' 1 ;;
	# Past the M0 cliff (~1680 Hz at inter_frame_us=50).
	overload)      run overload      "echo 3000 > $S/sample_rate" "echo $RATE0 > $S/sample_rate" 3 ;;
	# Wall clock jumps an hour ahead: everything here runs on
	# CLOCK_MONOTONIC, so nothing may react. A negative control.
	clock_jump)    run clock_jump    'date -s "@$(( $(date +%s) + 3600 ))" >/dev/null' 'date -s "@$(( $(date +%s) - 3600 ))" >/dev/null' 3 ;;
	*) echo "unknown scenario $s" >&2 ;;
	esac
done
restore
echo "results in $OUT"
