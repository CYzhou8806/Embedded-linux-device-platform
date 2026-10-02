#pragma once

// Shared-memory layout of one devbus service. Everything here lives in
// POSIX shared-memory segments mapped at different addresses in different
// processes, so: no pointers (only offsets and chunk indices), only
// trivially-copyable data and always-lock-free atomics.
//
// Two segments since layout version 2:
//
//   control  /dev/shm/devbus.<svc>       0660, every participant maps it read-write
//     [SegmentHeader]
//     [SubscriberSlot 0][data ring][done ring]
//     [SubscriberSlot 1][data ring][done ring]
//     ...
//   data     /dev/shm/devbus.<svc>.data  0640, only the publisher can write it
//     [ChunkHeader x chunk_count]
//     [payload chunk 0][payload chunk 1]...   (each cache-line aligned)
//
// Before version 2 it was one read-write segment, so any subscriber could
// rewrite a payload another subscriber was reading at that moment (threat
// model F13). Now subscribers map the data segment PROT_READ, from a file
// they can only open O_RDONLY: the page tables, not convention, stop them.
// What a subscriber can still write is the control segment - its own
// indices, and in principle other slots' - which the publisher already
// treats as untrusted (bounds-checked, never followed as pointers). The
// worst a hostile subscriber can do there is misdirect or starve others,
// not forge content.
//
// Per publisher->subscriber pair there are two single-producer queues of
// chunk indices:
//   - data ring: publisher -> subscriber ("here is a new sample")
//   - done ring: subscriber -> publisher ("I'm finished with this chunk")
// Only the publisher ever frees chunks (owner-driven reclaim, same idea as
// iceoryx2). Reference counts live in the publisher's private memory, so a
// crashing subscriber can't corrupt them; it can only leave chunks
// outstanding, which the publisher takes back once it notices the crash.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace devbus::detail {

inline constexpr uint64_t kMagic = 0x3153554256454431ull; // "1DEVBUS1"
inline constexpr uint32_t kLayoutVersion = 2;
inline constexpr std::size_t kCacheLine = 64;

