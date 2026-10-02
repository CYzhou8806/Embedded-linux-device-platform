#include "clock_calibrator.hpp"

namespace acq {

namespace {
// A forward jump bigger than this is not a few lost samples but the MCU
// restarting its counter (a soft reset writes control=1, case-05) - the
// two halves of such a run are different time bases and can't be fitted
// as one line.
constexpr uint32_t kMaxSeqJump = 1000;

// docs/debugging/case-07: in the collapsed regime the driver stamps a
// whole batch with one IRQ's timestamp. A fit over those measures the
// drain cadence, not the MCU's clock. More than this fraction of repeats
// means the link was not clean enough to calibrate against.
constexpr double kMaxRepeatedFraction = 0.01;
} // namespace

void ClockCalibrator::reset(uint32_t nominal_hz) {
	std::lock_guard<std::mutex> lock(mutex_);
	fit_ = Fit{};
	fit_.nominal_hz = nominal_hz;
}

void ClockCalibrator::observe(uint32_t seq, int64_t irq_ts_ns) {
	if (irq_ts_ns <= 0)
		return; // unstamped sample, nothing to fit
	std::lock_guard<std::mutex> lock(mutex_);
	Fit& f = fit_;
	if (f.broken)
		return;

	if (!f.have_first) {
		f.have_first = true;
		f.first_ts = irq_ts_ns;
		f.x = 0;
	} else {
		// uint32 subtraction handles the 2^32 wrap for free.
		const uint32_t step = seq - f.last_seq;
		if (step == 0 || step > kMaxSeqJump) {
			f.broken = true;
			f.broken_reason = "sequence restarted during calibration (step " + std::to_string(step) + ")";
			return;
		}
		if (step != 1)
			++f.gaps; // lost samples don't bias the fit: x still advances by the real step
		if (irq_ts_ns == f.last_ts)
			++f.repeated_ts;
		f.x += step;
	}
	f.last_seq = seq;
	f.last_ts = irq_ts_ns;

	// Welford co-moment update, on time relative to the first sample.
	const double x = f.x;
	const double y = static_cast<double>(irq_ts_ns - f.first_ts);
	++f.n;
	const double dx = x - f.mean_x;
	f.mean_x += dx / static_cast<double>(f.n);
	f.mean_y += (y - f.mean_y) / static_cast<double>(f.n);
	f.cxx += dx * (x - f.mean_x);
	f.cxy += dx * (y - f.mean_y);
}

CalibrationResult ClockCalibrator::result(uint64_t min_samples) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const Fit& f = fit_;
	CalibrationResult r;
	r.samples = f.n;
	r.nominal_hz = f.nominal_hz;
	r.gaps = f.gaps;
	r.repeated_timestamps = f.repeated_ts;
	r.duration_s = f.n ? static_cast<double>(f.last_ts - f.first_ts) / 1e9 : 0.0;

	if (f.broken) {
		r.reason = f.broken_reason;
		return r;
	}
	if (f.nominal_hz == 0) {
		r.reason = "nominal sample rate unknown";
		return r;
	}
	if (f.n < min_samples || f.n < 2 || f.cxx <= 0) {
		r.reason = "too few samples (" + std::to_string(f.n) + " < " + std::to_string(min_samples) + ")";
		return r;
	}
	if (static_cast<double>(f.repeated_ts) > kMaxRepeatedFraction * static_cast<double>(f.n)) {
		r.reason = "timestamps are batched, not per-sample (" + std::to_string(f.repeated_ts) +
			   " repeats) - link congested, see case-07";
		return r;
	}

	const double spacing_ns = f.cxy / f.cxx; // fitted ns per sample
	r.measured_hz = 1e9 / spacing_ns;
	r.drift_ppm = (r.measured_hz / static_cast<double>(f.nominal_hz) - 1.0) * 1e6;
	r.ok = true;
	return r;
}

} // namespace acq
