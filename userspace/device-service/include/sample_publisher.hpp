#pragma once

// Publishes each acquired sample on devbus, so other processes on the
// device can consume the stream without going through /dev/acq0
// themselves (only one reader can, and each extra reader would be another
// copy). devbus README's roadmap item 1.
//
// Kept behind a pimpl so devbus's headers stay out of acquisition_worker
// and everything that includes it: this is the only translation unit in
// device-service that knows devbus exists.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "device.hpp"

namespace acq {

class SamplePublisher {
public:
	// Creating it creates the devbus service; throws if a live publisher
	// already holds that name (one publisher per service, by design).
	explicit SamplePublisher(const std::string& service, uint32_t max_subscribers = 4,
				  uint32_t queue_capacity = 16);
	~SamplePublisher();
	SamplePublisher(const SamplePublisher&) = delete;
	SamplePublisher& operator=(const SamplePublisher&) = delete;

	// Never throws and never blocks: a full subscriber queue is that
	// subscriber's problem (its own drop policy decides), not the
	// acquisition thread's. Returns false only if no chunk was available,
	// which with devbus's chunk budget means a subscriber is misbehaving.
	bool publish(const Sample& sample) noexcept;

	// Deepest subscriber queue as a fraction of its capacity - the
	// consumer-side counterpart to AcquisitionWorker::sample_age(). Rises
	// while a consumer falls behind, before any drop policy has fired.
	float pressure() const noexcept;
	uint32_t subscribers() const noexcept;
	uint64_t publish_failures() const noexcept { return publish_failures_; }

	const std::string& service() const noexcept { return service_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::string service_;
	uint64_t publish_failures_ = 0;
};

} // namespace acq