static_assert(std::atomic<uint64_t>::is_always_lock_free, "devbus needs lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "devbus needs lock-free 32-bit atomics");
static_assert(std::atomic<int32_t>::is_always_lock_free, "devbus needs lock-free 32-bit atomics");

constexpr std::size_t round_up(std::size_t value, std::size_t align) {
	return (value + align - 1) / align * align;
}

constexpr uint32_t next_pow2(uint32_t v) {
	uint32_t p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

enum class SlotState : uint32_t {
	Free = 0,    // unused
	Claimed = 1, // a subscriber is initializing it; publisher ignores it
	Active = 2,  // publisher delivers to it
	Closing = 3, // subscriber left (or died); publisher reclaims, then frees it
};

struct alignas(kCacheLine) SegmentHeader {
	uint64_t magic;
	uint32_t layout_version;
	uint32_t type_size;
	uint64_t type_hash;
	uint32_t type_align;
	uint32_t max_subscribers;
	uint32_t queue_capacity;      // power of 2
	uint32_t completion_capacity; // power of 2, >= chunk_count
	uint32_t max_borrowed;
	uint32_t max_loans;
	uint32_t chunk_count;
	uint32_t _reserved;
	uint64_t chunk_stride;
	uint64_t slot_stride;
	uint64_t slots_offset;
	uint64_t chunk_headers_offset; // in the data segment
	uint64_t payloads_offset;      // in the data segment
	uint64_t total_size;           // of the control segment
	uint64_t data_size;            // of the data segment

	std::atomic<int32_t> publisher_pid; // 0 once the publisher has shut down
	std::atomic<uint32_t> ready;        // 1 once the segment is fully initialized

	alignas(kCacheLine) std::atomic<uint64_t> published;
	std::atomic<uint64_t> loan_failures;
};

struct alignas(kCacheLine) SubscriberSlot {
	std::atomic<uint32_t> state;
	std::atomic<int32_t> pid;
	uint32_t overflow;        // devbus::Overflow
	uint32_t block_timeout_us;

	// Each index on its own cache line: publisher and subscriber write
	// different ones at high rate, and sharing a line would make every
	// write bounce the line between the two cores (false sharing).
	alignas(kCacheLine) std::atomic<uint64_t> data_head; // written by publisher
	alignas(kCacheLine) std::atomic<uint64_t> data_tail; // CAS by subscriber (pop) and publisher (evict)
	alignas(kCacheLine) std::atomic<uint64_t> done_head; // written by subscriber
	alignas(kCacheLine) std::atomic<uint64_t> done_tail; // written by publisher

	alignas(kCacheLine) std::atomic<uint32_t> futex_word; // bumped by publisher to wake
	std::atomic<uint32_t> sleeping;                       // subscriber is (about to be) in futex_wait

	// The reverse direction, for Overflow::Block: the publisher sleeps here
	// when this subscriber's queue stays full, and the subscriber wakes it
	// after taking a sample out. Same handshake as the pair above.
	alignas(kCacheLine) std::atomic<uint32_t> space_futex;   // bumped by subscriber to wake
	std::atomic<uint32_t> publisher_waiting;                 // publisher is (about to be) in futex_wait

	// Written by the publisher only; read by anyone for introspection.
	alignas(kCacheLine) std::atomic<uint64_t> delivered;
	std::atomic<uint64_t> dropped_oldest;
	std::atomic<uint64_t> dropped_newest;
	std::atomic<uint64_t> block_timeouts;
	std::atomic<uint64_t> blocked_ns;

	std::atomic<uint32_t>* data_ring() {
		return reinterpret_cast<std::atomic<uint32_t>*>(reinterpret_cast<std::byte*>(this) + sizeof(SubscriberSlot));
	}
	std::atomic<uint32_t>* done_ring(uint32_t queue_capacity) { return data_ring() + queue_capacity; }
};

struct ChunkHeader {
	uint64_t seq;       // service-wide, starts at 1
	int64_t publish_ns; // CLOCK_MONOTONIC at send()
};

struct Layout {
	uint32_t queue_capacity;
	uint32_t completion_capacity;
	uint32_t chunk_count;
	uint64_t chunk_stride;
	uint64_t slot_stride;
	uint64_t slots_offset;
	uint64_t chunk_headers_offset;
	uint64_t payloads_offset;
	uint64_t total_size; // control segment
	uint64_t data_size;  // data segment
};

// The chunk budget is what makes loan() deterministic: every place a chunk
// can be held is bounded - publisher loans, each subscriber's queue, each
// subscriber's borrowed samples - so with enough chunks for all of them at
// once, loan() can only fail if a caller exceeds max_loans.
inline Layout compute_layout(uint32_t max_subscribers, uint32_t queue_capacity, uint32_t max_borrowed,
			     uint32_t max_loans, std::size_t type_size, std::size_t type_align) {
	Layout l{};
	l.queue_capacity = next_pow2(queue_capacity < 1 ? 1 : queue_capacity);
	l.chunk_count = max_subscribers * (l.queue_capacity + max_borrowed) + max_loans;
	l.completion_capacity = next_pow2(l.chunk_count);
	l.chunk_stride = round_up(type_size, type_align > kCacheLine ? type_align : kCacheLine);
	l.slot_stride = round_up(sizeof(SubscriberSlot) +
					 sizeof(std::atomic<uint32_t>) * (l.queue_capacity + l.completion_capacity),
				 kCacheLine);
	l.slots_offset = round_up(sizeof(SegmentHeader), kCacheLine);
	l.total_size = round_up(l.slots_offset + l.slot_stride * max_subscribers, 4096);
	l.chunk_headers_offset = 0;
	l.payloads_offset = round_up(sizeof(ChunkHeader) * l.chunk_count,
				     type_align > kCacheLine ? type_align : kCacheLine);
	l.data_size = round_up(l.payloads_offset + l.chunk_stride * l.chunk_count, 4096);
	return l;
}

} // namespace devbus::detail
