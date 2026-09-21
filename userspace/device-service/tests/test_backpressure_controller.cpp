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

// --- Congestion::Warning: the leading signal added 2026-09-20 -----------
//
// The point of this level is that there is a window between "straining"
// and "losing data" (measured: three sweep points and an 11x latency
// climb, docs/devbus-experiments.md). These tests pin down that a warning
// is treated as strictly gentler than loss and strictly firmer than a
// clean window - the property the whole idea rests on.

using acq::Congestion;

TEST(BackpressureController, WarningEasesOffByOneStepNotByHalving) {
	EXPECT_EQ(next_backpressure_rate(1000, Congestion::Warning, /*min=*/200, /*target=*/1000, /*divisor=*/2,
					  /*step=*/100),
		  900u);
	// For contrast, the same rate with actual loss:
	EXPECT_EQ(next_backpressure_rate(1000, Congestion::Loss, 200, 1000, 2, 100), 500u);
}

TEST(BackpressureController, WarningStopsTheRampUp) {
	// Mid-recovery at 500 Hz: a clean window would climb, a warning must not.
	EXPECT_EQ(next_backpressure_rate(500, Congestion::None, 200, 1000, 2, 100), 600u);
	EXPECT_EQ(next_backpressure_rate(500, Congestion::Warning, 200, 1000, 2, 100), 400u);
}

TEST(BackpressureController, WarningNeverGoesBelowFloor) {
	EXPECT_EQ(next_backpressure_rate(200, Congestion::Warning, /*min=*/200, 1000, 2, /*step=*/100), 200u);
	// A rate below one full step from the floor clamps rather than wrapping
	// around (these are unsigned).
	EXPECT_EQ(next_backpressure_rate(250, Congestion::Warning, /*min=*/200, 1000, 2, /*step=*/100), 200u);
	EXPECT_EQ(next_backpressure_rate(50, Congestion::Warning, /*min=*/200, 1000, 2, /*step=*/100), 200u);
}

TEST(BackpressureController, WarningAndCleanWindowsSettleInsteadOfOscillating) {
	// Back-off and ramp are the same size, so a link hovering at the edge
	// alternates between two adjacent rates rather than sawtoothing across
	// the whole range the way a halve-then-ramp cycle would.
	uint32_t hz = 1000;
	hz = next_backpressure_rate(hz, Congestion::Warning, 200, 1000, 2, 100); // 900
	hz = next_backpressure_rate(hz, Congestion::None, 200, 1000, 2, 100);    // 1000
	hz = next_backpressure_rate(hz, Congestion::Warning, 200, 1000, 2, 100); // 900
	EXPECT_EQ(hz, 900u);
}

TEST(BackpressureController, BoolOverloadStillMeansLossOrClean) {
	// The pre-Warning signature has to keep behaving exactly as before -
	// callers and the earlier tests above depend on it.
	EXPECT_EQ(next_backpressure_rate(1000, true, 200, 1000, 2, 100),
		  next_backpressure_rate(1000, Congestion::Loss, 200, 1000, 2, 100));
	EXPECT_EQ(next_backpressure_rate(500, false, 200, 1000, 2, 100),
		  next_backpressure_rate(500, Congestion::None, 200, 1000, 2, 100));
}
