#pragma once

// Type-erased publisher/subscriber cores. They move chunk indices and
// bytes; the typed Publisher<T>/Subscriber<T> wrappers on top only add
// the static type and the RAII handles. Keeping the cores untyped keeps
// the lock-free code compiled once, in one .cpp, instead of per payload
// type in every header that includes it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "devbus/config.hpp"
#include "devbus/detail/layout.hpp"
#include "devbus/detail/os.hpp"

namespace devbus {

struct TypeInfo {
	uint64_t hash;
	std::size_t size;
	std::size_t align;
};

// What one send() did, summed over all subscribers.
struct SendReport {
	uint64_t seq = 0;
	uint32_t delivered = 0;      // queued for a subscriber
	uint32_t evicted = 0;        // older samples discarded to make room (DropOldest)
	uint32_t rejected = 0;       // this sample refused (DropNewest)
	uint32_t block_timeouts = 0; // Block policy gave up waiting
};

enum class LoanError { LoanLimit, OutOfChunks };
enum class ReceiveStatus { Ok, Empty, BorrowLimit, PublisherGone };

namespace detail {

class PublisherCore {
public:
	PublisherCore(std::string_view service, const TypeInfo& type, const ServiceConfig& cfg);
	~PublisherCore();
	PublisherCore(const PublisherCore&) = delete;
	PublisherCore& operator=(const PublisherCore&) = delete;

	// Returns a chunk index or why there is none. Allocation-free.
	std::optional<uint32_t> loan(LoanError* error = nullptr) noexcept;
	void release_loan(uint32_t chunk) noexcept;
	SendReport send(uint32_t chunk) noexcept;
	std::byte* payload(uint32_t chunk) const noexcept;

	// Reclaims subscribers that closed or whose process died, taking back
	// every chunk they held. Runs automatically every liveness_check_every sends.
	// Returns the number of subscribers reaped.
	uint32_t check_liveness() noexcept;

	uint32_t active_subscribers() const noexcept;

	// Deepest queue occupancy across active subscribers, and the same as a
	// fraction of queue_capacity. This is the publisher-side early warning
	// that a subscriber is falling behind: it rises while everything still
	// looks healthy from the outside, long before the queue is full and the
	// overflow policy starts dropping (or, with Block, before the publisher
	// itself is dragged down). Cheap enough to call on every send - one
	// acquire load per active subscriber, no syscall.
	uint32_t max_queued() const noexcept;
	float pressure() const noexcept;

	uint32_t free_chunks() const noexcept { return static_cast<uint32_t>(free_.size()); }
	uint32_t chunk_count() const noexcept { return header_->chunk_count; }
	bool memory_locked() const noexcept { return memory_locked_; }
	const std::string& shm_name() const noexcept { return seg_.name(); }

private:
	SubscriberSlot* slot(uint32_t i) const noexcept;
	void scan_slots() noexcept;
	void rebuild_active_list() noexcept;
	void drain_done(uint32_t s) noexcept;
	void release_ref(uint32_t s, uint32_t chunk) noexcept;
	void reclaim_slot(uint32_t s) noexcept;
	bool deliver(uint32_t s, uint32_t chunk, SendReport& report) noexcept;
	void free_chunk(uint32_t chunk) noexcept;

	ShmSegment seg_;
	SegmentHeader* header_ = nullptr;
	ChunkHeader* chunk_headers_ = nullptr;
	bool memory_locked_ = false;

	// Publisher-private bookkeeping, deliberately NOT in shared memory: a
	// misbehaving or crashing subscriber can't corrupt it.
	std::vector<uint32_t> free_;          // free chunk stack
	std::vector<uint16_t> refcount_;      // per chunk: subscribers still holding it
	std::vector<uint16_t> outstanding_;   // [subscriber * chunk_count + chunk]
	std::vector<uint8_t> active_;         // per slot: publisher has onboarded it
	std::vector<int> pidfds_;             // per slot
	std::vector<uint32_t> active_list_;   // onboarded slots, Block-policy ones last
	uint32_t loans_out_ = 0;
	uint64_t next_seq_ = 1;
	uint32_t liveness_every_ = 0;
	uint32_t sends_since_liveness_ = 0;
};

class SubscriberCore {
public:
	SubscriberCore(std::string_view service, const TypeInfo& type, const SubscriberConfig& cfg);
	~SubscriberCore();
	SubscriberCore(const SubscriberCore&) = delete;
	SubscriberCore& operator=(const SubscriberCore&) = delete;

	ReceiveStatus try_receive(uint32_t& chunk) noexcept;
	void release(uint32_t chunk) noexcept;

	// Waits until data is available, the timeout expires, or the publisher
	// goes away. Returns true if data is (probably) available. A negative
	// timeout waits indefinitely.
	bool wait(std::chrono::nanoseconds timeout) noexcept;

	const ChunkHeader& header(uint32_t chunk) const noexcept { return chunk_headers_[chunk]; }
	const std::byte* payload(uint32_t chunk) const noexcept;

	bool publisher_alive() const noexcept;
	uint64_t received() const noexcept { return received_; }
	// Holes between consecutive received sequence numbers. Counted here,
	// from what actually arrived - independently of the publisher's drop
	// counters in shared memory - so the two can be cross-checked.
	uint64_t observed_gaps() const noexcept { return observed_gaps_; }
	uint32_t borrowed() const noexcept { return borrowed_; }
	uint32_t slot_index() const noexcept { return slot_index_; }

private:
	bool has_data() const noexcept;

	ShmSegment seg_;
	SegmentHeader* header_ = nullptr;
	SubscriberSlot* slot_ = nullptr;
	ChunkHeader* chunk_headers_ = nullptr;
	SubscriberConfig cfg_;
	uint32_t slot_index_ = 0;
	uint64_t mask_ = 0;
	uint64_t done_mask_ = 0;
	uint32_t borrowed_ = 0;
	uint64_t received_ = 0;
	uint64_t observed_gaps_ = 0;
	uint64_t last_seq_ = 0;
	int publisher_pidfd_ = -1;
};

} // namespace detail
} // namespace devbus
