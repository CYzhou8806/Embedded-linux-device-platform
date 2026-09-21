#include "devbus/detail/core.hpp"

#include <algorithm>
#include <atomic>
#include <new>
#include <thread>

#include <sched.h>
#include <unistd.h>

namespace devbus::detail {

namespace {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	asm volatile("yield" ::: "memory");
#endif
}

SegmentHeader* header_of(const ShmSegment& seg) {
	return std::launder(reinterpret_cast<SegmentHeader*>(seg.data()));
}

} // namespace

// ---------------------------------------------------------------------------
// PublisherCore
// ---------------------------------------------------------------------------

PublisherCore::PublisherCore(std::string_view service, const TypeInfo& type, const ServiceConfig& cfg) {
	if (cfg.max_subscribers == 0)
		throw Error("devbus: max_subscribers must be at least 1");
	const std::string name = detail::shm_name(service);
	const Layout l = compute_layout(cfg.max_subscribers, cfg.queue_capacity, cfg.max_borrowed, cfg.max_loans,
					type.size, type.align);
	if (l.chunk_count > UINT16_MAX)
		throw Error("devbus: too many chunks for 16-bit refcounts; lower queue_capacity or max_subscribers");

	// A segment left behind by a publisher that crashed is reclaimed; one
	// whose publisher is still running is an error (one publisher per
	// service). There's a small race between two publishers starting at
	// the same instant - O_EXCL in create() turns that into an error too.
	try {
		ShmSegment existing = ShmSegment::open(name);
		const auto* h = header_of(existing);
		if (existing.size() >= sizeof(SegmentHeader) && process_alive(h->publisher_pid.load()))
			throw Error("devbus: service '" + std::string(service) + "' already has a live publisher");
		ShmSegment::unlink(name);
	} catch (const Error& e) {
		if (std::string_view(e.what()).find("live publisher") != std::string_view::npos)
			throw;
		// open failed: no stale segment, the normal case
	}

	seg_ = ShmSegment::create(name, l.total_size);
	std::byte* base = seg_.data();

	header_ = new (base) SegmentHeader{};
	header_->magic = kMagic;
	header_->layout_version = kLayoutVersion;
	header_->type_hash = type.hash;
	header_->type_size = static_cast<uint32_t>(type.size);
	header_->type_align = static_cast<uint32_t>(type.align);
	header_->max_subscribers = cfg.max_subscribers;
	header_->queue_capacity = l.queue_capacity;
	header_->completion_capacity = l.completion_capacity;
	header_->max_borrowed = cfg.max_borrowed;
	header_->max_loans = cfg.max_loans;
	header_->chunk_count = l.chunk_count;
	header_->chunk_stride = l.chunk_stride;
	header_->slot_stride = l.slot_stride;
	header_->slots_offset = l.slots_offset;
	header_->chunk_headers_offset = l.chunk_headers_offset;
	header_->payloads_offset = l.payloads_offset;
	header_->total_size = l.total_size;

	for (uint32_t i = 0; i < cfg.max_subscribers; ++i) {
		auto* s = new (base + l.slots_offset + i * l.slot_stride) SubscriberSlot{};
		auto* rings = s->data_ring();
		for (uint32_t r = 0; r < l.queue_capacity + l.completion_capacity; ++r)
			new (&rings[r]) std::atomic<uint32_t>(0);
	}
	chunk_headers_ = std::launder(reinterpret_cast<ChunkHeader*>(base + l.chunk_headers_offset));
	for (uint32_t c = 0; c < l.chunk_count; ++c)
		new (&chunk_headers_[c]) ChunkHeader{};

	free_.reserve(l.chunk_count);
	for (uint32_t c = l.chunk_count; c-- > 0;)
		free_.push_back(c);
	refcount_.assign(l.chunk_count, 0);
	outstanding_.assign(static_cast<std::size_t>(l.chunk_count) * cfg.max_subscribers, 0);
	active_.assign(cfg.max_subscribers, 0);
	pidfds_.assign(cfg.max_subscribers, -1);
	active_list_.reserve(cfg.max_subscribers);

	if (cfg.lock_memory)
		memory_locked_ = seg_.lock();
	liveness_every_ = cfg.liveness_check_every;

	header_->publisher_pid.store(getpid(), std::memory_order_relaxed);
	header_->ready.store(1, std::memory_order_release); // publishes everything above
}

