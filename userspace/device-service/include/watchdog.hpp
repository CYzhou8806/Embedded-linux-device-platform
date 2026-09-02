#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "acquisition_worker.hpp"
#include "device.hpp"

namespace acq {

// /dev/acq0 and sysfs give userspace no explicit "MCU disconnected"
// signal (confirmed while researching Phase 1 - see the plan file and
// docs/learning-qa.md), so this can only infer trouble indirectly: no new
// sample for longer than timeout while acquisition is supposed to be
// running. On a timeout it does a non-invasive liveness probe
// (device_id sysfs read - already a real SPI round trip) and, if that
// still succeeds, one soft-reset attempt (stop_acquisition() then
// start_acquisition() - case-05 established that writing control=1 makes
// the MCU firmware reset its own seq_counter/FIFO). If the probe itself
// throws, the SPI link is genuinely down and this stops trying - that's
// beyond what software can fix; the log points at tools/mcu-reset.sh
// (a real hardware reset over SWD) as the manual fallback.
class Watchdog {
public:
	Watchdog(Device& device, AcquisitionWorker& worker, std::chrono::milliseconds timeout);
	~Watchdog();

	void start();
	void stop();

private:
	void run();
	void check_once();

	Device& device_;
	AcquisitionWorker& worker_;
	std::chrono::milliseconds timeout_;

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_requested_ = false;

	// Don't attempt a soft-reset more than once per timeout window - a
	// genuinely dead link would otherwise get hammered with reset
	// attempts every check interval.
	std::chrono::steady_clock::time_point last_recovery_attempt_{};
};

} // namespace acq
