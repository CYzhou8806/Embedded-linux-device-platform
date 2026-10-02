#include <gtest/gtest.h>

#include "prometheus.hpp"

using devmon::to_prometheus;

namespace {
nlohmann::json sample_status() {
	return {{"state", "Running"}, {"in_state_s", 42},  {"samples_read", 123456}, {"gaps", 3},
		{"sample_age_ewma_us", 973}, {"recoveries", 1}, {"recovery_attempts", 2}, {"faults", 0}};
}
bool has(const std::string& text, const std::string& line) {
	return text.find(line + "\n") != std::string::npos;
}
} // namespace

TEST(Prometheus, StateIsOneHotGauge) {
	const auto t = to_prometheus(sample_status());
	EXPECT_TRUE(has(t, "device_state{state=\"Running\"} 1"));
	EXPECT_TRUE(has(t, "device_state{state=\"Fault\"} 0"));
	EXPECT_TRUE(has(t, "# TYPE device_state gauge"));
}

TEST(Prometheus, CountersAndGauges) {
	const auto t = to_prometheus(sample_status());
	EXPECT_TRUE(has(t, "device_samples_read_total 123456"));
	EXPECT_TRUE(has(t, "device_sequence_gaps_total 3"));
	EXPECT_TRUE(has(t, "device_sample_age_microseconds 973"));
	EXPECT_TRUE(has(t, "device_recoveries_total 1"));
	EXPECT_TRUE(has(t, "device_faults_total 0"));
	EXPECT_TRUE(has(t, "# TYPE device_faults_total counter"));
}

TEST(Prometheus, DriftOnlyFromASuccessfulCalibration) {
	auto s = sample_status();
	EXPECT_EQ(to_prometheus(s).find("device_clock_drift_ppm"), std::string::npos);
	s["last_calibration"] = {{"ok", false}, {"reason", "too few samples"}};
	EXPECT_EQ(to_prometheus(s).find("device_clock_drift_ppm"), std::string::npos);
	s["last_calibration"] = {{"ok", true}, {"drift_ppm", -64.42}};
	EXPECT_TRUE(has(to_prometheus(s), "device_clock_drift_ppm -64.42"));
}

TEST(Prometheus, MissingFieldsReadAsZeroNotThrow) {
	const auto t = to_prometheus(nlohmann::json{{"state", "Fault"}});
	EXPECT_TRUE(has(t, "device_state{state=\"Fault\"} 1"));
	EXPECT_TRUE(has(t, "device_samples_read_total 0"));
}
