#include "acquisition_worker.hpp"

namespace acq {

namespace {
constexpr int kPollTimeoutMs = 200;
}

AcquisitionWorker::AcquisitionWorker(Device& device, RingBuffer<Sample>& buffer)
	: device_(device), buffer_(buffer) {}

AcquisitionWorker::~AcquisitionWorker() {
	stop();
}

void AcquisitionWorker::start() {
	stop_requested_ = false;
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
			samples_read_.fetch_add(1);

			if (last_seq_.has_value() && s.seq != *last_seq_ + 1)
				gap_count_.fetch_add(1);
			last_seq_ = s.seq;

			buffer_.push(s);
		}
	} catch (...) {
		last_error_ = std::current_exception();
	}
}

} // namespace acq
