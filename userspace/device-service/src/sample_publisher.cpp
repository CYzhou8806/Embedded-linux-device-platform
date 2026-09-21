#include "sample_publisher.hpp"

#include "devbus/acq_sample.hpp"
#include "devbus/devbus.hpp"

namespace acq {

struct SamplePublisher::Impl {
	devbus::Publisher<AcqSample> pub;
	explicit Impl(const std::string& service, const devbus::ServiceConfig& cfg)
		: pub(devbus::Publisher<AcqSample>::create(service, cfg)) {}
};

SamplePublisher::SamplePublisher(const std::string& service, uint32_t max_subscribers, uint32_t queue_capacity)
	: service_(service) {
	devbus::ServiceConfig cfg;
	cfg.max_subscribers = max_subscribers;
	cfg.queue_capacity = queue_capacity;
	impl_ = std::make_unique<Impl>(service, cfg);
}

SamplePublisher::~SamplePublisher() = default;

bool SamplePublisher::publish(const Sample& sample) noexcept {
	auto loan = impl_->pub.loan();
	if (!loan) {
		++publish_failures_;
		return false;
	}
	// One 16-byte copy, because Device::read_sample() has already read the
	// sample into its own storage. acq-bridge avoids even this by read()ing
	// straight into the loaned chunk; doing the same here would mean
	// reshaping Device's read path around devbus, which is not worth it at
	// this payload size - the copy is a few nanoseconds against a ~1 ms
	// end-to-end path.
	(*loan)->seq = sample.seq;
	(*loan)->value = sample.value;
	(*loan)->irq_ts_ns = sample.irq_ts_ns;
	impl_->pub.send(std::move(*loan));
	return true;
}

float SamplePublisher::pressure() const noexcept {
	return impl_->pub.pressure();
}

uint32_t SamplePublisher::subscribers() const noexcept {
	return impl_->pub.active_subscribers();
}

} // namespace acq
