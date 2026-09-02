#include "sequence_tracker.hpp"

#include <gtest/gtest.h>
#include <limits>

using acq::SequenceTracker;

TEST(SequenceTracker, FirstSampleIsNeverAGap) {
	// Each of these is the *first* observe() call on a fresh tracker -
	// there's no prior seq to compare against, so none can be a gap,
	// no matter how "unexpected" the starting value looks.
	EXPECT_FALSE(SequenceTracker().observe(0));
	EXPECT_FALSE(SequenceTracker().observe(500));
}

TEST(SequenceTracker, ConsecutiveIncrementsAreNotGaps) {
	SequenceTracker t;
	for (uint32_t seq = 0; seq < 1000; ++seq)
		EXPECT_FALSE(t.observe(seq));
	EXPECT_EQ(t.gap_count(), 0u);
}

TEST(SequenceTracker, SkippedSequenceIsAGap) {
	SequenceTracker t;
	t.observe(10);
	EXPECT_TRUE(t.observe(12)); // 11 was lost
	EXPECT_EQ(t.gap_count(), 1u);
}

TEST(SequenceTracker, RepeatedOrBackwardsSequenceIsAGap) {
	SequenceTracker t;
	t.observe(100);
	EXPECT_TRUE(t.observe(100)); // stuck / duplicate
	EXPECT_TRUE(t.observe(50));  // went backwards (e.g. MCU-side reset)
	EXPECT_EQ(t.gap_count(), 2u);
}

TEST(SequenceTracker, WrapsAt32BitsWithoutFalseGap) {
	SequenceTracker t;
	t.observe(std::numeric_limits<uint32_t>::max());
	EXPECT_FALSE(t.observe(0)); // UINT32_MAX + 1 wraps to 0 - not a gap
	EXPECT_EQ(t.gap_count(), 0u);
}

TEST(SequenceTracker, MultipleGapsAccumulate) {
	SequenceTracker t;
	t.observe(0);
	t.observe(5);  // gap
	t.observe(6);
	t.observe(20); // gap
	EXPECT_EQ(t.gap_count(), 2u);
}
