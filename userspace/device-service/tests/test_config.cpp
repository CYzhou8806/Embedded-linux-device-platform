#include "config.hpp"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unistd.h>
#include <vector>

using acq::Config;

namespace {

// Writes `contents` to a temp file and returns its path; the file is
// removed when the returned guard goes out of scope. Uses mkstemp() rather
// than tmpnam() - tmpnam() just returns a name with no guarantee nothing
// else creates that path before you do (a real, if unlikely, race);
// mkstemp() atomically creates-and-opens the file in one call.
class TempFile {
public:
	explicit TempFile(const std::string& contents) {
		std::vector<char> tmpl(path_.begin(), path_.end());
		tmpl.push_back('\0');
		int fd = mkstemp(tmpl.data());
		if (fd < 0)
			throw std::runtime_error("mkstemp failed");
		path_.assign(tmpl.data());
		::write(fd, contents.data(), contents.size());
		::close(fd);
	}
	~TempFile() { std::remove(path_.c_str()); }
	const std::string& path() const { return path_; }

private:
	std::string path_ = "/tmp/device-service-test-XXXXXX";
};

} // namespace

TEST(Config, MissingFileFallsBackToDefaults) {
	Config cfg = Config::load("/nonexistent/path/does-not-exist.json");
	EXPECT_EQ(cfg.dev_path, "/dev/acq0");
	EXPECT_EQ(cfg.buffer_capacity, 4096u);
	EXPECT_EQ(cfg.liveness_timeout_ms, 3000);
}

TEST(Config, PartialFileOnlyOverridesPresentFields) {
	TempFile f(R"({"log_level": "debug"})");
	Config cfg = Config::load(f.path());
	EXPECT_EQ(cfg.log_level, "debug");         // overridden
	EXPECT_EQ(cfg.dev_path, "/dev/acq0");       // still default
	EXPECT_EQ(cfg.buffer_capacity, 4096u);      // still default
}

TEST(Config, AllFieldsOverridden) {
	TempFile f(R"({
		"dev_path": "/dev/acq1",
		"sysfs_dir": "/sys/bus/spi/devices/spi0.1/",
		"log_level": "warn",
		"buffer_capacity": 128,
		"liveness_timeout_ms": 1000,
		"metrics_interval_ms": 500
	})");
	Config cfg = Config::load(f.path());
	EXPECT_EQ(cfg.dev_path, "/dev/acq1");
	EXPECT_EQ(cfg.sysfs_dir, "/sys/bus/spi/devices/spi0.1/");
	EXPECT_EQ(cfg.log_level, "warn");
	EXPECT_EQ(cfg.buffer_capacity, 128u);
	EXPECT_EQ(cfg.liveness_timeout_ms, 1000);
	EXPECT_EQ(cfg.metrics_interval_ms, 500);
}

TEST(Config, MalformedJsonThrows) {
	TempFile f("{ not valid json");
	EXPECT_THROW(Config::load(f.path()), nlohmann::json::parse_error);
}
