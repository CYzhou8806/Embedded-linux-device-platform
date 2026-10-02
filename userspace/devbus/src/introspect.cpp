#include "devbus/introspect.hpp"

#include <filesystem>
#include <new>

#include "devbus/config.hpp"
#include "devbus/detail/layout.hpp"
#include "devbus/detail/os.hpp"

namespace devbus {

namespace {

const char* state_name(uint32_t s) {
	switch (static_cast<detail::SlotState>(s)) {
	case detail::SlotState::Claimed:
		return "claimed";
	case detail::SlotState::Active:
		return "active";
	case detail::SlotState::Closing:
		return "closing";
	default:
		return "free";
	}
}

const char* overflow_name(uint32_t o) {
	switch (static_cast<Overflow>(o)) {
	case Overflow::DropOldest:
		return "drop-oldest";
	case Overflow::DropNewest:
		return "drop-newest";
	case Overflow::Block:
		return "block";
	}
	return "?";
}

} // namespace

std::vector<ServiceInfo> list_services() {
	std::vector<ServiceInfo> out;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator("/dev/shm", ec)) {
		const std::string file = entry.path().filename().string();
		if (file.rfind("devbus.", 0) != 0)
			continue;
		if (file.size() > 5 && file.compare(file.size() - 5, 5, ".data") == 0)
			continue; // a service's payload segment, listed with its control segment
		try {
			detail::ShmSegment seg = detail::ShmSegment::open("/" + file);
			if (seg.size() < sizeof(detail::SegmentHeader))
				continue;
			auto* h = std::launder(reinterpret_cast<detail::SegmentHeader*>(seg.data()));
			if (h->magic != detail::kMagic || h->ready.load(std::memory_order_acquire) != 1 ||
			    seg.size() < h->total_size)
				continue;

			ServiceInfo info;
			info.shm_name = "/" + file;
			info.publisher_pid = h->publisher_pid.load(std::memory_order_acquire);
			info.publisher_alive = detail::process_alive(info.publisher_pid);
			info.type_size = h->type_size;
			info.queue_capacity = h->queue_capacity;
			info.chunk_count = h->chunk_count;
			info.segment_bytes = h->total_size;
			info.published = h->published.load(std::memory_order_relaxed);
			info.loan_failures = h->loan_failures.load(std::memory_order_relaxed);

			for (uint32_t i = 0; i < h->max_subscribers; ++i) {
				auto* sl = std::launder(reinterpret_cast<detail::SubscriberSlot*>(
					seg.data() + h->slots_offset + i * h->slot_stride));
				uint32_t st = sl->state.load(std::memory_order_acquire);
				if (st == static_cast<uint32_t>(detail::SlotState::Free))
					continue;
				SubscriberInfo s;
				s.slot = i;
				s.pid = sl->pid.load(std::memory_order_relaxed);
				s.state = state_name(st);
				s.overflow = overflow_name(sl->overflow);
				s.queued = sl->data_head.load(std::memory_order_relaxed) -
					   sl->data_tail.load(std::memory_order_relaxed);
				s.delivered = sl->delivered.load(std::memory_order_relaxed);
				s.dropped_oldest = sl->dropped_oldest.load(std::memory_order_relaxed);
				s.dropped_newest = sl->dropped_newest.load(std::memory_order_relaxed);
				s.block_timeouts = sl->block_timeouts.load(std::memory_order_relaxed);
				s.blocked_ns = sl->blocked_ns.load(std::memory_order_relaxed);
				info.subscribers.push_back(s);
			}
			out.push_back(std::move(info));
		} catch (const Error&) {
			// raced with a publisher shutting down; skip it
		}
	}
	return out;
}

} // namespace devbus
