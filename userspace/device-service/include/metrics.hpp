#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "acquisition_worker.hpp"
#include "device.hpp"
#include "ring_buffer.hpp"

namespace acq {

// Periodic "read existing state and log a summary line" reporter -
// deliberately not a second counting system. AcquisitionWorker already
// tracks samples_read()/gap_count(), RingBuffer already tracks size(), and
// Device can already read kfifo_overflow from sysfs; this class only adds
// the "read them on a timer and log it" behavior plus a derived rate
// (samples since the last report, divided by the interval).
class MetricsReporter {
public:
	MetricsReporter(AcquisitionWorker& worker, RingBuffer<Sample>& buffer, Device& device,
			 std::chrono::milliseconds interval);
	~MetricsReporter();

	void start();
	void stop();

	// Called by MetricsReporter's own timer, but also invoked directly by
	// main() right before shutdown so the final state is logged even if
	// the last periodic tick hasn't fired yet.
	void report_once();

	// Invoked at the end of every periodic tick (not on the explicit
	// report_once() call from shutdown). Defaults to a no-op; wiring this
	// to sd_notify(WATCHDOG=1) for the systemd integration reuses this
	// same heartbeat instead of running a second timer for that.
	void set_on_tick(std::function<void()> cb) { on_tick_ = std::move(cb); }

private:
	void run();

	AcquisitionWorker& worker_;
	RingBuffer<Sample>& buffer_;
	Device& device_;
	std::chrono::milliseconds interval_;

	std::thread thread_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool stop_requested_ = false;

	uint64_t last_samples_read_ = 0;
	std::function<void()> on_tick_;
};

} // namespace acq
