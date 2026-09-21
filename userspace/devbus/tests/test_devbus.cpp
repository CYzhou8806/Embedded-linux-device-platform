#include "devbus/devbus.hpp"
#include "devbus/introspect.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace devbus;
using namespace std::chrono_literals;

namespace {

struct Msg {
	uint64_t value;
	uint32_t tag;
};

struct Other {
	uint64_t value;
	uint32_t tag;
};

// Unique per test and per test process, so tests never see each other's segments.
std::string service_name() {
	const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
	return std::string("test/") + info->test_suite_name() + "." + info->name() + "." + std::to_string(getpid());
}

void send_value(Publisher<Msg>& pub, uint64_t v) {
	auto loan = pub.loan();
	ASSERT_TRUE(loan.has_value());
	(*loan)->value = v;
	(*loan)->tag = 0;
	pub.send(std::move(*loan));
}

std::vector<uint64_t> drain(Subscriber<Msg>& sub) {
	std::vector<uint64_t> out;
	while (auto s = sub.receive())
		out.push_back((*s)->value);
	return out;
}

} // namespace

TEST(Devbus, SubscriberSeesPublisherMemoryInPlace) {
	auto pub = Publisher<Msg>::create(service_name());
	Subscriber<Msg> sub(service_name());

	auto loan = pub.loan();
	ASSERT_TRUE(loan);
	(*loan)->value = 42;
	(*loan)->tag = 7;
	SendReport r = pub.send(std::move(*loan));
	EXPECT_EQ(r.seq, 1u);
	EXPECT_EQ(r.delivered, 1u);

	auto s = sub.receive();
	ASSERT_TRUE(s);
	EXPECT_EQ((*s)->value, 42u);
	EXPECT_EQ((*s)->tag, 7u);
	EXPECT_EQ(s->seq(), 1u);
	EXPECT_GT(s->publish_ns(), 0);
}

TEST(Devbus, LateSubscriberStartsFromNowAndEmptyIsEmpty) {
	auto pub = Publisher<Msg>::create(service_name());
	send_value(pub, 1); // nobody listening: chunk goes straight back to the pool
	EXPECT_EQ(pub.free_chunks(), pub.chunk_count());

	Subscriber<Msg> sub(service_name());
	ReceiveStatus st;
	EXPECT_FALSE(sub.receive(&st));
	EXPECT_EQ(st, ReceiveStatus::Empty);
	send_value(pub, 2);
	EXPECT_EQ(drain(sub), std::vector<uint64_t>{2});
}

TEST(Devbus, PayloadTypeMismatchIsRejected) {
	auto pub = Publisher<Msg>::create(service_name());
	EXPECT_THROW(Subscriber<Other> sub(service_name()), Error);
}

TEST(Devbus, SecondLivePublisherIsRejected) {
	auto pub = Publisher<Msg>::create(service_name());
	EXPECT_THROW(Publisher<Msg>::create(service_name()), Error);
}

TEST(Devbus, NoPublisherMeansNoSubscriber) {
	EXPECT_THROW(Subscriber<Msg> sub(service_name()), Error);
}

TEST(Devbus, DropOldestKeepsFreshestAndCountsBothWays) {
	auto pub = Publisher<Msg>::create(service_name(), {.queue_capacity = 4});
	Subscriber<Msg> sub(service_name(), {.overflow = Overflow::DropOldest});
	for (uint64_t v = 1; v <= 10; ++v)
		send_value(pub, v);
	EXPECT_EQ(drain(sub), (std::vector<uint64_t>{7, 8, 9, 10}));

	send_value(pub, 11);
	EXPECT_EQ(drain(sub), std::vector<uint64_t>{11});
	// 1..6 lost before the first receive -> not a "gap" between receipts
	EXPECT_EQ(sub.observed_gaps(), 0u);
	auto info = list_services();
	bool found = false;
	for (const auto& s : info)
		if (s.shm_name.find(std::to_string(getpid())) != std::string::npos &&
		    s.shm_name.find("DropOldest") != std::string::npos) {
			ASSERT_EQ(s.subscribers.size(), 1u);
			EXPECT_EQ(s.subscribers[0].dropped_oldest, 6u);
			found = true;
		}
	EXPECT_TRUE(found);
}

