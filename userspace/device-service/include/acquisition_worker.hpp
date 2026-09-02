#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <thread>

#include "device.hpp"
#include "ring_buffer.hpp"

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
	uint64_t gap_count() const { return gap_count_.load(); }

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
	std::atomic<uint64_t> gap_count_{0};
	std::optional<uint32_t> last_seq_;
	std::exception_ptr last_error_;
};

} // namespace acq
