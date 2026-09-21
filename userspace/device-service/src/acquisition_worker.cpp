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

AcquisitionWorker::AcquisitionWorker(Device& device, RingBuffer<Sample>& buffer, LatencyLogger* latency_logger,
				      SamplePublisher* sample_publisher)
	: device_(device), buffer_(buffer), latency_logger_(latency_logger), sample_publisher_(sample_publisher) {}

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
			// How long this sample already sat in the driver's kfifo. The
			// leading congestion signal - see the header comment on
			// sample_age(). irq_ts_ns can be 0 on a sample the driver
			// couldn't stamp, which would otherwise read as an enormous age.
			if (s.irq_ts_ns > 0) {
				const int64_t age = recv_ts_ns - s.irq_ts_ns;
				sample_age_ns_.store(age, std::memory_order_relaxed);
				// EWMA, alpha = 1/64. Only this thread writes it, so a
				// plain load/store pair is enough - no CAS loop needed.
				// Seeded with the first sample rather than crawling up
				// from zero, which would read as "healthy" for the first
				// few hundred samples.
				const int64_t prev = age_ewma_ns_.load(std::memory_order_relaxed);
				age_ewma_ns_.store(prev == 0 ? age : prev + (age - prev) / 64, std::memory_order_relaxed);
			}
			if (latency_logger_)
				latency_logger_->log(s.seq, s.irq_ts_ns, recv_ts_ns);
			// Publish before the local ring buffer: other processes should
			// not wait on this one's consumer. publish() never blocks.
			if (sample_publisher_)
				sample_publisher_->publish(s);
			buffer_.push(s);
		}
	} catch (...) {
		last_error_ = std::current_exception();
	}
}

} // namespace acq