TEST(Devbus, DropNewestKeepsOldestHistory) {
	auto pub = Publisher<Msg>::create(service_name(), {.queue_capacity = 4});
	Subscriber<Msg> sub(service_name(), {.overflow = Overflow::DropNewest});
	for (uint64_t v = 1; v <= 10; ++v)
		send_value(pub, v);
	EXPECT_EQ(drain(sub), (std::vector<uint64_t>{1, 2, 3, 4}));
	send_value(pub, 11);
	EXPECT_EQ(drain(sub), std::vector<uint64_t>{11});
	EXPECT_EQ(sub.observed_gaps(), 6u); // 5..10 missing between 4 and 11
}

TEST(Devbus, BlockTimesOutInsteadOfHangingForever) {
	auto pub = Publisher<Msg>::create(service_name(), {.queue_capacity = 2});
	Subscriber<Msg> sub(service_name(), {.overflow = Overflow::Block, .block_timeout = 2000us});
	send_value(pub, 1);
	send_value(pub, 2);
	auto t0 = std::chrono::steady_clock::now();
	auto loan = pub.loan();
	ASSERT_TRUE(loan);
	SendReport r = pub.send(std::move(*loan));
	auto waited = std::chrono::steady_clock::now() - t0;
	EXPECT_EQ(r.block_timeouts, 1u);
	EXPECT_EQ(r.delivered, 0u);
	EXPECT_GE(waited, 2ms);
	EXPECT_LT(waited, 200ms);
}

TEST(Devbus, BorrowLimitIsEnforced) {
	auto pub = Publisher<Msg>::create(service_name(), {.max_borrowed = 2});
	Subscriber<Msg> sub(service_name());
	for (uint64_t v = 1; v <= 3; ++v)
		send_value(pub, v);
	auto a = sub.receive();
	auto b = sub.receive();
	ASSERT_TRUE(a && b);
	ReceiveStatus st;
	EXPECT_FALSE(sub.receive(&st));
	EXPECT_EQ(st, ReceiveStatus::BorrowLimit);
	a.reset();
	EXPECT_TRUE(sub.receive());
}

TEST(Devbus, LoanLimitIsEnforced) {
	auto pub = Publisher<Msg>::create(service_name(), {.max_loans = 1});
	auto first = pub.loan();
	ASSERT_TRUE(first);
	LoanError err;
	EXPECT_FALSE(pub.loan(&err));
	EXPECT_EQ(err, LoanError::LoanLimit);
}

TEST(Devbus, FutexWaitWakesOnSendAndTimesOut) {
	auto pub = Publisher<Msg>::create(service_name());
	Subscriber<Msg> sub(service_name(), {.wait_mode = WaitMode::Futex});
	auto t0 = std::chrono::steady_clock::now();
	EXPECT_FALSE(sub.wait(20ms));
	EXPECT_GE(std::chrono::steady_clock::now() - t0, 20ms);

	std::thread producer([&] {
		std::this_thread::sleep_for(20ms);
		send_value(pub, 5);
	});
	EXPECT_TRUE(sub.wait(2s));
	producer.join();
	EXPECT_EQ(drain(sub), std::vector<uint64_t>{5});
}

TEST(Devbus, PublisherShutdownWakesWaitingSubscriber) {
	auto pub = std::make_unique<Publisher<Msg>>(Publisher<Msg>::create(service_name()));
	Subscriber<Msg> sub(service_name());
	std::thread killer([&] {
		std::this_thread::sleep_for(20ms);
		pub.reset();
	});
	auto t0 = std::chrono::steady_clock::now();
	EXPECT_FALSE(sub.wait(5s));
	EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
	killer.join();
	EXPECT_FALSE(sub.publisher_alive());
	ReceiveStatus st;
	sub.receive(&st);
	EXPECT_EQ(st, ReceiveStatus::PublisherGone);
}

