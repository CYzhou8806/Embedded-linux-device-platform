#include "devbus/detail/os.hpp"

#include <cerrno>
#include <climits>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "devbus/config.hpp"

namespace devbus::detail {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
	throw Error(what + ": " + std::strerror(errno));
}

std::byte* map(int fd, std::size_t size, const std::string& name, bool writable = true) {
	// MAP_POPULATE pre-faults every page now, at setup time, instead of on
	// first touch somewhere in the hot path.
	void* p = mmap(nullptr, size, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
	if (p == MAP_FAILED)
		throw_errno("mmap " + name);
	return static_cast<std::byte*>(p);
}

} // namespace

std::string shm_name(std::string_view service) {
	std::string name = "/devbus.";
	for (char c : service)
		name += (c == '/') ? '.' : c; // shm names can't contain further slashes
	return name;
}

std::string shm_data_name(std::string_view service) {
	return shm_name(service) + ".data";
}

ShmSegment::~ShmSegment() {
	if (data_)
		munmap(data_, size_);
}

ShmSegment::ShmSegment(ShmSegment&& other) noexcept
    : name_(std::move(other.name_)), data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

ShmSegment& ShmSegment::operator=(ShmSegment&& other) noexcept {
	if (this != &other) {
		if (data_)
			munmap(data_, size_);
		name_ = std::move(other.name_);
		data_ = std::exchange(other.data_, nullptr);
		size_ = std::exchange(other.size_, 0);
	}
	return *this;
}

ShmSegment ShmSegment::create(const std::string& name, std::size_t size, mode_t mode) {
	int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, mode);
	if (fd < 0)
		throw_errno("shm_open(create) " + name);
	// shm_open's mode goes through the umask: with the common 022, the
	// 0660 the control segment needs for group subscribers silently became
	// 0640, and a subscriber in the group could not open it at all. The
	// file mode is part of the security boundary here, so set it exactly.
	if (fchmod(fd, mode) != 0) {
		int saved = errno;
		close(fd);
		shm_unlink(name.c_str());
		errno = saved;
		throw_errno("fchmod " + name);
	}
	if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
		int saved = errno;
		close(fd);
		shm_unlink(name.c_str());
		errno = saved;
		throw_errno("ftruncate " + name);
	}
	ShmSegment seg;
	try {
		seg.data_ = map(fd, size, name);
	} catch (...) {
		close(fd);
		shm_unlink(name.c_str());
		throw;
	}
	close(fd); // the mapping keeps the object alive
	seg.name_ = name;
	seg.size_ = size;
	return seg;
}

ShmSegment ShmSegment::open(const std::string& name, bool writable) {
	int fd = shm_open(name.c_str(), writable ? O_RDWR : O_RDONLY, 0);
	if (fd < 0)
		throw_errno("shm_open(open) " + name);
	struct stat st{};
	if (fstat(fd, &st) != 0) {
		close(fd);
		throw_errno("fstat " + name);
	}
	ShmSegment seg;
	try {
		seg.data_ = map(fd, static_cast<std::size_t>(st.st_size), name, writable);
	} catch (...) {
		close(fd);
		throw;
	}
	close(fd);
	seg.name_ = name;
	seg.size_ = static_cast<std::size_t>(st.st_size);
	return seg;
}

void ShmSegment::unlink(const std::string& name) noexcept {
	shm_unlink(name.c_str());
}

bool ShmSegment::lock() noexcept {
	return data_ && mlock(data_, size_) == 0;
}

bool futex_wait(std::atomic<uint32_t>* word, uint32_t expected, std::chrono::nanoseconds timeout) noexcept {
	timespec ts{};
	timespec* tsp = nullptr;
	if (timeout.count() >= 0) {
		ts.tv_sec = static_cast<time_t>(timeout.count() / 1'000'000'000);
		ts.tv_nsec = static_cast<long>(timeout.count() % 1'000'000'000);
		tsp = &ts;
	}
	long rc = syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT, expected, tsp, nullptr, 0);
	return !(rc == -1 && errno == ETIMEDOUT);
}

void futex_wake_all(std::atomic<uint32_t>* word) noexcept {
	syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

int pidfd_open(int pid) noexcept {
	return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
}

bool pidfd_alive(int pidfd) noexcept {
	if (pidfd < 0)
		return false;
	pollfd p{pidfd, POLLIN, 0};
	// A pidfd becomes readable when its process exits.
	return poll(&p, 1, 0) == 0;
}

bool process_alive(int pid) noexcept {
	if (pid <= 0)
		return false;
	int fd = pidfd_open(pid);
	if (fd < 0)
		return errno != ESRCH;
	bool alive = pidfd_alive(fd);
	close(fd);
	return alive;
}

} // namespace devbus::detail
