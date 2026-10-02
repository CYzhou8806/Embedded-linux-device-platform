#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "clock_calibrator.hpp"

using acq::ClockCalibrator;

namespace {
// A 1 kHz source whose clock runs `ppm` off, timestamped with Gaussian
// jitter - the shape of docs/performance.md's M3 capture.
void feed(ClockCalibrator& c, int n, double ppm, double jitter_ns, uint32_t seq0 = 0, int64_t t0 = 5'000'000'000LL) {
	std::mt19937 rng(42);
	std::normal_distribution<double> noise(0.0, jitter_ns);
	const double spacing = 1e6 / (1.0 + ppm * 1e-6); // ns
	for (int i = 0; i < n; ++i)
		c.observe(seq0 + static_cast<uint32_t>(i), t0 + static_cast<int64_t>(i * spacing + noise(rng)));
}
} // namespace

TEST(ClockCalibrator, RecoversTheMeasuredM3Drift) {
	ClockCalibrator c;
	c.reset(1000);
	feed(c, 300'000, -64.42, 5000.0); // 300 s, 5 us jitter, like the real run
	const auto r = c.result(1000);
	ASSERT_TRUE(r.ok) << r.reason;
	EXPECT_NEAR(r.drift_ppm, -64.42, 0.05);
	EXPECT_NEAR(r.measured_hz, 1000.0 * (1 - 64.42e-6), 1e-4);
	EXPECT_NEAR(r.duration_s, 300.0, 0.1);
}

// The reason for the Welford form: real irq_ts_ns values are ~1e12+ and a
// naive sum-of-products fit loses the ppm entirely at this scale.
TEST(ClockCalibrator, StaysPreciseAtLargeAbsoluteTimestamps) {
	ClockCalibrator c;
	c.reset(1000);
	feed(c, 100'000, 25.0, 0.0, 0, 3'000'000'000'000'000LL); // ~35 days of uptime
	const auto r = c.result(1000);
	ASSERT_TRUE(r.ok) << r.reason;
	EXPECT_NEAR(r.drift_ppm, 25.0, 0.01);
}

TEST(ClockCalibrator, LostSamplesDoNotBiasTheFit) {
	ClockCalibrator c;
	c.reset(1000);
	const double spacing = 1e6 / (1.0 + 30e-6);
	for (uint32_t seq = 0; seq < 20'000; ++seq) {
		if (seq % 97 == 0 && seq != 0)
			continue; // dropped
		c.observe(seq, 1'000'000'000LL + static_cast<int64_t>(seq * spacing));
	}
	const auto r = c.result(1000);
	ASSERT_TRUE(r.ok) << r.reason;
	EXPECT_GT(r.gaps, 0u);
	EXPECT_NEAR(r.drift_ppm, 30.0, 0.01);
}

TEST(ClockCalibrator, HandlesSequenceWrap) {
	ClockCalibrator c;
	c.reset(1000);
	feed(c, 10'000, -10.0, 0.0, 0xFFFFFFFFu - 5000u);
	const auto r = c.result(1000);
	ASSERT_TRUE(r.ok) << r.reason;
	EXPECT_EQ(r.gaps, 0u);
	EXPECT_NEAR(r.drift_ppm, -10.0, 0.01);
}

TEST(ClockCalibrator, SequenceRestartInvalidatesTheRun) {
	ClockCalibrator c;
	c.reset(1000);
	feed(c, 5000, 0.0, 0.0, 100'000);
	feed(c, 5000, 0.0, 0.0, 0, 20'000'000'000LL); // MCU soft-reset mid-run
	const auto r = c.result(1000);
	EXPECT_FALSE(r.ok);
	EXPECT_NE(r.reason.find("restarted"), std::string::npos);
}

TEST(ClockCalibrator, BatchedTimestampsAreRejected) {
	ClockCalibrator c;
	c.reset(1000);
	// case-07's collapsed regime: one IRQ timestamp for a run of samples.
	for (uint32_t seq = 0; seq < 5000; ++seq)
		c.observe(seq, 1'000'000'000LL + static_cast<int64_t>(seq / 10) * 10'000'000LL);
	const auto r = c.result(1000);
	EXPECT_FALSE(r.ok);
	EXPECT_NE(r.reason.find("batched"), std::string::npos);
}

TEST(ClockCalibrator, TooFewSamples) {
	ClockCalibrator c;
	c.reset(1000);
	feed(c, 10, 0.0, 0.0);
	EXPECT_FALSE(c.result(1000).ok);
}

TEST(ClockCalibrator, UnstampedSamplesAreIgnored) {
	ClockCalibrator c;
	c.reset(1000);
	c.observe(0, 0);
	c.observe(1, -5);
	EXPECT_EQ(c.result(1).samples, 0u);
}

TEST(ClockCalibrator, ResetClearsEverything) {
	ClockCalibrator c;
	c.reset(1000);
	feed(c, 5000, 0.0, 0.0, 100'000);
	feed(c, 10, 0.0, 0.0, 0); // breaks it
	c.reset(500);
	EXPECT_EQ(c.result(1).samples, 0u);
	EXPECT_EQ(c.result(1).nominal_hz, 500u);
}
