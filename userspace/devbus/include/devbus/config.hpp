#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace devbus {

// What a publisher does for one subscriber whose queue is full. Chosen per
// subscriber, not per service: a recorder and a live display reading the
// same stream want different things when they fall behind.
//
// - DropOldest: evict the oldest queued sample. The subscriber always gets
//   the freshest data; the past has holes. Publisher never waits.
//   (iceoryx2 calls this "safe overflow".)
// - DropNewest: reject the new sample for this subscriber. It gets a
//   complete-but-stale history. Publisher never waits.
// - Block: publisher waits for space, up to block_timeout, then drops.
//   The only policy that lets one subscriber slow the producer down - and
//   therefore every other subscriber too. The timeout bounds that damage.
enum class Overflow : uint32_t {
	DropOldest = 0,
	DropNewest = 1,
	Block = 2,
};

// How a subscriber waits for data. Latency vs. CPU trade-off:
// - BusySpin: lowest latency, burns a whole core.
// - Yield: spin with sched_yield(); still ~100% CPU, friendlier to
//   other threads on the same core.
// - Futex: sleeps in the kernel; the publisher issues a FUTEX_WAKE only
//   when the subscriber is actually asleep, so an awake subscriber costs
//   the publisher no syscall at all.
enum class WaitMode : uint32_t {
	BusySpin = 0,
	Yield = 1,
	Futex = 2,
};

// Fixed when the service is created. Shared memory is sized from it once;
// nothing is allocated afterwards.
struct ServiceConfig {
	uint32_t max_subscribers = 4;
	uint32_t queue_capacity = 16; // per subscriber, rounded up to a power of 2
	uint32_t max_borrowed = 2;    // samples one subscriber may hold at once
	uint32_t max_loans = 2;       // chunks the publisher may hold unsent at once
	// mlock() the whole segment so the hot path never takes a page fault.
	// Needs CAP_IPC_LOCK or a large enough RLIMIT_MEMLOCK; failure is
	// reported by Publisher::memory_locked(), not fatal.
	bool lock_memory = false;
	// How often (in sends) the publisher checks whether subscriber
	// processes are still alive. The check is one poll() syscall; 0 means
	// only when check_liveness() is called explicitly.
	uint32_t liveness_check_every = 1024;
};

struct SubscriberConfig {
	Overflow overflow = Overflow::DropOldest;
	std::chrono::microseconds block_timeout{1000}; // only for Overflow::Block
	WaitMode wait_mode = WaitMode::Futex;
};

// Setup-time failures (create/open). The hot path (loan/send/receive)
// never throws; it reports through return values.
class Error : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

} // namespace devbus
