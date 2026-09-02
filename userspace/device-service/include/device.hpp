#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace acq {

// Matches driver/custom-acq/custom_acq.c's struct custom_acq_sample exactly
// (seq then value, both u32, no padding on this arch) — this is what
// read() on /dev/acq0 hands back, 8 bytes at a time.
struct Sample {
	uint32_t seq;
	uint32_t value;
};

class DeviceError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Thin wrapper around /dev/acq0 (the kernel misc device) and the matching
// sysfs directory (the same SPI device's control/status attributes). No
// reconnect/retry logic here on purpose — see docs/learning-qa.md and
// Plan.md V4 Phase 2 for why that's deferred (the driver gives userspace
// no explicit "MCU disconnected" signal to react to; Phase 1 just
// surfaces failures as exceptions).
class Device {
public:
	// dev_path: e.g. "/dev/acq0". sysfs_dir: e.g.
	// "/sys/bus/spi/devices/spi0.0/" (trailing slash required).
	Device(std::string dev_path, std::string sysfs_dir);
	~Device();

	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;

	// Opens /dev/acq0. Throws DeviceError on failure.
	void open();

	// Writes "1"/"0" to sysfs_dir/control. Throws DeviceError on failure.
	void start_acquisition();
	void stop_acquisition();

	// Blocking read of exactly one sample from /dev/acq0. Throws
	// DeviceError on a real error; a signal-interrupted read (EINTR) is
	// retried internally so callers don't need their own retry loop.
	// Only call this after wait_readable() reports data is available —
	// see wait_readable()'s comment for why.
	Sample read_sample();

	// poll()s /dev/acq0 for up to timeout_ms, returns true if EPOLLIN was
	// reported (a call to read_sample() right after this is expected not
	// to block). Exists so AcquisitionWorker can periodically re-check its
	// stop flag instead of sitting in an indefinite blocking read() —
	// closing the fd out from under a thread blocked in read() is racy in
	// POSIX, so that's not how shutdown is done here.
	bool wait_readable(int timeout_ms);

	// One-shot sysfs reads, each a real SPI round trip on the driver side
	// (device_id/fw_version) or a plain kfifo_len()/counter read
	// (kfifo_overflow) — see docs/learning-qa.md Q25 for what's live vs
	// cached. Used for the startup liveness check and the shutdown report,
	// not on any hot path.
	uint32_t read_device_id();
	uint32_t read_fw_version();
	uint32_t read_kfifo_overflow();

private:
	std::string write_sysfs(const std::string& name, const std::string& value);
	std::string read_sysfs(const std::string& name);

	std::string dev_path_;
	std::string sysfs_dir_;
	int fd_ = -1;
};

} // namespace acq
