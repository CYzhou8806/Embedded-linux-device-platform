#include "backpressure_controller.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace acq {

uint32_t next_backpressure_rate(uint32_t current_hz, bool overflow_this_window, uint32_t min_hz,
				 uint32_t target_hz, uint32_t backoff_divisor, uint32_t recovery_step_hz) {
	if (overflow_this_window) {
		uint32_t backed_off = current_hz / std::max(1u, backoff_divisor);
		return std::max(min_hz, backed_off);
	}
	return std::min(target_hz, current_hz + recovery_step_hz);
}

BackpressureController::BackpressureController(Device& device, std::chrono::milliseconds check_interval,
						 uint32_t min_hz, uint32_t target_hz, uint32_t backoff_divisor,
						 uint32_t recovery_step_hz)
	: device_(device),
	  check_interval_(check_interval),
	  min_hz_(min_hz),
	  target_hz_(target_hz),
	  backoff_divisor_(backoff_divisor),
	  recovery_step_hz_(recovery_step_hz) {}

BackpressureController::~BackpressureController() {
	stop();
}

void BackpressureController::start() {
	thread_ = std::thread(&BackpressureController::run, this);
}

void BackpressureController::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_requested_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void BackpressureController::check_once() {
	uint32_t overflow_now;
	try {
		overflow_now = device_.read_kfifo_overflow();
	} catch (const std::exception& e) {
		spdlog::warn("backpressure: kfifo_overflow read failed ({}), skipping this window", e.what());
		return;
	}

	if (!have_baseline_) {
		// First tick just establishes a starting point - comparing
		// against a not-yet-read value would misread whatever overflow
		// happened to accumulate before this controller even started.
		last_overflow_ = overflow_now;
		have_baseline_ = true;
		return;
	}

	bool overflowed = overflow_now != last_overflow_;
	last_overflow_ = overflow_now;

	uint32_t current_hz;
	try {
		current_hz = device_.read_sample_rate();
	} catch (const std::exception& e) {
		spdlog::warn("backpressure: sample_rate read failed ({}), skipping this window", e.what());
		return;
	}

	uint32_t next_hz =
		next_backpressure_rate(current_hz, overflowed, min_hz_, target_hz_, backoff_divisor_, recovery_step_hz_);
	if (next_hz == current_hz)
		return; // already at floor/ceiling or mid-recovery with nothing to add - no need to touch SPI

	try {
		device_.write_sample_rate(next_hz);
		if (overflowed)
			spdlog::warn("backpressure: kfifo_overflow moved, backing MCU off {} -> {} Hz", current_hz, next_hz);
		else
			spdlog::info("backpressure: clean window, ramping MCU back up {} -> {} Hz", current_hz, next_hz);
	} catch (const std::exception& e) {
		spdlog::warn("backpressure: sample_rate write failed ({})", e.what());
	}
}

void BackpressureController::run() {
	std::unique_lock<std::mutex> lock(mutex_);
	while (!cv_.wait_for(lock, check_interval_, [this] { return stop_requested_; })) {
		lock.unlock();
		check_once();
		lock.lock();
	}
}

} // namespace acq
