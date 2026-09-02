#include "watchdog.hpp"

#include <spdlog/spdlog.h>

namespace acq {

namespace {
// How often to check, independent of how long the timeout itself is -
// clamped so a very short configured timeout (e.g. in a test) still gets
// checked promptly, and a very long one doesn't check needlessly often.
std::chrono::milliseconds check_interval(std::chrono::milliseconds timeout) {
	auto half = timeout / 2;
	return half < std::chrono::milliseconds(200) ? std::chrono::milliseconds(200) : half;
}
} // namespace

Watchdog::Watchdog(Device& device, AcquisitionWorker& worker, std::chrono::milliseconds timeout)
	: device_(device), worker_(worker), timeout_(timeout) {}

Watchdog::~Watchdog() {
	stop();
}

void Watchdog::start() {
	thread_ = std::thread(&Watchdog::run, this);
}

void Watchdog::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_requested_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void Watchdog::check_once() {
	auto since_last_sample = std::chrono::steady_clock::now() - worker_.last_sample_time();
	if (since_last_sample < timeout_)
		return; // still healthy

	auto now = std::chrono::steady_clock::now();
	if (now - last_recovery_attempt_ < timeout_) {
		// Already tried recovering within this same timeout window -
		// don't hammer the SPI bus with repeated attempts back to back.
		return;
	}
	last_recovery_attempt_ = now;

	spdlog::warn(
		"watchdog: no sample received in {} ms (timeout {} ms), probing device...",
		std::chrono::duration_cast<std::chrono::milliseconds>(since_last_sample).count(),
		timeout_.count());

	uint32_t device_id;
	try {
		device_id = device_.read_device_id();
	} catch (const std::exception& e) {
		// The link itself is down - a software soft-reset can't fix this,
		// it needs a real hardware reset (tools/mcu-reset.sh, over SWD,
		// bypasses SPI entirely) or a manual power-cycle.
		spdlog::error(
			"watchdog: liveness probe failed ({}) - SPI link appears down; "
			"a soft reset can't fix this, try tools/mcu-reset.sh or a power-cycle",
			e.what());
		return;
	}

	spdlog::info("watchdog: device still responds (DEVICE_ID=0x{:08x}), attempting soft recovery", device_id);
	try {
		device_.stop_acquisition();
		device_.start_acquisition(); // MCU firmware resets seq_counter/FIFO on this write (case-05)
		spdlog::info("watchdog: soft recovery attempted (control 0 -> 1)");
	} catch (const std::exception& e) {
		spdlog::error("watchdog: soft recovery attempt itself failed: {}", e.what());
	}
}

void Watchdog::run() {
	auto interval = check_interval(timeout_);
	std::unique_lock<std::mutex> lock(mutex_);
	while (!cv_.wait_for(lock, interval, [this] { return stop_requested_; })) {
		lock.unlock();
		check_once();
		lock.lock();
	}
}

} // namespace acq
