#pragma once

// devbus - zero-copy publish/subscribe over shared memory for one Linux
// device. See README.md for the design and docs/design/devbus.md for the
// reasoning behind it.
//
//   // producer process
//   auto pub = devbus::Publisher<Frame>::create("camera/frames");
//   if (auto loan = pub.loan()) {
//           fill(*loan);                 // written straight into shared memory
//           pub.send(std::move(*loan));  // publishes an index, copies nothing
//   }
//
//   // consumer process
//   devbus::Subscriber<Frame> sub("camera/frames", {.overflow = devbus::Overflow::DropOldest});
//   while (sub.wait(100ms))
//           while (auto sample = sub.receive())
//                   process(**sample);   // reads the publisher's memory in place

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "devbus/config.hpp"
#include "devbus/detail/core.hpp"

namespace devbus {

namespace detail {

constexpr uint64_t fnv1a(std::string_view s) {
	uint64_t h = 1469598103934665603ull;
	for (char c : s) {
		h ^= static_cast<unsigned char>(c);
		h *= 1099511628211ull;
	}
	return h;
}

// The compiler's own spelling of T, hashed. Enough to catch "publisher and
// subscriber were built against different payload types" - which, with
// shared memory, would otherwise be silent garbage, not a crash.
template <typename T>
constexpr uint64_t type_hash() {
	return fnv1a(__PRETTY_FUNCTION__);
}

} // namespace detail

template <typename T>
constexpr TypeInfo type_info_of() {
	return TypeInfo{detail::type_hash<T>(), sizeof(T), alignof(T)};
}

template <typename T>
concept ShmPayload = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template <ShmPayload T>
class Publisher;

// A chunk of shared memory the publisher is writing into. Not yet visible
// to anyone. Dropping it without send() gives the chunk back.
template <ShmPayload T>
class Loan {
public:
	Loan(Loan&& other) noexcept
	    : core_(std::exchange(other.core_, nullptr)), chunk_(other.chunk_), ptr_(other.ptr_) {}
	Loan& operator=(Loan&&) = delete;
	Loan(const Loan&) = delete;
	~Loan() {
		if (core_)
			core_->release_loan(chunk_);
	}

	// The memory is uninitialized from this loan's point of view (it holds
	// whatever the previous sample in this chunk left behind): write every
	// field you rely on.
	T& operator*() noexcept { return *ptr_; }
	T* operator->() noexcept { return ptr_; }

private:
	friend class Publisher<T>;
	Loan(detail::PublisherCore* core, uint32_t chunk)
	    : core_(core), chunk_(chunk), ptr_(reinterpret_cast<T*>(core->payload(chunk))) {}

	detail::PublisherCore* core_ = nullptr;
	uint32_t chunk_ = 0;
	T* ptr_ = nullptr;
};

// The single publisher of a service. Creating it creates the service.
// Not thread-safe: use it from one thread (there is exactly one producer
// per service by design).
template <ShmPayload T>
class Publisher {
public:
	static Publisher create(std::string_view service, const ServiceConfig& cfg = {}) {
		return Publisher(std::make_unique<detail::PublisherCore>(service, type_info_of<T>(), cfg));
	}

	std::optional<Loan<T>> loan(LoanError* error = nullptr) noexcept {
		if (auto chunk = core_->loan(error))
			return Loan<T>(core_.get(), *chunk);
		return std::nullopt;
	}

	SendReport send(Loan<T>&& loan) noexcept {
		detail::PublisherCore* core = std::exchange(loan.core_, nullptr);
		return core->send(loan.chunk_);
	}

	// Convenience for small payloads: one copy into shared memory. For big
	// ones, write into loan() directly - that's the zero-copy path.
	std::optional<SendReport> publish_copy(const T& value, LoanError* error = nullptr) noexcept {
		auto l = loan(error);
		if (!l)
			return std::nullopt;
		**l = value;
		return send(std::move(*l));
	}

	uint32_t check_liveness() noexcept { return core_->check_liveness(); }
	uint32_t active_subscribers() const noexcept { return core_->active_subscribers(); }

	// How far behind the slowest subscriber is: samples sitting in its
	// queue, and that as a fraction of queue_capacity. Rises before any
	// drop policy fires, so it is usable as a leading congestion signal -
	// unlike a drop counter, which only moves once data is already lost.
	uint32_t max_queued() const noexcept { return core_->max_queued(); }
	float pressure() const noexcept { return core_->pressure(); }
	uint32_t free_chunks() const noexcept { return core_->free_chunks(); }
	uint32_t chunk_count() const noexcept { return core_->chunk_count(); }
	bool memory_locked() const noexcept { return core_->memory_locked(); }

private:
	explicit Publisher(std::unique_ptr<detail::PublisherCore> core) : core_(std::move(core)) {}
	std::unique_ptr<detail::PublisherCore> core_;
};

template <ShmPayload T>
class Subscriber;

// A received sample: a read-only view of the publisher's shared memory.
// Destroying it tells the publisher this subscriber is done with the chunk.
// Must be destroyed on the subscriber's thread, before the Subscriber.
template <ShmPayload T>
class Sample {
public:
	Sample(Sample&& other) noexcept
	    : core_(std::exchange(other.core_, nullptr)), chunk_(other.chunk_), ptr_(other.ptr_) {}
	Sample& operator=(Sample&&) = delete;
	Sample(const Sample&) = delete;
	~Sample() {
		if (core_)
			core_->release(chunk_);
	}

	const T& operator*() const noexcept { return *ptr_; }
	const T* operator->() const noexcept { return ptr_; }
	uint64_t seq() const noexcept { return core_->header(chunk_).seq; }
	int64_t publish_ns() const noexcept { return core_->header(chunk_).publish_ns; }

private:
	friend class Subscriber<T>;
	Sample(detail::SubscriberCore* core, uint32_t chunk)
	    : core_(core), chunk_(chunk), ptr_(reinterpret_cast<const T*>(core->payload(chunk))) {}

	detail::SubscriberCore* core_ = nullptr;
	uint32_t chunk_ = 0;
	const T* ptr_ = nullptr;
};

// One subscriber of a service. Opening fails if the service has no live
// publisher yet - the caller decides whether to retry. Not thread-safe.
template <ShmPayload T>
class Subscriber {
public:
	explicit Subscriber(std::string_view service, const SubscriberConfig& cfg = {})
	    : core_(std::make_unique<detail::SubscriberCore>(service, type_info_of<T>(), cfg)) {}

	std::optional<Sample<T>> receive(ReceiveStatus* status = nullptr) noexcept {
		uint32_t chunk = 0;
		ReceiveStatus st = core_->try_receive(chunk);
		if (status)
			*status = st;
		if (st != ReceiveStatus::Ok)
			return std::nullopt;
		return Sample<T>(core_.get(), chunk);
	}

	template <typename Rep, typename Period>
	bool wait(std::chrono::duration<Rep, Period> timeout) noexcept {
		return core_->wait(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout));
	}

	bool publisher_alive() const noexcept { return core_->publisher_alive(); }
	uint64_t received() const noexcept { return core_->received(); }
	uint64_t observed_gaps() const noexcept { return core_->observed_gaps(); }
	uint32_t borrowed() const noexcept { return core_->borrowed(); }

private:
	std::unique_ptr<detail::SubscriberCore> core_;
};

} // namespace devbus
