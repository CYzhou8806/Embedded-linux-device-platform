#pragma once

// Thin Linux wrappers: shared-memory segments, futexes, pidfds, the clock.
// Kept in one place so the lock-free logic in publisher/subscriber reads
// as logic, not as syscall plumbing.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <sys/types.h>
#include <time.h>

namespace devbus::detail {

inline int64_t monotonic_ns() noexcept {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// "/devbus.<service>" - the name shm_open() wants, and what shows up
// under /dev/shm. The control segment (header, subscriber slots, rings).
std::string shm_name(std::string_view service);
// "/devbus.<service>.data" - chunk headers and payloads. Separate so that
// subscribers can map it read-only (threat model F13).
std::string shm_data_name(std::string_view service);

// Owns one mmap()ed shared-memory segment.
class ShmSegment {
public:
	ShmSegment() = default;
	~ShmSegment();
	ShmSegment(ShmSegment&& other) noexcept;
	ShmSegment& operator=(ShmSegment&& other) noexcept;
	ShmSegment(const ShmSegment&) = delete;
	ShmSegment& operator=(const ShmSegment&) = delete;

	// Creates a new segment (O_EXCL) of exactly `size` bytes, zero-filled
	// and pre-faulted, with file mode `mode`. Throws devbus::Error,
	// including when it already exists - stale-segment cleanup is the
	// caller's decision.
	static ShmSegment create(const std::string& name, std::size_t size, mode_t mode = 0660);
	// Opens an existing segment at whatever size it has. writable=false
	// opens it O_RDONLY and maps it PROT_READ: a write through the mapping
	// is a SIGSEGV, not a silent change other processes would see.
	static ShmSegment open(const std::string& name, bool writable = true);
	static void unlink(const std::string& name) noexcept;

	std::byte* data() const { return data_; }
	std::size_t size() const { return size_; }
	const std::string& name() const { return name_; }

	// Returns false (and leaves the segment unlocked) if mlock() is not permitted.
	bool lock() noexcept;

private:
	std::string name_;
	std::byte* data_ = nullptr;
	std::size_t size_ = 0;
};

// Process-shared futex (no FUTEX_PRIVATE_FLAG: waiter and waker are
// usually different processes mapping the same page).
// Returns false on timeout.
bool futex_wait(std::atomic<uint32_t>* word, uint32_t expected, std::chrono::nanoseconds timeout) noexcept;
void futex_wake_all(std::atomic<uint32_t>* word) noexcept;

// A pidfd refers to one specific process, not to a PID number that the
// kernel may reuse after that process dies - checking liveness through
// kill(pid, 0) could mistake an unrelated new process for the old one.
// Returns -1 if the process is already gone.
int pidfd_open(int pid) noexcept;
// True while the process behind the pidfd is running.
bool pidfd_alive(int pidfd) noexcept;
bool process_alive(int pid) noexcept;

} // namespace devbus::detail