PublisherCore::~PublisherCore() {
	if (!header_)
		return;
	header_->publisher_pid.store(0, std::memory_order_release);
	for (uint32_t s = 0; s < header_->max_subscribers; ++s) {
		SubscriberSlot* sl = slot(s);
		sl->futex_word.fetch_add(1, std::memory_order_release);
		futex_wake_all(&sl->futex_word);
		if (pidfds_[s] >= 0)
			close(pidfds_[s]);
	}
	// Subscribers that still have it mapped keep working until they unmap;
	// the name disappears now so the next publisher starts clean.
	ShmSegment::unlink(seg_.name());
}

SubscriberSlot* PublisherCore::slot(uint32_t i) const noexcept {
	return std::launder(
		reinterpret_cast<SubscriberSlot*>(seg_.data() + header_->slots_offset + i * header_->slot_stride));
}

std::byte* PublisherCore::payload(uint32_t chunk) const noexcept {
	return seg_.data() + header_->payloads_offset + chunk * header_->chunk_stride;
}

uint32_t PublisherCore::active_subscribers() const noexcept {
	return static_cast<uint32_t>(active_list_.size());
}

uint32_t PublisherCore::max_queued() const noexcept {
	uint32_t worst = 0;
	for (uint32_t s : active_list_) {
		const SubscriberSlot* sl = slot(s);
		// head is only written by us, tail moves under the subscriber, so
		// this can read a value that is already stale by the time it
		// returns - always in the safe direction (the subscriber only ever
		// drains), and a pressure signal doesn't need to be exact.
		const uint64_t head = sl->data_head.load(std::memory_order_relaxed);
		const uint64_t tail = sl->data_tail.load(std::memory_order_acquire);
		const uint64_t queued = head - tail;
		if (queued > worst)
			worst = static_cast<uint32_t>(queued);
	}
	return worst;
}

float PublisherCore::pressure() const noexcept {
	return static_cast<float>(max_queued()) / static_cast<float>(header_->queue_capacity);
}

void PublisherCore::free_chunk(uint32_t chunk) noexcept {
	free_.push_back(chunk); // capacity reserved up front: never allocates
}

void PublisherCore::release_ref(uint32_t s, uint32_t chunk) noexcept {
	// Everything read from a done ring is untrusted input from another
	// process: an index we never gave this subscriber is ignored instead
	// of being allowed to corrupt the refcounts.
	if (chunk >= header_->chunk_count)
		return;
	uint16_t& out = outstanding_[static_cast<std::size_t>(s) * header_->chunk_count + chunk];
	if (out == 0)
		return;
	--out;
	if (--refcount_[chunk] == 0)
		free_chunk(chunk);
}

void PublisherCore::drain_done(uint32_t s) noexcept {
	SubscriberSlot* sl = slot(s);
	std::atomic<uint32_t>* ring = sl->done_ring(header_->queue_capacity);
	const uint64_t mask = header_->completion_capacity - 1;
	uint64_t tail = sl->done_tail.load(std::memory_order_relaxed);
	const uint64_t head = sl->done_head.load(std::memory_order_acquire);
	if (tail == head)
		return;
	for (; tail != head; ++tail)
		release_ref(s, ring[tail & mask].load(std::memory_order_relaxed));
	sl->done_tail.store(tail, std::memory_order_release);
}

