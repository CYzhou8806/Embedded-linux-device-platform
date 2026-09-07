#pragma once

#include <cstddef>
#include <string>

namespace acq {

// All fields have sane defaults so the service runs without a config file
// at all (convenient for local dev/testing); Config::load() only overrides
// the fields actually present in the file, and only ever throws if the
// file exists but isn't valid JSON — a missing file or missing individual
// keys both silently fall back to the default.
struct Config {
	std::string dev_path = "/dev/acq0";
	std::string sysfs_dir = "/sys/bus/spi/devices/spi0.0/";
	std::string log_level = "info";
	std::size_t buffer_capacity = 4096;
	int liveness_timeout_ms = 3000;
	int metrics_interval_ms = 5000;

	// Empty (default) = latency logging disabled. Set to a real path
	// (e.g. "/tmp/latency.csv") to enable per-sample IRQ-to-userspace
	// latency logging for a V7 experiment run — see LatencyLogger.
	std::string latency_log_path;

	// V7 scheduler-comparison knob: mlockall(MCL_CURRENT | MCL_FUTURE) at
	// startup (see main.cpp). Off by default — a failed attempt (no
	// CAP_IPC_LOCK / not root) is logged as a warning, not fatal, so this
	// stays safe to leave set for ordinary non-experiment runs too.
	bool lock_memory = false;

	// V7 scheduler-comparison knobs, both applied to the whole process
	// (every thread) at startup, both non-fatal on failure. Equivalent to
	// `chrt -f <sched_fifo_priority>` / `taskset -c <cpu_affinity_core>`
	// wrapping the process — implemented directly instead of relying on
	// those external tools, since this project's minimal Yocto image
	// doesn't carry util-linux's chrt/taskset.
	// 0 = leave the scheduling policy alone (default SCHED_OTHER).
	int sched_fifo_priority = 0;
	// -1 = leave CPU affinity alone (default: any CPU).
	int cpu_affinity_core = -1;

	static Config load(const std::string& path);
};

} // namespace acq
