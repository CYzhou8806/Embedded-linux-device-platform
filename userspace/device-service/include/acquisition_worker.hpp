#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <thread>

#include "device.hpp"
#include "latency_logger.hpp"
#include "ring_buffer.hpp"
#include "sample_publisher.hpp"
#include "sequence_tracker.hpp"

namespace acq {

// Runs Device::read_sample() in a loop on its own thread, checks sequence
// continuity (testv13.py's convention: seq should increment by exactly 1,
// wrapping at 2^32 — a gap means something was lost between the MCU and
// here), and pushes each sample into the shared RingBuffer.
class AcquisitionWorker {
public:
	// latency_logger may be null (default) - meaning "not logging",
	// matching LatencyLogger's own no-op-when-path-is-empty behavior one
	// layer up. Kept as a pointer rather than a reference so callers that
	// don't care about V7 latency logging (tests, ad hoc tools) don't need
	// to construct a LatencyLogger just to pass one in.
	// sample_publisher may be null (default) = not publishing on devbus,
	// same optional-pointer arrangement as latency_logger above.
	AcquisitionWorker(Device& device, RingBuffer<Sample>& buffer, LatencyLogger* latency_logger = nullptr,
			   SamplePublisher* sample_publisher = nullptr);
	~AcquisitionWorker();

	void start();

	// Signals the read loop to stop and joins the thread. Safe to call
	// even if start() was never called.
	void stop();

	uint64_t samples_read() const { return samples_read_.load(); }
	uint64_t gap_count() const { return sequence_tracker_.gap_count(); }

	// How old the most recent sample already was when this thread read it:
	// now - irq_ts_ns, i.e. how long it sat in the driver's kfifo waiting
	// for the threaded SPI drain to get to it. Zero until the first sample.
	//
	// This is the pipeline's *leading* congestion indicator. Sweeping the
	// driver's inter_frame_us on real hardware (docs/devbus-experiments.md)
	// moved this from 1.2 ms to 13 ms - an 11x climb - while sample rate,
	// sequence gaps and kfifo_overflow all still read perfectly healthy,
	// right up to the point where the pipeline collapsed. The same shape
	// appears on a completely different axis in docs/performance.md's M0
	// overload sweep. BackpressureController consumes it through main.cpp.
	std::chrono::nanoseconds sample_age() const {
		return std::chrono::nanoseconds(sample_age_ns_.load(std::memory_order_relaxed));
	}

	// A smoothed average of sample_age(), and the signal a threshold
	// should actually be tested against. Two statistics were tried on
	// hardware first and both were wrong (2026-09-20):
	//
	//   last value  biased low. The driver drains its kfifo in batches -
	//               the first sample of a batch is the oldest, the last
	//               the freshest - so whichever happens to be most recent
	//               when a poller looks is a sample of that sawtooth. At
	//               1250 Hz / inter_frame_us=100 it read 920-950 us where
	//               docs/performance.md's M0 section records ~1720 us
	//               median for the same configuration.
	//   window peak biased high, with a floor. The oldest member of each
	//               batch is about one drain period old whatever the load,
	//               so the peak stays high even on an idle link. A
	//               threshold set against it never clears: measured, the
	//               controller ratcheted the MCU from 1250 Hz down to
	//               550 Hz with nothing actually wrong.
	//
	// An exponential moving average (alpha = 1/64, so a ~64 ms time
	// constant at 1 kHz) tracks the middle of the sawtooth instead, which
	// is the quantity M0's overload sweep saw climb 973 -> 1720 us before
	// the cliff. Cheap: two loads, a subtract, a shift and a store per
	// sample, no history buffer.
	std::chrono::nanoseconds sample_age_ewma() const {
		return std::chrono::nanoseconds(age_ewma_ns_.load(std::memory_order_relaxed));
	}

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
	LatencyLogger* latency_logger_;
	SamplePublisher* sample_publisher_;
	std::thread thread_;
	std::atomic<bool> stop_requested_{false};
	std::atomic<uint64_t> samples_read_{0};
	SequenceTracker sequence_tracker_;
	std::exception_ptr last_error_;
	std::atomic<int64_t> last_sample_ms_{0};
	std::atomic<int64_t> sample_age_ns_{0};
	std::atomic<int64_t> age_ewma_ns_{0};
};

} // namespace acq