void PublisherCore::reclaim_slot(uint32_t s) noexcept {
	drain_done(s);
	const uint32_t n = header_->chunk_count;
	for (uint32_t c = 0; c < n; ++c) {
		uint16_t& out = outstanding_[static_cast<std::size_t>(s) * n + c];
		if (out == 0)
			continue;
		refcount_[c] = static_cast<uint16_t>(refcount_[c] - out);
		out = 0;
		if (refcount_[c] == 0)
			free_chunk(c);
	}

	SubscriberSlot* sl = slot(s);
	sl->data_head.store(0, std::memory_order_relaxed);
	sl->data_tail.store(0, std::memory_order_relaxed);
	sl->done_head.store(0, std::memory_order_relaxed);
	sl->done_tail.store(0, std::memory_order_relaxed);
	sl->sleeping.store(0, std::memory_order_relaxed);
	sl->delivered.store(0, std::memory_order_relaxed);
	sl->dropped_oldest.store(0, std::memory_order_relaxed);
	sl->dropped_newest.store(0, std::memory_order_relaxed);
	sl->block_timeouts.store(0, std::memory_order_relaxed);
	sl->blocked_ns.store(0, std::memory_order_relaxed);
	sl->pid.store(0, std::memory_order_relaxed);
	if (pidfds_[s] >= 0) {
		close(pidfds_[s]);
		pidfds_[s] = -1;
	}
	active_[s] = 0;
	sl->state.store(static_cast<uint32_t>(SlotState::Free), std::memory_order_release);
}

void PublisherCore::scan_slots() noexcept {
	bool changed = false;
	for (uint32_t s = 0; s < header_->max_subscribers; ++s) {
		SubscriberSlot* sl = slot(s);
		auto st = static_cast<SlotState>(sl->state.load(std::memory_order_acquire));
		if (st == SlotState::Active && !active_[s]) {
			// Onboarding: happens once per subscriber, so the pidfd_open()
			// syscall here is not a steady-state hot-path cost.
			pidfds_[s] = pidfd_open(sl->pid.load(std::memory_order_relaxed));
			active_[s] = 1;
			changed = true;
		} else if (st == SlotState::Closing) {
			reclaim_slot(s);
			changed = true;
		}
	}
	if (!changed)
		return;
	rebuild_active_list();
}

void PublisherCore::rebuild_active_list() noexcept {
	active_list_.clear(); // capacity reserved in the constructor
	// Non-blocking subscribers first, Block ones last: a full Block
	// subscriber then delays the publisher only after every other
	// subscriber already has the sample.
	for (int pass = 0; pass < 2; ++pass)
		for (uint32_t s = 0; s < header_->max_subscribers; ++s)
			if (active_[s] && ((slot(s)->overflow == static_cast<uint32_t>(Overflow::Block)) == (pass == 1)))
				active_list_.push_back(s);
}

uint32_t PublisherCore::check_liveness() noexcept {
	scan_slots(); // reclaim subscribers that left cleanly, too
	uint32_t reaped = 0;
	for (uint32_t s = 0; s < header_->max_subscribers; ++s) {
		SubscriberSlot* sl = slot(s);
		auto st = static_cast<SlotState>(sl->state.load(std::memory_order_acquire));
		bool dead = false;
		if (active_[s])
			dead = !pidfd_alive(pidfds_[s]);
		else if (st == SlotState::Claimed || st == SlotState::Active)
			dead = !process_alive(sl->pid.load(std::memory_order_relaxed));
		if (dead) {
			reclaim_slot(s);
			++reaped;
		}
	}
	if (reaped)
		rebuild_active_list();
	return reaped;
}

std::optional<uint32_t> PublisherCore::loan(LoanError* error) noexcept {
	if (loans_out_ >= header_->max_loans) {
		if (error)
			*error = LoanError::LoanLimit;
		return std::nullopt;
	}
	for (uint32_t s : active_list_)
		drain_done(s);
	if (free_.empty()) {
		// With the chunk budget from compute_layout() this means a
		// subscriber broke the protocol (or died and hasn't been reaped).
		scan_slots();
		if (free_.empty()) {
			header_->loan_failures.fetch_add(1, std::memory_order_relaxed);
			if (error)
				*error = LoanError::OutOfChunks;
			return std::nullopt;
		}
	}
	uint32_t c = free_.back();
	free_.pop_back();
	++loans_out_;
	return c;
}

