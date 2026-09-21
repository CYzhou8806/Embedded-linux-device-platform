#include "backpressure_controller.hpp"

#include <gtest/gtest.h>

using acq::next_backpressure_rate;

TEST(BackpressureController, OverflowHalvesRate) {
	EXPECT_EQ(next_backpressure_rate(1000, /*overflow=*/true, /*min=*/200, /*target=*/1000, /*divisor=*/2,
					  /*step=*/100),
		  500u);
}

TEST(BackpressureController, OverflowNeverGoesBelowFloor) {
	// 200 / 2 = 100, which is below min_hz=200 - clamps to the floor
	// instead of continuing to halve past it.
	EXPECT_EQ(next_backpressure_rate(200, /*overflow=*/true, /*min=*/200, /*target=*/1000, /*divisor=*/2,
					  /*step=*/100),
		  200u);
}

TEST(BackpressureController, CleanWindowStepsTowardTarget) {
	EXPECT_EQ(next_backpressure_rate(500, /*overflow=*/false, /*min=*/200, /*target=*/1000, /*divisor=*/2,
					  /*step=*/100),
		  600u);
}

TEST(BackpressureController, CleanWindowNeverOvershootsTarget) {
	// 950 + 100 = 1050 would overshoot target=1000 - clamps instead.
	EXPECT_EQ(next_backpressure_rate(950, /*overflow=*/false, /*min=*/200, /*target=*/1000, /*divisor=*/2,
					  /*step=*/100),
		  1000u);
}

TEST(BackpressureController, AlreadyAtTargetStaysThere) {
	EXPECT_EQ(next_backpressure_rate(1000, /*overflow=*/false, /*min=*/200, /*target=*/1000, /*divisor=*/2,
					  /*step=*/100),
		  1000u);
}

TEST(BackpressureController, ZeroDivisorTreatedAsOne) {
	// Guards against a misconfigured divisor of 0 (would be a
	// division-by-zero) rather than producing undefined behavior.
	EXPECT_EQ(next_backpressure_rate(1000, /*overflow=*/true, /*min=*/200, /*target=*/1000, /*divisor=*/0,
					  /*step=*/100),
		  1000u);
}
