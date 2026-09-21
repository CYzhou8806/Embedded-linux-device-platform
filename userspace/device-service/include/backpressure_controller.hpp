#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "device.hpp"

namespace acq {

// What the last check window saw. The two levels below exist because this
// pipeline gives two signals at two different times, measured on two
// independent axes (docs/performance.md's M0 overload sweep, and the
// inter_frame_us sweep in docs/devbus-experiments.md):
//
//   Warning  the pipeline is straining but has lost nothing yet. Sample
//            age (or devbus queue pressure) climbs steadily - 11x on the
//            inter_frame_us axis - while rate, sequence gaps and
//            kfifo_overflow all still read perfectly healthy.
//   Loss     kfifo_overflow moved: data is already gone. This only
//            happens at the cliff, by which point the pipeline has
//            already collapsed to roughly two thirds of its rate.
//
// Warning therefore gets a gentle correction (give back one recovery
// step, and stop ramping up), Loss keeps the original aggressive
// response (divide). The point of Warning is that there is a window -
// three sweep points wide in the measurement above - in which the
// controller can act before anything is lost. Reacting only to Loss
// gives it none of that window.
enum class Congestion {
	None,    // clean window: ramp back toward target
	Warning, // leading indicator tripped, nothing lost yet
	Loss,    // kfifo_overflow moved
};

// Pure decision function, factored out so it's unit-testable without a
// real Device/SPI link (see tests/test_backpressure_controller.cpp).
uint32_t next_backpressure_rate(uint32_t current_hz, Congestion congestion, uint32_t min_hz, uint32_t target_hz,
				 uint32_t backoff_divisor, uint32_t recovery_step_hz);

// Pre-Warning overload, kept so existing callers and tests read the same.
// overflow_this_window: did driver/custom-acq/custom_acq.c's
// kfifo_overflow counter move at all during the last check interval -
// docs/performance.md's overload sweep found this pipeline's cliff is
// sharp (0% loss right up to it), not gradual, so "any movement at all"
// is a meaningful congestion signal here, not noise to filter.
uint32_t next_backpressure_rate(uint32_t current_hz, bool overflow_this_window, uint32_t min_hz,
				 uint32_t target_hz, uint32_t backoff_divisor, uint32_t recovery_step_hz);

// Plan.md V2/M0's backpressure experiment: periodically compares
// Device::read_kfifo_overflow() against its previous value; on any
// movement, backs REG_SAMPLE_RATE off (next_backpressure_rate() above);
// on a clean window, ramps it back toward config's backpressure_target_hz.
//
// kfifo_overflow on its own is a *reactive* signal: it only moves once
// data has already been lost. The optional early_warning callback is the
// predictive one this controller lacked until 2026-09-20 - it returns
// true while the pipeline is straining but has lost nothing. main.cpp
// builds it from AcquisitionWorker::sample_age() (how old each sample
// already is when userspace reads it, which is what rises when the
// driver's SPI drain falls behind) and, when publishing, from devbus
// queue pressure (which rises when a *consumer* falls behind instead).
// Left null, this class behaves exactly as it did before.
class BackpressureController {
public:
	// early_warning may be null (default) = leading signal disabled, in
	// which case only kfifo_overflow drives the decision.
	BackpressureController(Device& device, std::chrono::milliseconds check_interval, uint32_t min_hz,
				uint32_t target_hz, uint32_t backoff_divisor, uint32_t recovery_step_hz,
				std::function<bool()> early_warning = nullptr);
	~BackpressureController();

	void start();
	void stop();

private:
	void run();
	void check_once();

	Device& device_;
	std::chrono::milliseconds check_interval_;
	uint32_t min_hz_;
	uint32_t target_hz_;
	uint32_t backoff_divisor_;
	uint32_t recovery_step_hz_;
	std::function<bool()> early_warning_;

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_requested_ = false;

	uint32_t last_overflow_ = 0;
	bool have_baseline_ = false; // first check_once() only establishes last_overflow_, doesn't act
};

} // namespace acq
