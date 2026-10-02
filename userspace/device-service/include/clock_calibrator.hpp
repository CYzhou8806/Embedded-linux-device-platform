#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace acq {

struct CalibrationResult {
	bool ok = false;
	std::string reason;     // why not ok; empty when ok
	uint64_t samples = 0;
	uint32_t nominal_hz = 0;
	double measured_hz = 0; // what the Pi's clock says the MCU actually produced
	double drift_ppm = 0;   // (measured / nominal - 1) * 1e6; negative = MCU slow
	double duration_s = 0;
	uint64_t gaps = 0;
	uint64_t repeated_timestamps = 0;
};

// docs/performance.md's M3 clock-drift measurement turned into an online
// procedure the Supervisor can run on request (Plan.md V2/M5: calibration
// as a state-machine-managed workflow). Same method as the offline one:
// with REG_SAMPLE_RATE known exactly, the MCU's sample sequence is a
// second clock, and a least-squares fit of irq_ts_ns against seq gives
// the real inter-sample spacing. The offline analysis of a 300 s capture
// found -64.42 ppm.
//
// The fit is accumulated incrementally (Welford-style co-moments) rather
// than as raw sums: irq_ts_ns is ~1e12 and sum(x*y) over a few hundred
// thousand samples would sit well past the 2^53 at which a double stops
// representing every integer.
//
// observe() is called on the acquisition thread, result() on the
// supervisor's; the mutex is only ever contended while a calibration is
// actually running.
class ClockCalibrator {
public:
	void reset(uint32_t nominal_hz);
	void observe(uint32_t seq, int64_t irq_ts_ns);
	CalibrationResult result(uint64_t min_samples) const;

private:
	struct Fit {
		uint32_t nominal_hz = 0;
		bool have_first = false;
		bool broken = false;
		std::string broken_reason;
		uint32_t last_seq = 0;
		int64_t last_ts = 0;
		int64_t first_ts = 0;
		double x = 0; // unwrapped sequence position relative to the first sample
		uint64_t n = 0;
		double mean_x = 0, mean_y = 0, cxx = 0, cxy = 0;
		uint64_t gaps = 0;
		uint64_t repeated_ts = 0;
	};

	mutable std::mutex mutex_;
	Fit fit_;
};

} // namespace acq
