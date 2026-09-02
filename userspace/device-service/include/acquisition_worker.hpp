#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <thread>

#include "device.hpp"
#include "ring_buffer.hpp"
#include "sequence_tracker.hpp"

namespace acq {

// Runs Device::read_sample() in a loop on its own thread, checks sequence
// continuity (testv13.py's convention: seq should increment by exactly 1,
// wrapping at 2^32 — a gap means something was lost between the MCU and
// here), and pushes each sample into the shared RingBuffer.
class AcquisitionWorker {
public:
	AcquisitionWorker(Device& device, RingBuffer<Sample>& buffer);
	~AcquisitionWorker();

	void start();

	// Signals the read loop to stop and joins the thread. Safe to call
	// even if start() was never called.
	void stop();

	uint64_t samples_read() const { return samples_read_.load(); }
	uint64_t gap_count() const { return sequence_tracker_.gap_count(); }

	// When the most recent sample was received (steady_clock, immune to
	// wall-clock adjustments) - reset to "now" by start() so a Watchdog
	// checking this doesn't see a stale/zero value as an immediate
	// timeout before acquisition has even had a chance to produce data.
	// Read from other threads (a Watchdog's own timer), stored as raw
	// milliseconds in an atomic since std::chrono::time_point itself
	// isn't atomic-friendly.
	std::chrono::steady_clock::time_point last_sample_time() const {
		return std::chrono::steady_clock::time_point(std::chrono::milliseconds(last_sample_ms_.load()));
	}

	// Re-thrown by the caller (e.g. main) after stop() if the worker
	// thread exited due to a DeviceError rather than a requested stop.
	std::exception_ptr last_error() const { return last_error_; }

private:
	void run();

	Device& device_;
	RingBuffer<Sample>& buffer_;
	std::thread thread_;
	std::atomic<bool> stop_requested_{false};
	std::atomic<uint64_t> samples_read_{0};
	SequenceTracker sequence_tracker_;
	std::exception_ptr last_error_;
	std::atomic<int64_t> last_sample_ms_{0};
};

} // namespace acq