// A subscriber process that dies while holding samples and with a full
// queue must not leak chunks: the publisher reclaims all of them.
TEST(Devbus, CrashedSubscriberChunksAreReclaimed) {
	const std::string name = service_name();
	auto pub = Publisher<Msg>::create(name, {.max_subscribers = 1, .queue_capacity = 4, .max_borrowed = 2,
						 .liveness_check_every = 0});
	int ready[2];
	ASSERT_EQ(pipe(ready), 0);
	pid_t child = fork();
	if (child == 0) {
		Subscriber<Msg> sub(name);
		char c = 1;
		(void)!write(ready[1], &c, 1);
		for (;;) {
			// hold two samples forever, never return them
			static std::optional<Sample<Msg>> held[2];
			for (auto& h : held)
				if (!h)
					if (auto s = sub.receive())
						h.emplace(std::move(*s));
			usleep(1000);
		}
	}
	char c;
	ASSERT_EQ(read(ready[0], &c, 1), 1);
	for (uint64_t v = 1; v <= 20; ++v)
		send_value(pub, v); // fills the child's queue and its borrowed slots
	std::this_thread::sleep_for(50ms);
	EXPECT_LT(pub.free_chunks(), pub.chunk_count());

	kill(child, SIGKILL);
	waitpid(child, nullptr, 0);
	EXPECT_EQ(pub.check_liveness(), 1u);
	EXPECT_EQ(pub.free_chunks(), pub.chunk_count());
	EXPECT_EQ(pub.active_subscribers(), 0u);

	// the slot is reusable
	Subscriber<Msg> again(name);
	send_value(pub, 99);
	EXPECT_EQ(drain(again), std::vector<uint64_t>{99});
}

// A publisher that crashed leaves its segment behind; the next one takes over.
TEST(Devbus, StaleSegmentFromCrashedPublisherIsReplaced) {
	const std::string name = service_name();
	pid_t child = fork();
	if (child == 0) {
		auto pub = Publisher<Msg>::create(name);
		kill(getpid(), SIGKILL); // no destructor, segment stays in /dev/shm
	}
	waitpid(child, nullptr, 0);
	auto pub = Publisher<Msg>::create(name);
	Subscriber<Msg> sub(name);
	send_value(pub, 3);
	EXPECT_EQ(drain(sub), std::vector<uint64_t>{3});
}

