#include "device.hpp"
#include "read_exact.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <unistd.h>

namespace acq {

Device::Device(std::string dev_path, std::string sysfs_dir)
	: dev_path_(std::move(dev_path)), sysfs_dir_(std::move(sysfs_dir)) {}

Device::~Device() {
	if (fd_ >= 0)
		::close(fd_);
}

void Device::open() {
	fd_ = ::open(dev_path_.c_str(), O_RDONLY);
	if (fd_ < 0) {
		throw DeviceError("failed to open " + dev_path_ + ": " + std::strerror(errno));
	}
}

std::string Device::write_sysfs(const std::string& name, const std::string& value) {
	std::ofstream f(sysfs_dir_ + name);
	if (!f) {
		throw DeviceError("failed to open sysfs attribute " + name + " for writing");
	}
	f << value;
	if (!f) {
		// sysfs write callbacks (e.g. control_store()) can reject the value
		// and return an error, which surfaces here as a failed stream write.
		throw DeviceError("write to sysfs attribute " + name + " failed (rejected by driver?)");
	}
	return value;
}

std::string Device::read_sysfs(const std::string& name) {
	std::ifstream f(sysfs_dir_ + name);
	if (!f) {
		throw DeviceError("failed to open sysfs attribute " + name + " for reading");
	}
	std::string line;
	std::getline(f, line);
	return line;
}

void Device::start_acquisition() {
	write_sysfs("control", "1");
}

void Device::stop_acquisition() {
	write_sysfs("control", "0");
}

Sample Device::read_sample() {
	// The driver's custom_acq_read() never returns EOF for a blocking
	// read - read_exact() throwing on premature EOF would only happen if
	// something closed the device out from under us, which is itself a
	// bug.
	Sample s{};
	read_exact(fd_, &s, sizeof(s), dev_path_);
	return s;
}

bool Device::wait_readable(int timeout_ms) {
	pollfd pfd{};
	pfd.fd = fd_;
	pfd.events = POLLIN;
	int ret = ::poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		if (errno == EINTR)
			return false;
		throw DeviceError(std::string("poll on ") + dev_path_ + " failed: " + std::strerror(errno));
	}
	return ret > 0 && (pfd.revents & POLLIN);
}

uint32_t Device::read_device_id() {
	return std::stoul(read_sysfs("device_id"), nullptr, 0);
}

uint32_t Device::read_fw_version() {
	return std::stoul(read_sysfs("fw_version"), nullptr, 0);
}

uint32_t Device::read_kfifo_overflow() {
	return std::stoul(read_sysfs("kfifo_overflow"), nullptr, 0);
}

} // namespace acq
