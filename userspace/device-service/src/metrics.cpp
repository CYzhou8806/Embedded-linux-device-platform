#include "metrics.hpp"

#include <spdlog/spdlog.h>

namespace acq {

MetricsReporter::MetricsReporter(AcquisitionWorker& worker, RingBuffer<Sample>& buffer, Device& device,
				  std::chrono::milliseconds interval)
	: worker_(worker), buffer_(buffer), device_(device), interval_(interval) {}

MetricsReporter::~MetricsReporter() {
	stop();
}

void MetricsReporter::start() {
	thread_ = std::thread(&MetricsReporter::run, this);
}

void MetricsReporter::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_requested_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void MetricsReporter::report_once() {
	uint64_t samples = worker_.samples_read();
	uint64_t delta = samples - last_samples_read_;
	last_samples_read_ = samples;
	double rate = interval_.count() > 0 ? delta * 1000.0 / interval_.count() : 0.0;

	uint32_t kfifo_overflow = 0;
	try {
		kfifo_overflow = device_.read_kfifo_overflow();
	} catch (const std::exception& e) {
		// Don't let a transient sysfs read failure kill the metrics
		// thread - report what we have and note the read itself failed.
		spdlog::warn("metrics: failed to read kfifo_overflow: {}", e.what());
	}

	spdlog::info(
		"metrics: rate={:.1f}/s samples_read={} gap_count={} buffer={} kfifo_overflow={}",
		rate, samples, worker_.gap_count(), buffer_.size(), kfifo_overflow);
}

void MetricsReporter::run() {
	std::unique_lock<std::mutex> lock(mutex_);
	while (!cv_.wait_for(lock, interval_, [this] { return stop_requested_; })) {
		lock.unlock();
		report_once();
		if (on_tick_)
			on_tick_();
		lock.lock();
	}
}

} // namespace acq
