#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace acq {

class Device;

// Plan.md V2/M5: "every fault automatically saves cross-layer evidence".
// When the Supervisor latches Fault it calls capture() once, which writes
// a single JSON file holding what each layer looked like at that moment:
//
//   service  the caller's own snapshot (state history, counters) - passed
//            in as `service`, since only the Supervisor knows it
//   driver   every custom-acq sysfs attribute, read one by one; a read
//            that fails is recorded as its error text, because that
//            failure is often the most useful line in the file (a dead
//            MCU reads device_id=0x00000000 but fw_version=EIO)
//   kernel   the kernel log lines mentioning the driver or SPI, via
//            klogctl() so it works without a dmesg binary on the image
//
// The point is that by the time someone looks at a fault, the system has
// usually been power-cycled and every one of those three is gone.
//
// Never throws: evidence capture failing must not turn one fault into two.
// Returns the path written, or empty if nothing could be written (dir
// empty = capture disabled, or the directory isn't writable - read-only
// rootfs on the production image, see docs/security/hardening.md).
std::string capture_fault_evidence(const std::string& dir, std::size_t keep, const std::string& reason,
				   const nlohmann::json& service, Device& device);

} // namespace acq