// The chunk budget invariant under concurrency: with DropOldest, loan()
// never fails no matter how slow the subscribers are, the Block subscriber
// gets everything in order, and a slow lossy subscriber's losses reconcile
// exactly - between its own gap count and the publisher's counters in
// shared memory, two numbers computed in different threads from different
// information.
TEST(Devbus, StressLoanNeverFailsAndDropsReconcile) {
	const std::string name = service_name();
	constexpr uint64_t kCount = 200000;
	auto pub = Publisher<Msg>::create(name, {.max_subscribers = 3, .queue_capacity = 8, .max_borrowed = 2});
	std::atomic<bool> done{false};
	std::atomic<int> joined{0};
	uint64_t fast_received = 0, fast_gaps = 0, fast_order_errors = 0;
	uint64_t slow_received = 0, slow_gaps = 0, slow_first = 0, slow_last = 0;
	SubscriberInfo slow_info;

	std::thread fast([&] {
		Subscriber<Msg> sub(name, {.overflow = Overflow::Block, .block_timeout = 1s});
		++joined;
		uint64_t prev = 0;
		while (!done || sub.wait(0ms)) {
			sub.wait(10ms);
			while (auto s = sub.receive()) {
				if (prev != 0 && s->seq() != prev + 1)
					++fast_order_errors;
				prev = s->seq();
			}
		}
		fast_received = sub.received();
		fast_gaps = sub.observed_gaps();
	});
	std::thread slow([&] {
		Subscriber<Msg> sub(name, {.overflow = Overflow::DropOldest});
		++joined;
		while (!done || sub.wait(0ms)) {
			sub.wait(10ms);
			auto a = sub.receive(); // hold two at once: exercises the borrow budget
			auto b = sub.receive();
			for (auto* s : {&a, &b})
				if (*s) {
					if (slow_first == 0)
						slow_first = (*s)->seq();
					slow_last = (*s)->seq();
				}
			std::this_thread::sleep_for(20us);
		}
		slow_received = sub.received();
		slow_gaps = sub.observed_gaps();
		// read the publisher-side counters before this subscriber closes (closing resets its slot)
		for (const auto& svc : list_services())
			if (svc.shm_name == detail::shm_name(name))
				for (const auto& info : svc.subscribers)
					if (info.overflow == "drop-oldest")
						slow_info = info;
	});
	while (joined < 2)
		std::this_thread::sleep_for(1ms);

	uint64_t loan_failures = 0;
	for (uint64_t v = 1; v <= kCount; ++v) {
		auto l = pub.loan();
		if (!l) {
			++loan_failures;
			continue;
		}
		(*l)->value = v;
		pub.send(std::move(*l));
	}
	done = true;
	fast.join();
	slow.join();

	EXPECT_EQ(loan_failures, 0u);
	EXPECT_EQ(fast_received, kCount);
	EXPECT_EQ(fast_gaps, 0u);
	EXPECT_EQ(fast_order_errors, 0u);

	EXPECT_GT(slow_info.dropped_oldest, 0u); // it really was too slow
	EXPECT_EQ(slow_received + slow_gaps, slow_last - slow_first + 1);
	EXPECT_EQ(slow_info.delivered, slow_received + slow_info.dropped_oldest);
	EXPECT_EQ(slow_info.queued, 0u);

	pub.check_liveness();
	EXPECT_EQ(pub.free_chunks(), pub.chunk_count());
}

// Queue pressure is the leading congestion signal: it has to rise while
// the queue is still absorbing everything and no drop counter has moved,
// otherwise it is no better than the drop counters it is meant to precede.
TEST(Devbus, PressureRisesBeforeAnythingIsDropped) {
	const std::string svc = service_name();
	auto pub = Publisher<Msg>::create(svc, {.max_subscribers = 2, .queue_capacity = 8});
	EXPECT_EQ(pub.max_queued(), 0u);
	EXPECT_FLOAT_EQ(pub.pressure(), 0.0f);

	Subscriber<Msg> sub(svc, {.overflow = Overflow::DropOldest});
	// A subscriber that never receives: each send leaves one more in its queue.
	for (uint32_t i = 1; i <= 4; ++i) {
		send_value(pub, i);
		EXPECT_EQ(pub.max_queued(), i);
		EXPECT_FLOAT_EQ(pub.pressure(), static_cast<float>(i) / 8.0f);
	}

	// Still nothing lost at this point - that is the whole point of the signal.
	auto before = list_services();
	for (const auto& s : before)
		if (s.shm_name.find(svc.substr(svc.find('/') + 1)) != std::string::npos)
			for (const auto& si : s.subscribers)
				EXPECT_EQ(si.dropped_oldest + si.dropped_newest, 0u);

	// Drain it and the signal falls back.
	while (auto sample = sub.receive())
		(void)**sample;
	send_value(pub, 99); // one send for the publisher to observe the drained tail
	EXPECT_LE(pub.max_queued(), 1u);

	// A queue that is actually full reads as full, not as an overflowed value.
	Subscriber<Msg> slow(svc, {.overflow = Overflow::DropOldest});
	for (uint32_t i = 0; i < 20; ++i)
		send_value(pub, i);
	EXPECT_EQ(pub.max_queued(), 8u);
	EXPECT_FLOAT_EQ(pub.pressure(), 1.0f);
}
