#!/usr/bin/env bash
# Case 06 diagnostic (Plan.md V7): traces the SPI subsystem with ftrace
# while a stress run is in progress, to see whether spi_sync()/the RP1
# controller's own transfer-completion path is what stalls - see
# docs/debugging/case-06-spi-controller-stall-under-sustained-load.md's
# "Next steps" for why this is the next thing to try (ruled out: this
# driver's own lock logic and MCU firmware error recovery).
#
# Run ON the Raspberry Pi itself (needs /sys/kernel/debug/tracing), as
# root, with MCU + custom_acq.ko already up:
#
#   sudo bash tests/hardware/case06_ftrace_spi.sh [duration_seconds]
#
# CAVEAT (found 2026-09-07, see case-06 doc's "Update" section): this
# repo's actual minimal Yocto image does not carry python3, bash, or
# timeout on-target, so this script cannot literally run there as
# written - it was actually exercised as a VM-driven SSH-polling
# equivalent instead. Kept here as the reference/intended design; port
# to that image (or add these tools to it) before trying to run this
# script itself on the Pi.
#
# What it does:
#   1. Snapshots spi_rearm_fail/spi_error_count/kfifo_overflow before.
#   2. Turns on function_graph tracing filtered to every function ftrace
#      can see with "spi" as a *word*, anchored (not a bare substring -
#      "spi" is also a substring of "spin", so an unanchored grep also
#      enables tracing on every spin_lock/spin_unlock in the kernel,
#      which wraps the ring buffer on irrelevant noise long before
#      anything SPI-specific happens; confirmed this the hard way on
#      2026-09-07, see case-06 doc). Covers this driver's own
#      spi_sync_transfer() calls *and* whatever the RP1/bcm2835 SPI
#      controller driver's functions turn out to be called - not
#      hardcoded, since this repo doesn't know that driver's internals.
#   3. Runs tests/hardware/stress_test.py for duration_seconds (default
#      30) in the background.
#   4. Snapshots the counters again after, and saves the full trace
#      buffer to results/ftrace/<timestamp>.txt regardless of whether a
#      stall was detected - a clean run is useful too (rules out ftrace
#      overhead itself as a variable, gives a baseline to diff against).
#
# This does NOT interpret the trace for you - reading a raw
# function_graph trace of a real stall and pinning down which function
# actually blocks is exactly the manual analysis step V7 exists for.
# This script's job is only to make sure that trace evidence exists to
# look at, since a stall is intermittent and expensive to reproduce.
set -euo pipefail

DURATION_S="${1:-30}"
TRACE_DIR=/sys/kernel/debug/tracing
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RESULTS_DIR="$REPO_ROOT/results/ftrace"
TS="$(date +%Y%m%d-%H%M%S)"
OUT="$RESULTS_DIR/$TS.txt"

if [[ $EUID -ne 0 ]]; then
	echo "must run as root (needs /sys/kernel/debug/tracing)" >&2
	exit 1
fi

if [[ ! -d "$TRACE_DIR" ]]; then
	echo "$TRACE_DIR not present - debugfs not mounted? try: mount -t debugfs none /sys/kernel/debug" >&2
	exit 1
fi

mkdir -p "$RESULTS_DIR"

SYSFS_DIR=/sys/bus/spi/devices/spi0.0
read_counter() { cat "$SYSFS_DIR/$1" 2>/dev/null || echo "unreadable"; }

echo "=== Case 06 ftrace capture: $TS ===" | tee "$OUT"
echo "duration_s=$DURATION_S" | tee -a "$OUT"
echo "--- before ---" | tee -a "$OUT"
{
	echo "kfifo_overflow=$(read_counter kfifo_overflow)"
	echo "spi_rearm_fail=$(read_counter spi_rearm_fail)"
	echo "spi_error_count=$(read_counter spi_error_count)"
} | tee -a "$OUT"

# Reset tracer to a known state, then set up function_graph filtered to
# every function ftrace can see with "spi" in its name.
echo nop > "$TRACE_DIR/current_tracer"
echo > "$TRACE_DIR/set_ftrace_filter"      # clear any previous filter
# Word-anchored, not a bare substring - "spi" is also a substring of
# "spin" (spin_lock/spin_unlock/raw_spin_*), which would otherwise trace
# every spinlock in the kernel and wrap the ring buffer on noise.
grep -iE '(^|_)spi($|_)' "$TRACE_DIR/available_filter_functions" > "$TRACE_DIR/set_ftrace_filter" || {
	echo "warning: no spi-related functions found in available_filter_functions" >&2
}
echo function_graph > "$TRACE_DIR/current_tracer"
echo 1 > "$TRACE_DIR/tracing_on"

STALL_DETECTED=0
if ! timeout "$((DURATION_S + 15))" python3 "$REPO_ROOT/tests/hardware/stress_test.py" \
		--duration-seconds "$DURATION_S" >> "$OUT" 2>&1; then
	STALL_DETECTED=1
fi

echo 0 > "$TRACE_DIR/tracing_on"

echo "--- after ---" | tee -a "$OUT"
{
	echo "kfifo_overflow=$(read_counter kfifo_overflow)"
	echo "spi_rearm_fail=$(read_counter spi_rearm_fail)"
	echo "spi_error_count=$(read_counter spi_error_count)"
	echo "stall_detected=$STALL_DETECTED"
} | tee -a "$OUT"

echo "--- trace ---" >> "$OUT"
cat "$TRACE_DIR/trace" >> "$OUT"

# Reset the tracer so a forgotten run doesn't keep tracing (and the
# associated overhead) active indefinitely.
echo nop > "$TRACE_DIR/current_tracer"
echo > "$TRACE_DIR/set_ftrace_filter"

echo "saved: $OUT"