void PublisherCore::release_loan(uint32_t chunk) noexcept {
	--loans_out_;
	free_chunk(chunk);
}

bool PublisherCore::deliver(uint32_t s, uint32_t chunk, SendReport& report) noexcept {
	SubscriberSlot* sl = slot(s);
	std::atomic<uint32_t>* ring = sl->data_ring();
	const uint64_t cap = header_->queue_capacity;
	const uint64_t mask = cap - 1;
	const uint64_t head = sl->data_head.load(std::memory_order_relaxed);
	const auto policy = static_cast<Overflow>(sl->overflow);
	int64_t block_start = 0;

	for (;;) {
		uint64_t tail = sl->data_tail.load(std::memory_order_acquire);
		if (head - tail < cap)
			break;
		switch (policy) {
		case Overflow::DropNewest:
			sl->dropped_newest.fetch_add(1, std::memory_order_relaxed);
			++report.rejected;
			return false;
		case Overflow::DropOldest: {
			// Race the subscriber for the oldest entry: whoever wins the
			// CAS on data_tail owns it. If the subscriber popped it first,
			// there is room now anyway and the loop exits.
			uint32_t victim = ring[tail & mask].load(std::memory_order_relaxed);
			if (sl->data_tail.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel)) {
				release_ref(s, victim);
				sl->dropped_oldest.fetch_add(1, std::memory_order_relaxed);
				++report.evicted;
			}
			break;
		}
		case Overflow::Block: {
			const int64_t now = monotonic_ns();
			if (block_start == 0)
				block_start = now;
			if (now - block_start > static_cast<int64_t>(sl->block_timeout_us) * 1000 ||
			    sl->state.load(std::memory_order_acquire) != static_cast<uint32_t>(SlotState::Active)) {
				sl->block_timeouts.fetch_add(1, std::memory_order_relaxed);
				sl->blocked_ns.fetch_add(static_cast<uint64_t>(now - block_start), std::memory_order_relaxed);
				++report.block_timeouts;
				return false;
			}
			cpu_relax();
			break;
		}
		}
	}
	if (block_start != 0)
		sl->blocked_ns.fetch_add(static_cast<uint64_t>(monotonic_ns() - block_start), std::memory_order_relaxed);

	ring[head & mask].store(chunk, std::memory_order_relaxed);
	// seq_cst, not just release: this store and the sleeping load below
	// pair with the subscriber's seq_cst store to `sleeping` and load of
	// data_head in wait() (Dekker-style handshake). In the single total
	// order of seq_cst operations, either the subscriber sees the new head
	// before it sleeps, or we see its sleeping flag and wake it - never
	// neither. (A standalone fence would do the same, but ThreadSanitizer
	// can't model fences, and the tests run under it.)
	sl->data_head.store(head + 1, std::memory_order_seq_cst);
	++refcount_[chunk];
	++outstanding_[static_cast<std::size_t>(s) * header_->chunk_count + chunk];
	sl->delivered.fetch_add(1, std::memory_order_relaxed);
	++report.delivered;

	// The FUTEX_WAKE syscall is only paid when the subscriber really is asleep.
	if (sl->sleeping.load(std::memory_order_seq_cst)) {
		sl->futex_word.fetch_add(1, std::memory_order_release);
		futex_wake_all(&sl->futex_word);
	}
	return true;
}

