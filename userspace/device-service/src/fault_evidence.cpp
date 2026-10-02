#include "fault_evidence.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <sys/klog.h>

#include <spdlog/spdlog.h>

#include "device.hpp"

namespace acq {

namespace {

namespace fs = std::filesystem;

// The read-only custom-acq attributes worth having in a post-mortem.
// control and the auth_* pair are left out on purpose: control is
// write-only, and reading auth_response would touch the MCU
// authentication state machine (docs/security/device-authentication.md).
constexpr const char* kSysfsAttrs[] = {
	"device_id", "fw_version", "sample_rate", "fifo_level", "kfifo_level", "kfifo_overflow",
	"policy_dropped", "spi_rearm_fail", "spi_error_count",
};

constexpr std::size_t kKernelLines = 60;

std::string utc_stamp(const char* fmt) {
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[64];
	std::strftime(buf, sizeof(buf), fmt, &tm);
	return buf;
}

nlohmann::json driver_snapshot(Device& device) {
	nlohmann::json j = nlohmann::json::object();
	for (const char* name : kSysfsAttrs) {
		try {
			j[name] = device.read_attribute(name);
		} catch (const std::exception& e) {
			j[name] = std::string("error: ") + e.what();
		}
	}
	return j;
}

// klogctl rather than /dev/kmsg or a dmesg binary: one syscall, no parsing
// of the kmsg record format, and it works on BusyBox images. Under the
// production sandbox (ProtectKernelLogs=yes) it fails with EPERM, which is
// recorded as such - that's the sandbox doing its job, not a bug.
nlohmann::json kernel_snapshot() {
	const int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, nullptr, 0);
	if (size <= 0)
		return std::string("unavailable: ") + std::strerror(errno);
	std::string buf(static_cast<std::size_t>(size), '\0');
	const int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf.data(), size);
	if (n < 0)
		return std::string("unavailable: ") + std::strerror(errno);
	buf.resize(static_cast<std::size_t>(n));

	std::vector<std::string> hits;
	std::istringstream in(buf);
	std::string line;
	while (std::getline(in, line)) {
		if (line.find("custom_acq") != std::string::npos || line.find("custom-acq") != std::string::npos ||
		    line.find("spi") != std::string::npos)
			hits.push_back(line);
	}
	const std::size_t from = hits.size() > kKernelLines ? hits.size() - kKernelLines : 0;
	return nlohmann::json(std::vector<std::string>(hits.begin() + static_cast<long>(from), hits.end()));
}

// Bounded: a device stuck in a fault/reset loop must not fill /data.
void prune(const fs::path& dir, std::size_t keep) {
	std::vector<fs::path> files;
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(dir, ec)) {
		const std::string name = e.path().filename().string();
		if (name.rfind("fault-", 0) == 0 && e.path().extension() == ".json")
			files.push_back(e.path());
	}
	if (files.size() <= keep)
		return;
	// Names embed a sortable UTC timestamp, so name order is age order.
	std::sort(files.begin(), files.end());
	for (std::size_t i = 0; i + keep < files.size(); ++i)
		fs::remove(files[i], ec);
}

} // namespace

std::string capture_fault_evidence(const std::string& dir, std::size_t keep, const std::string& reason,
				   const nlohmann::json& service, Device& device) {
	if (dir.empty())
		return {};
	try {
		nlohmann::json j;
		j["captured_at"] = utc_stamp("%Y-%m-%dT%H:%M:%SZ");
		j["reason"] = reason;
		j["service"] = service;
		j["driver"] = driver_snapshot(device);
		j["kernel"] = kernel_snapshot();

		std::error_code ec;
		fs::create_directories(dir, ec);
		// Two faults inside one second would otherwise overwrite each other.
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count() % 1000;
		char suffix[8];
		std::snprintf(suffix, sizeof(suffix), "%03lld", static_cast<long long>(ms));
		const fs::path path = fs::path(dir) / ("fault-" + utc_stamp("%Y%m%dT%H%M%S") + suffix + ".json");

		std::ofstream f(path);
		f << j.dump(2) << '\n';
		if (!f)
			throw std::runtime_error("cannot write " + path.string());
		f.close();
		prune(dir, keep);
		return path.string();
	} catch (const std::exception& e) {
		spdlog::warn("fault evidence not saved: {}", e.what());
		return {};
	}
}

} // namespace acq
