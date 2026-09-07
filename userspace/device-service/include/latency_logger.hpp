#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace acq {

// Optional per-sample CSV logger for the IRQ-to-userspace latency chain
// (Plan.md V7). Disabled (no-op) unless Config::latency_log_path is set —
// this is instrumentation for deliberate experiments, not something that
// should cost a file write on every sample during normal daily operation.
// Raw numbers only; percentiles (median/p99/p99.9/max) are computed offline
// from the CSV, not tracked live here.
class LatencyLogger {
public:
	// path empty => every log() call is a no-op.
	explicit LatencyLogger(const std::string& path);

	void log(uint32_t seq, int64_t irq_ts_ns, int64_t recv_ts_ns);

	bool enabled() const { return file_.is_open(); }

private:
	std::ofstream file_;
};

} // namespace acq
