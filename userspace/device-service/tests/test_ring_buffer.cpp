#include "ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using acq::RingBuffer;

TEST(RingBuffer, SingleThreadedPushPopPreservesOrder) {
	RingBuffer<int> buf(4);
	buf.push(1);
	buf.push(2);
	buf.push(3);
	EXPECT_EQ(buf.size(), 3u);

	int out = 0;
	ASSERT_TRUE(buf.pop(out));
	EXPECT_EQ(out, 1);
	ASSERT_TRUE(buf.pop(out));
	EXPECT_EQ(out, 2);
	ASSERT_TRUE(buf.pop(out));
	EXPECT_EQ(out, 3);
	EXPECT_EQ(buf.size(), 0u);
}

TEST(RingBuffer, ProducerConsumerThreadsDeliverEveryItemExactlyOnce) {
	constexpr int kCount = 20000;
	RingBuffer<int> buf(64); // deliberately small, forces push() to block on a full buffer

	std::thread producer([&] {
		for (int i = 0; i < kCount; ++i)
			buf.push(i);
	});

	std::vector<int> received;
	received.reserve(kCount);
	std::thread consumer([&] {
		int v;
		while (buf.pop(v))
			received.push_back(v);
	});

	producer.join();
	buf.stop(); // no more items coming - let the consumer's pop() drain and return
	consumer.join();

	ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
	for (int i = 0; i < kCount; ++i)
		EXPECT_EQ(received[i], i) << "at index " << i;
}

TEST(RingBuffer, StopUnblocksAWaitingPopOnAnEmptyBuffer) {
	RingBuffer<int> buf(4);
	std::atomic<bool> pop_returned{false};

	std::thread consumer([&] {
		int v;
		buf.pop(v); // buffer is empty - this blocks until stop()
		pop_returned = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(pop_returned) << "pop() returned before stop() was ever called";

	buf.stop();
	consumer.join();
	EXPECT_TRUE(pop_returned);
}

TEST(RingBuffer, StopUnblocksAWaitingPushOnAFullBuffer) {
	RingBuffer<int> buf(1);
	buf.push(1); // fill it - capacity is 1
	std::atomic<bool> push_returned{false};

	std::thread producer([&] {
		buf.push(2); // buffer is full - this blocks until stop()
		push_returned = true;
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_FALSE(push_returned) << "push() returned before stop() was ever called";

	buf.stop();
	producer.join();
	EXPECT_TRUE(push_returned);
}
