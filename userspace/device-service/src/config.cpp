#include "config.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace acq {

Config Config::load(const std::string& path) {
	Config cfg; // start from defaults

	std::ifstream f(path);
	if (!f)
		return cfg; // no file - not an error, just run on defaults

	nlohmann::json j;
	f >> j; // throws nlohmann::json::parse_error on malformed JSON - deliberately not caught here

	// value(key, default) only overrides fields actually present in the
	// file; anything missing keeps whatever Config's own default already
	// set above.
	cfg.dev_path = j.value("dev_path", cfg.dev_path);
	cfg.sysfs_dir = j.value("sysfs_dir", cfg.sysfs_dir);
	cfg.log_level = j.value("log_level", cfg.log_level);
	cfg.buffer_capacity = j.value("buffer_capacity", cfg.buffer_capacity);
	cfg.liveness_timeout_ms = j.value("liveness_timeout_ms", cfg.liveness_timeout_ms);
	cfg.metrics_interval_ms = j.value("metrics_interval_ms", cfg.metrics_interval_ms);
	cfg.latency_log_path = j.value("latency_log_path", cfg.latency_log_path);
	cfg.lock_memory = j.value("lock_memory", cfg.lock_memory);
	cfg.sched_fifo_priority = j.value("sched_fifo_priority", cfg.sched_fifo_priority);
	cfg.cpu_affinity_core = j.value("cpu_affinity_core", cfg.cpu_affinity_core);
	cfg.backpressure_enabled = j.value("backpressure_enabled", cfg.backpressure_enabled);
	cfg.backpressure_check_interval_ms = j.value("backpressure_check_interval_ms", cfg.backpressure_check_interval_ms);
	cfg.backpressure_min_hz = j.value("backpressure_min_hz", cfg.backpressure_min_hz);
	cfg.backpressure_target_hz = j.value("backpressure_target_hz", cfg.backpressure_target_hz);
	cfg.backpressure_backoff_divisor = j.value("backpressure_backoff_divisor", cfg.backpressure_backoff_divisor);
	cfg.backpressure_recovery_step_hz = j.value("backpressure_recovery_step_hz", cfg.backpressure_recovery_step_hz);
	cfg.backpressure_max_sample_age_us = j.value("backpressure_max_sample_age_us", cfg.backpressure_max_sample_age_us);
	cfg.backpressure_max_devbus_pressure =
		j.value("backpressure_max_devbus_pressure", cfg.backpressure_max_devbus_pressure);
	cfg.devbus_service = j.value("devbus_service", cfg.devbus_service);
	cfg.devbus_max_subscribers = j.value("devbus_max_subscribers", cfg.devbus_max_subscribers);
	cfg.devbus_queue_capacity = j.value("devbus_queue_capacity", cfg.devbus_queue_capacity);
	cfg.autostart = j.value("autostart", cfg.autostart);
	cfg.control_socket = j.value("control_socket", cfg.control_socket);
	cfg.state_dir = j.value("state_dir", cfg.state_dir);
	cfg.evidence_keep = j.value("evidence_keep", cfg.evidence_keep);
	cfg.max_recovery_attempts = j.value("max_recovery_attempts", cfg.max_recovery_attempts);
	cfg.recovery_timeout_ms = j.value("recovery_timeout_ms", cfg.recovery_timeout_ms);
	cfg.recovery_backoff_ms = j.value("recovery_backoff_ms", cfg.recovery_backoff_ms);
	cfg.calibration_duration_ms = j.value("calibration_duration_ms", cfg.calibration_duration_ms);
	cfg.calibration_min_samples = j.value("calibration_min_samples", cfg.calibration_min_samples);

	return cfg;
}

} // namespace acq
