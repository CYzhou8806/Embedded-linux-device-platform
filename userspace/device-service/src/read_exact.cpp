#include "read_exact.hpp"

#include "device.hpp"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace acq {

void read_exact(int fd, void* buf, std::size_t len, const std::string& what) {
	char* p = static_cast<char*>(buf);
	std::size_t total = 0;
	while (total < len) {
		ssize_t n = ::read(fd, p + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			throw DeviceError("read from " + what + " failed: " + std::strerror(errno));
		}
		if (n == 0) {
			throw DeviceError(what + " read returned EOF unexpectedly");
		}
		total += static_cast<std::size_t>(n);
	}
}

} // namespace acq