SendReport PublisherCore::send(uint32_t chunk) noexcept {
	SendReport report;
	--loans_out_;
	report.seq = next_seq_++;
	chunk_headers_[chunk].seq = report.seq;
	chunk_headers_[chunk].publish_ns = monotonic_ns();

	scan_slots();
	for (uint32_t s : active_list_)
		deliver(s, chunk, report);
	if (refcount_[chunk] == 0)
		free_chunk(chunk); // nobody took it
	header_->published.fetch_add(1, std::memory_order_relaxed);

	if (liveness_every_ != 0 && ++sends_since_liveness_ >= liveness_every_) {
		sends_since_liveness_ = 0;
		check_liveness();
	}
	return report;
}

// ---------------------------------------------------------------------------
// SubscriberCore
// ---------------------------------------------------------------------------

SubscriberCore::SubscriberCore(std::string_view service, const TypeInfo& type, const SubscriberConfig& cfg)
    : cfg_(cfg) {
	seg_ = ShmSegment::open(detail::shm_name(service));
	if (seg_.size() < sizeof(SegmentHeader))
		throw Error("devbus: segment for '" + std::string(service) + "' is truncated");
	header_ = header_of(seg_);
	if (header_->magic != kMagic || header_->layout_version != kLayoutVersion)
		throw Error("devbus: '" + std::string(service) + "' is not a compatible devbus segment");
	if (header_->ready.load(std::memory_order_acquire) != 1)
		throw Error("devbus: service '" + std::string(service) + "' is still being created");
	if (header_->type_hash != type.hash || header_->type_size != type.size || header_->type_align != type.align)
		throw Error("devbus: payload type mismatch for service '" + std::string(service) + "'");
	const int pub = header_->publisher_pid.load(std::memory_order_acquire);
	publisher_pidfd_ = pidfd_open(pub);
	if (pub == 0 || !pidfd_alive(publisher_pidfd_)) {
		if (publisher_pidfd_ >= 0)
			close(publisher_pidfd_);
		throw Error("devbus: service '" + std::string(service) + "' has no live publisher");
	}

	chunk_headers_ = std::launder(reinterpret_cast<ChunkHeader*>(seg_.data() + header_->chunk_headers_offset));
	mask_ = header_->queue_capacity - 1;
	done_mask_ = header_->completion_capacity - 1;

	for (uint32_t i = 0; i < header_->max_subscribers; ++i) {
		auto* sl = std::launder(reinterpret_cast<SubscriberSlot*>(seg_.data() + header_->slots_offset +
									   i * header_->slot_stride));
		uint32_t expected = static_cast<uint32_t>(SlotState::Free);
		if (sl->state.compare_exchange_strong(expected, static_cast<uint32_t>(SlotState::Claimed),
						      std::memory_order_acq_rel)) {
			slot_ = sl;
			slot_index_ = i;
			break;
		}
	}
	if (!slot_) {
		close(publisher_pidfd_);
		throw Error("devbus: service '" + std::string(service) + "' has no free subscriber slot");
	}

	slot_->pid.store(getpid(), std::memory_order_relaxed);
	slot_->overflow = static_cast<uint32_t>(cfg.overflow);
	slot_->block_timeout_us = static_cast<uint32_t>(cfg.block_timeout.count());
	// The publisher doesn't touch a Claimed slot, so these plain resets
	// can't race with it. Start from "now": no history for late joiners.
	slot_->data_tail.store(slot_->data_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
	slot_->done_head.store(slot_->done_tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
	slot_->state.store(static_cast<uint32_t>(SlotState::Active), std::memory_order_release);
}

SubscriberCore::~SubscriberCore() {
	// Samples must not outlive their subscriber; any still borrowed are
	// reclaimed by the publisher anyway, as if this process had crashed.
	if (slot_)
		slot_->state.store(static_cast<uint32_t>(SlotState::Closing), std::memory_order_release);
	if (publisher_pidfd_ >= 0)
		close(publisher_pidfd_);
}

const std::byte* SubscriberCore::payload(uint32_t chunk) const noexcept {
	return seg_.data() + header_->payloads_offset + chunk * header_->chunk_stride;
}

bool SubscriberCore::has_data() const noexcept {
	return slot_->data_tail.load(std::memory_order_acquire) != slot_->data_head.load(std::memory_order_acquire);
}

bool SubscriberCore::publisher_alive() const noexcept {
	return header_->publisher_pid.load(std::memory_order_acquire) != 0 && pidfd_alive(publisher_pidfd_);
}

ReceiveStatus SubscriberCore::try_receive(uint32_t& chunk) noexcept {
	if (borrowed_ >= header_->max_borrowed)
		return ReceiveStatus::BorrowLimit;
	std::atomic<uint32_t>* ring = slot_->data_ring();
	for (;;) {
		uint64_t tail = slot_->data_tail.load(std::memory_order_acquire);
		const uint64_t head = slot_->data_head.load(std::memory_order_acquire);
		if (tail == head) {
			return header_->publisher_pid.load(std::memory_order_acquire) == 0 ? ReceiveStatus::PublisherGone
											       : ReceiveStatus::Empty;
		}
		const uint32_t c = ring[tail & mask_].load(std::memory_order_acquire);
		// Lost the race to a DropOldest eviction: retry with the new tail.
		if (!slot_->data_tail.compare_exchange_strong(tail, tail + 1, std::memory_order_acq_rel))
			continue;
		if (c >= header_->chunk_count)
			continue; // corrupt entry; never hand out an out-of-range chunk
		const uint64_t seq = chunk_headers_[c].seq;
		if (last_seq_ != 0 && seq > last_seq_ + 1)
			observed_gaps_ += seq - last_seq_ - 1;
		last_seq_ = seq;
		++borrowed_;
		++received_;
		chunk = c;
		return ReceiveStatus::Ok;
	}
}

void SubscriberCore::release(uint32_t chunk) noexcept {
	std::atomic<uint32_t>* ring = slot_->done_ring(header_->queue_capacity);
	const uint64_t head = slot_->done_head.load(std::memory_order_relaxed);
	ring[head & done_mask_].store(chunk, std::memory_order_relaxed);
	slot_->done_head.store(head + 1, std::memory_order_release);
	--borrowed_;
}

bool SubscriberCore::wait(std::chrono::nanoseconds timeout) noexcept {
	if (has_data())
		return true;
	const bool forever = timeout.count() < 0;
	const int64_t deadline = forever ? 0 : monotonic_ns() + timeout.count();
	auto publisher_gone = [this] { return header_->publisher_pid.load(std::memory_order_acquire) == 0; };

	switch (cfg_.wait_mode) {
	case WaitMode::BusySpin:
	case WaitMode::Yield:
		for (uint32_t i = 0;; ++i) {
			if (has_data())
				return true;
			if ((i & 63) == 0 && (publisher_gone() || (!forever && monotonic_ns() >= deadline)))
				return has_data();
			if (cfg_.wait_mode == WaitMode::Yield)
				sched_yield();
			else
				cpu_relax();
		}
	case WaitMode::Futex:
		for (;;) {
			const uint32_t word = slot_->futex_word.load(std::memory_order_acquire);
			slot_->sleeping.store(1, std::memory_order_seq_cst); // see PublisherCore::deliver()
			if (slot_->data_tail.load(std::memory_order_acquire) !=
				    slot_->data_head.load(std::memory_order_seq_cst) ||
			    publisher_gone()) {
				slot_->sleeping.store(0, std::memory_order_relaxed);
				return has_data();
			}
			std::chrono::nanoseconds left{-1};
			if (!forever) {
				const int64_t remaining = deadline - monotonic_ns();
				if (remaining <= 0) {
					slot_->sleeping.store(0, std::memory_order_relaxed);
					return false;
				}
				left = std::chrono::nanoseconds(remaining);
			}
			futex_wait(&slot_->futex_word, word, left);
			slot_->sleeping.store(0, std::memory_order_relaxed);
			if (has_data())
				return true;
			// Spurious wakeup or timeout: loop re-checks the deadline.
		}
	}
	return has_data();
}

} // namespace devbus::detail
