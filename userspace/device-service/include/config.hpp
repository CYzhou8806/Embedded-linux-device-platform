#pragma once

#include <cstddef>
#include <string>

namespace acq {

// All fields have sane defaults so the service runs without a config file
// at all (convenient for local dev/testing); Config::load() only overrides
// the fields actually present in the file, and only ever throws if the
// file exists but isn't valid JSON — a missing file or missing individual
// keys both silently fall back to the default.
struct Config {
	std::string dev_path = "/dev/acq0";
	std::string sysfs_dir = "/sys/bus/spi/devices/spi0.0/";
	std::string log_level = "info";
	std::size_t buffer_capacity = 4096;
	int liveness_timeout_ms = 3000;
	int metrics_interval_ms = 5000;

	static Config load(const std::string& path);
};

} // namespace acq
