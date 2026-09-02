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

	return cfg;
}

} // namespace acq
