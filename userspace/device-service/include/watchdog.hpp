#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "acquisition_worker.hpp"

namespace acq {

// /dev/acq0 and sysfs give userspace no explicit "MCU disconnected"
// signal (confirmed while researching Phase 1 - see the plan file and
// docs/learning-qa.md), so trouble can only be inferred indirectly: no new
// sample for longer than timeout while acquisition is supposed to be
// running.
//
// Detection only, since M5 (Plan.md V2). This class used to also decide
// what to do about a stall - probe, then soft-reset - which meant it
// would happily soft-reset the MCU in the middle of a requested pause or
// a calibration run, because it had no idea either existed. Now it asks
// `armed()` whether a stall is even meaningful right now, and hands any
// stall to `on_stall`; Supervisor owns the recovery policy (retries,
// backoff, giving up into Fault).
class Watchdog {
public:
	Watchdog(AcquisitionWorker& worker, std::chrono::milliseconds timeout, std::function<bool()> armed,
		 std::function<void(std::chrono::milliseconds)> on_stall);
	~Watchdog();

	void start();
	void stop();

	// One check, as the timer thread would do it. Exposed for tests.
	void check_once();

private:
	void run();

	AcquisitionWorker& worker_;
	std::chrono::milliseconds timeout_;
	std::function<bool()> armed_;
	std::function<void(std::chrono::milliseconds)> on_stall_;

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_requested_ = false;

	// Report a given stall once per timeout window, not on every check -
	// the receiver is busy working on the first report.
	std::chrono::steady_clock::time_point last_report_{};
};

} // namespace acq
