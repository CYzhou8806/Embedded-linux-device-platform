#include "prometheus.hpp"

#include <sstream>

namespace devmon {

namespace {
constexpr const char* kStates[] = {"Init", "Ready", "Running", "Paused", "Calibrating", "Recovering", "Fault", "Stopped"};

void metric(std::ostringstream& out, const char* name, const char* type, const char* help, double value) {
	out << "# HELP " << name << ' ' << help << '\n'
	    << "# TYPE " << name << ' ' << type << '\n'
	    << name << ' ' << value << '\n';
}

double num(const nlohmann::json& j, const char* key) {
	const auto it = j.find(key);
	return (it != j.end() && it->is_number()) ? it->get<double>() : 0.0;
}
} // namespace

std::string to_prometheus(const nlohmann::json& status) {
	std::ostringstream out;
	out.precision(10);
	const std::string state = status.value("state", "");
	out << "# HELP device_state Machine state of the acquisition device (1 = current).\n"
	    << "# TYPE device_state gauge\n";
	for (const char* s : kStates)
		out << "device_state{state=\"" << s << "\"} " << (state == s ? 1 : 0) << '\n';

	metric(out, "device_state_seconds", "gauge", "Seconds in the current state.", num(status, "in_state_s"));
	metric(out, "device_samples_read_total", "counter", "Samples read from /dev/acq0.", num(status, "samples_read"));
	metric(out, "device_sequence_gaps_total", "counter", "Sequence gaps seen (lost samples).", num(status, "gaps"));
	metric(out, "device_sample_age_microseconds", "gauge",
	       "Smoothed age of a sample when userspace reads it (leading congestion signal).",
	       num(status, "sample_age_ewma_us"));
	metric(out, "device_recoveries_total", "counter", "Successful automatic recoveries.", num(status, "recoveries"));
	metric(out, "device_recovery_attempts_total", "counter", "Soft-reset attempts.", num(status, "recovery_attempts"));
	metric(out, "device_faults_total", "counter", "Times the device latched Fault.", num(status, "faults"));
	if (const auto it = status.find("last_calibration"); it != status.end() && it->value("ok", false))
		metric(out, "device_clock_drift_ppm", "gauge", "MCU clock drift from the last calibration.",
		       it->value("drift_ppm", 0.0));
	return out.str();
}

} // namespace devmon
