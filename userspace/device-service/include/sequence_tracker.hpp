#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace acq {

// Extracted from AcquisitionWorker so the gap-detection rule (testv13.py's
// convention: seq should increment by exactly 1, wrapping at 2^32) is a
// standalone, unit-testable piece of logic instead of inline state in a
// thread's run loop.
//
// observe() is only ever called from the single thread that owns this
// tracker (AcquisitionWorker::run()); gap_count() is read from other
// threads too (e.g. a metrics reporter polling it periodically), so the
// counter itself is atomic even though last_seq_ doesn't need to be.
class SequenceTracker {
public:
	// Returns true if this seq represents a gap relative to the last one
	// observed (never true for the very first sample seen).
	bool observe(uint32_t seq) {
		bool is_gap = last_seq_.has_value() && seq != *last_seq_ + 1;
		if (is_gap)
			gap_count_.fetch_add(1, std::memory_order_relaxed);
		last_seq_ = seq;
		return is_gap;
	}

	uint64_t gap_count() const { return gap_count_.load(std::memory_order_relaxed); }

private:
	std::optional<uint32_t> last_seq_;
	std::atomic<uint64_t> gap_count_{0};
};

} // namespace acq
