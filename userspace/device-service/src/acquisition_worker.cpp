#include "acquisition_worker.hpp"

namespace acq {

namespace {
constexpr int kPollTimeoutMs = 200;

int64_t now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// steady_clock is CLOCK_MONOTONIC on Linux glibc, the same clock the
// kernel's ktime_get_ns() (driver/custom-acq/custom_acq.c's irq_ts_ns)
// uses - directly comparable, no epoch conversion needed.
int64_t now_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

AcquisitionWorker::AcquisitionWorker(Device& device, RingBuffer<Sample>& buffer, LatencyLogger* latency_logger)
	: device_(device), buffer_(buffer), latency_logger_(latency_logger) {}

AcquisitionWorker::~AcquisitionWorker() {
	stop();
}

void AcquisitionWorker::start() {
	stop_requested_ = false;
	last_sample_ms_ = now_ms(); // grace period starts now, not at epoch 0
	thread_ = std::thread(&AcquisitionWorker::run, this);
}

void AcquisitionWorker::stop() {
	stop_requested_ = true;
	if (thread_.joinable())
		thread_.join();
}

void AcquisitionWorker::run() {
	try {
		while (!stop_requested_) {
			if (!device_.wait_readable(kPollTimeoutMs))
				continue; // timed out, loop back and re-check stop_requested_

			Sample s = device_.read_sample();
			int64_t recv_ts_ns = now_ns();
			samples_read_.fetch_add(1);
			last_sample_ms_ = now_ms();
			sequence_tracker_.observe(s.seq);
			if (latency_logger_)
				latency_logger_->log(s.seq, s.irq_ts_ns, recv_ts_ns);
			buffer_.push(s);
		}
	} catch (...) {
		last_error_ = std::current_exception();
	}
}

} // namespace acq
