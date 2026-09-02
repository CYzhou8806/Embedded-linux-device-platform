#pragma once

#include <cstddef>
#include <string>

namespace acq {

// Reads exactly len bytes from fd into buf, retrying on EINTR and on
// partial reads (a single ::read() is not guaranteed to fill the buffer
// even when more data is coming). Throws DeviceError, tagged with `what`
// (typically the path being read from), if fd hits EOF before len bytes
// have arrived, or on any other read() error.
//
// Pulled out of Device::read_sample() so the frame-assembly logic can be
// unit tested against a plain pipe fd, without a real /dev/acq0.
void read_exact(int fd, void* buf, std::size_t len, const std::string& what);

} // namespace acq
