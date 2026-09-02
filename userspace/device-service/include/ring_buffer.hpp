#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace acq {

// Single-producer/single-consumer ring buffer. Not lock-free on purpose —
// correctness first, this project's actual latency-optimization work
// (Plan.md V7) happens with tracing tools on the real bottlenecks, not by
// guessing here. push()/pop() both block when the buffer is full/empty,
// mirroring the kernel driver's own /dev/acq0: it already carries a
// separate overflow signal (sysfs kfifo_overflow) for "consumer too slow",
// so this layer doesn't need to invent a second, different one by
// silently dropping samples.
template <typename T>
class RingBuffer {
public:
	explicit RingBuffer(std::size_t capacity) : buf_(capacity) {}

	void push(const T& item) {
		std::unique_lock<std::mutex> lock(mutex_);
		not_full_.wait(lock, [this] { return count_ < buf_.size() || stopped_; });
		if (stopped_)
			return;
		buf_[write_pos_] = item;
		write_pos_ = (write_pos_ + 1) % buf_.size();
		++count_;
		lock.unlock();
		not_empty_.notify_one();
	}

	// Returns false only if stop() was called and the buffer drained empty
	// (i.e. shutdown, not a spurious wakeup).
	bool pop(T& out) {
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock, [this] { return count_ > 0 || stopped_; });
		if (count_ == 0 && stopped_)
			return false;
		out = buf_[read_pos_];
		read_pos_ = (read_pos_ + 1) % buf_.size();
		--count_;
		lock.unlock();
		not_full_.notify_one();
		return true;
	}

	std::size_t size() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return count_;
	}

	// Wakes any thread blocked in push()/pop() so shutdown doesn't hang
	// waiting on a buffer that will never fill/drain again.
	void stop() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopped_ = true;
		}
		not_empty_.notify_all();
		not_full_.notify_all();
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable not_empty_;
	std::condition_variable not_full_;
	std::vector<T> buf_;
	std::size_t read_pos_ = 0;
	std::size_t write_pos_ = 0;
	std::size_t count_ = 0;
	bool stopped_ = false;
};

} // namespace acq
