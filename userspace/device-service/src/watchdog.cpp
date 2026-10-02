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

Watchdog::Watchdog(AcquisitionWorker& worker, std::chrono::milliseconds timeout, std::function<bool()> armed,
		   std::function<void(std::chrono::milliseconds)> on_stall)
	: worker_(worker), timeout_(timeout), armed_(std::move(armed)), on_stall_(std::move(on_stall)) {}

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
	if (armed_ && !armed_())
		return; // paused, calibrating, recovering...: silence is expected

	const auto now = std::chrono::steady_clock::now();
	const auto since_last_sample = now - worker_.last_sample_time();
	if (since_last_sample < timeout_)
		return; // still healthy
	if (now - last_report_ < timeout_)
		return; // already reported this one
	last_report_ = now;

	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_last_sample);
	spdlog::warn("watchdog: no sample received in {} ms (timeout {} ms)", ms.count(), timeout_.count());
	if (on_stall_)
		on_stall_(ms);
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
