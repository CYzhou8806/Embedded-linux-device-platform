#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "device.hpp"

namespace acq {

// Pure decision function, factored out so it's unit-testable without a
// real Device/SPI link (see tests/test_backpressure_controller.cpp).
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
// This is a *reactive* signal, not the predictive one
// docs/performance.md's overload-sweep section found (median latency
// climbing steadily before loss starts, right up to the cliff) - reacting
// to kfifo_overflow only fires once real loss has already begun. Using it
// anyway because it's already observable via existing sysfs (no new
// AcquisitionWorker/LatencyLogger plumbing needed); wiring the earlier
// latency-climb signal in is a real improvement left for later, not done
// here.
class BackpressureController {
public:
	BackpressureController(Device& device, std::chrono::milliseconds check_interval, uint32_t min_hz,
				uint32_t target_hz, uint32_t backoff_divisor, uint32_t recovery_step_hz);
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

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_requested_ = false;

	uint32_t last_overflow_ = 0;
	bool have_baseline_ = false; // first check_once() only establishes last_overflow_, doesn't act
};

} // namespace acq
