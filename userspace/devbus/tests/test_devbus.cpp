#include "devbus/devbus.hpp"
#include "devbus/introspect.hpp"
#include "devbus/record.hpp"
#include "devbus/detail/layout.hpp"
#include "devbus/detail/os.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/stat.h>
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

// Threat model F13: a subscriber must not be able to change a payload other
// subscribers are reading. Since layout version 2 the payloads live in a
// separate segment that subscribers map PROT_READ, so a write through the
// pointer a Sample hands out - even with the const cast away - is a SIGSEGV.
TEST(DevbusDeathTest, SubscriberCannotWritePayloads) {
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	auto pub = Publisher<Msg>::create(service_name());
	Subscriber<Msg> sub(service_name());
	send_value(pub, 7);
	auto s = sub.receive();
	ASSERT_TRUE(s);
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
	// A sanitizer catches the SIGSEGV itself and exits with its own status
	// after printing "SEGV ... caused by a WRITE memory access".
	EXPECT_DEATH({ const_cast<Msg&>(**s).value = 666; std::_Exit(0); }, "SEGV");
#else
	EXPECT_EXIT({ const_cast<Msg&>(**s).value = 666; std::_Exit(0); }, ::testing::KilledBySignal(SIGSEGV), "");
#endif
	EXPECT_EQ((*s)->value, 7u);
}

// The publisher still writes in place: the split must not cost the
// zero-copy path anything.
TEST(Devbus, PublisherWritesInPlaceSubscriberReadsInPlace) {
	auto pub = Publisher<Msg>::create(service_name());
	Subscriber<Msg> a(service_name()), b(service_name());
	send_value(pub, 1234);
	auto sa = a.receive();
	auto sb = b.receive();
	ASSERT_TRUE(sa && sb);
	EXPECT_EQ(&**sa != nullptr, true);
	EXPECT_EQ((*sa)->value, 1234u);
	EXPECT_EQ((*sb)->value, 1234u);
}

// File modes are part of the boundary, so they must not depend on the
// creator's umask: before this, shm_open's 0660 became 0640 under the
// usual umask 022 and group subscribers couldn't open the segment at all.
TEST(Devbus, SegmentModesAreExactWhateverTheUmask) {
	for (mode_t mask : {mode_t{022}, mode_t{077}}) {
		const mode_t old = umask(mask);
		{
			auto pub = Publisher<Msg>::create(service_name());
			const std::string base = "/dev/shm" + detail::shm_name(service_name());
			struct stat ctl{}, dat{};
			ASSERT_EQ(stat(base.c_str(), &ctl), 0);
			ASSERT_EQ(stat((base + ".data").c_str(), &dat), 0);
			EXPECT_EQ(ctl.st_mode & 0777, 0660u) << "umask " << std::oct << mask;
			EXPECT_EQ(dat.st_mode & 0777, 0640u) << "umask " << std::oct << mask;
		}
		umask(old);
	}
}

TEST(Devbus, BothSegmentsAreRemovedWithThePublisher) {
	const std::string base = "/dev/shm" + detail::shm_name(service_name());
	{
		auto pub = Publisher<Msg>::create(service_name());
		EXPECT_EQ(access((base + ".data").c_str(), F_OK), 0);
	}
	EXPECT_NE(access(base.c_str(), F_OK), 0);
	EXPECT_NE(access((base + ".data").c_str(), F_OK), 0);
}

TEST(Devbus, IntrospectionListsAServiceOnceNotTwice) {
	auto pub = Publisher<Msg>::create(service_name());
	int seen = 0;
	for (const auto& s : list_services())
		if (s.shm_name == detail::shm_name(service_name()))
			++seen;
	EXPECT_EQ(seen, 1);
}

// --- record / replay ------------------------------------------------------

namespace {
std::string tmp_path(const char* what) {
	return "/tmp/devbus-test-" + std::to_string(getpid()) + "-" + what + ".rec";
}
} // namespace

TEST(DevbusRecord, RoundTripKeepsBytesOrderAndType) {
	const std::string path = tmp_path("roundtrip");
	std::remove(path.c_str());
	{
		auto pub = Publisher<Msg>::create(service_name());
		Recorder rec(service_name(), path);
		for (uint64_t v = 1; v <= 300; ++v) {
			auto loan = pub.loan();
			ASSERT_TRUE(loan);
			(*loan)->value = v * 7;
			(*loan)->tag = static_cast<uint32_t>(v);
			pub.send(std::move(*loan));
			if (v % 8 == 0)
				rec.poll(0ns);
		}
		rec.poll(0ns);
		EXPECT_EQ(rec.stats().samples, 300u);
		EXPECT_EQ(rec.stats().gaps, 0u);
	}

	Replayer rp(path);
	EXPECT_EQ(rp.recorded_service(), service_name());
	EXPECT_EQ(rp.type().hash, type_info_of<Msg>().hash);

	const std::string replay_name = service_name() + ".replay";
	std::vector<std::pair<uint64_t, uint32_t>> got;
	std::atomic<bool> ready{false};
	std::thread consumer([&] {
		// Retry until the replayer has created the service.
		std::unique_ptr<Subscriber<Msg>> sub;
		while (!sub) {
			try {
				sub = std::make_unique<Subscriber<Msg>>(replay_name,
					SubscriberConfig{.overflow = Overflow::Block, .block_timeout = 100ms});
			} catch (const Error&) {
				std::this_thread::sleep_for(1ms);
			}
		}
		ready = true;
		while (got.size() < 300 && sub->wait(2s))
			while (auto s = sub->receive())
				got.emplace_back((*s)->value, (*s)->tag);
	});
	const auto st = rp.run(replay_name, 0.0, {}, /*subscribers=*/1, 5s);
	consumer.join();
	EXPECT_EQ(st.sent, 300u);
	ASSERT_EQ(got.size(), 300u);
	for (uint64_t i = 0; i < 300; ++i) {
		EXPECT_EQ(got[i].first, (i + 1) * 7);
		EXPECT_EQ(got[i].second, i + 1);
	}
	std::remove(path.c_str());
}

TEST(DevbusRecord, ReplayKeepsTheRecordedTiming) {
	const std::string path = tmp_path("timing");
	std::remove(path.c_str());
	{
		auto pub = Publisher<Msg>::create(service_name());
		Recorder rec(service_name(), path);
		for (uint64_t v = 0; v < 100; ++v) { // 100 samples, 2 ms apart = ~200 ms
			send_value(pub, v);
			rec.poll(0ns);
			std::this_thread::sleep_for(2ms);
		}
		rec.poll(0ns);
	}
	Replayer rp(path);
	const auto t0 = std::chrono::steady_clock::now();
	const auto st = rp.run(service_name() + ".t", 1.0, {}, 0, 0ms);
	const auto took = std::chrono::steady_clock::now() - t0;
	EXPECT_EQ(st.sent, 100u);
	EXPECT_GT(took, 180ms);
	EXPECT_LT(took, 400ms);
	EXPECT_LT(st.mean_lateness_us, 200.0) << "max " << st.max_lateness_us;

	Replayer fast(path);
	const auto t1 = std::chrono::steady_clock::now();
	fast.run(service_name() + ".f", 0.0, {}, 0, 0ms);
	EXPECT_LT(std::chrono::steady_clock::now() - t1, 50ms);
	std::remove(path.c_str());
}

TEST(DevbusRecord, ReplayedServiceRejectsTheWrongType) {
	const std::string path = tmp_path("type");
	std::remove(path.c_str());
	{
		auto pub = Publisher<Msg>::create(service_name());
		Recorder rec(service_name(), path);
		send_value(pub, 1);
		rec.poll(10ms);
	}
	Replayer rp(path);
	const std::string name = service_name() + ".r";
	std::atomic<bool> checked{false};
	std::thread t([&] {
		for (int i = 0; i < 2000 && !checked; ++i) {
			try {
				Subscriber<Other> wrong(name);
				ADD_FAILURE() << "a Subscriber<Other> opened a replay of Msg";
				checked = true;
			} catch (const Error& e) {
				if (std::string(e.what()).find("type mismatch") != std::string::npos)
					checked = true;
				else
					std::this_thread::sleep_for(1ms);
			}
		}
	});
	// Wait for a subscriber that never comes (it has the wrong type), which
	// keeps the replayed service up long enough to be tried.
	rp.run(name, 0.0, {}, 1, 1s);
	t.join();
	EXPECT_TRUE(checked);
	std::remove(path.c_str());
}

TEST(DevbusRecord, RefusesToOverwriteAndRejectsGarbage) {
	const std::string path = tmp_path("garbage");
	{
		FILE* f = std::fopen(path.c_str(), "w");
		std::fputs("not a recording", f);
		std::fclose(f);
	}
	EXPECT_THROW(Replayer{path}, Error);
	auto pub = Publisher<Msg>::create(service_name());
	EXPECT_THROW(Recorder(service_name(), path), Error); // exists: not overwritten
	std::remove(path.c_str());
}

// Roadmap 6: a Block publisher used to spin for the whole wait. It now
// spins for 20 us and then sleeps on a futex the subscriber signals, so
// waiting on a slow subscriber costs (almost) no CPU - and the subscriber
// still gets every sample, in order.
TEST(Devbus, BlockedPublisherSleepsInsteadOfSpinning) {
	auto pub = Publisher<Msg>::create(service_name(), ServiceConfig{.queue_capacity = 2});
	Subscriber<Msg> sub(service_name(), SubscriberConfig{.overflow = Overflow::Block, .block_timeout = 2000ms});
	std::atomic<bool> stop{false};
	std::vector<uint64_t> got;
	std::thread slow([&] {
		while (!stop || got.size() < 40) {
			if (auto s = sub.receive()) {
				got.push_back((*s)->value);
				std::this_thread::sleep_for(5ms); // a slow consumer
			} else if (stop && got.size() >= 40) {
				break;
			} else {
				sub.wait(10ms);
			}
		}
	});
	timespec c0{}, c1{};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
	const auto t0 = std::chrono::steady_clock::now();
	SendReport total;
	for (uint64_t v = 0; v < 40; ++v) {
		auto loan = pub.loan();
		ASSERT_TRUE(loan);
		(*loan)->value = v;
		total.block_timeouts += pub.send(std::move(*loan)).block_timeouts;
	}
	const auto wall = std::chrono::steady_clock::now() - t0;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
	stop = true;
	slow.join();
	const double cpu_ms = (c1.tv_sec - c0.tv_sec) * 1e3 + (c1.tv_nsec - c0.tv_nsec) / 1e6;
	const double wall_ms = std::chrono::duration<double, std::milli>(wall).count();
	EXPECT_EQ(total.block_timeouts, 0u);
	EXPECT_GT(wall_ms, 150.0); // it really was held back by the slow consumer
	EXPECT_LT(cpu_ms, wall_ms * 0.2) << "publisher burned " << cpu_ms << " ms CPU in " << wall_ms << " ms";
	ASSERT_EQ(got.size(), 40u);
	for (uint64_t i = 0; i < 40; ++i)
		EXPECT_EQ(got[i], i);
}

TEST(Devbus, BlockedPublisherIsReleasedWhenTheSubscriberLeaves) {
	auto pub = Publisher<Msg>::create(service_name(), ServiceConfig{.queue_capacity = 1});
	auto sub = std::make_unique<Subscriber<Msg>>(service_name(),
		SubscriberConfig{.overflow = Overflow::Block, .block_timeout = 5000ms});
	send_value(pub, 1); // fills the queue
	std::thread leaver([&] {
		std::this_thread::sleep_for(50ms);
		sub.reset(); // leaves without reading
	});
	const auto t0 = std::chrono::steady_clock::now();
	auto loan = pub.loan();
	ASSERT_TRUE(loan);
	pub.send(std::move(*loan)); // blocks until the subscriber is gone, not 5 s
	EXPECT_LT(std::chrono::steady_clock::now() - t0, 1000ms);
	leaver.join();
}

// The onboarding race: a subscriber claims a slot (Free -> Claimed) and only
// then writes its pid. A liveness check in between used to see pid 0, call
// that a dead process and reclaim the slot under the subscriber, which then
// marked it Active and never received a sample. Reproduced here by doing
// the claim by hand and stopping right where the race window is.
TEST(Devbus, SlotBeingClaimedIsNotReapedAsDead) {
	auto pub = Publisher<Msg>::create(service_name());
	detail::ShmSegment ctl = detail::ShmSegment::open(detail::shm_name(service_name()));
	auto* h = reinterpret_cast<detail::SegmentHeader*>(ctl.data());
	auto* sl = reinterpret_cast<detail::SubscriberSlot*>(ctl.data() + h->slots_offset);
	uint32_t expected = static_cast<uint32_t>(detail::SlotState::Free);
	ASSERT_TRUE(sl->state.compare_exchange_strong(expected, static_cast<uint32_t>(detail::SlotState::Claimed)));
	ASSERT_EQ(sl->pid.load(), 0);

	EXPECT_EQ(pub.check_liveness(), 0u); // mid-claim: left alone
	EXPECT_EQ(sl->state.load(), static_cast<uint32_t>(detail::SlotState::Claimed));

	// The subscriber finishes the claim; the publisher onboards and delivers.
	sl->pid.store(getpid());
	sl->state.store(static_cast<uint32_t>(detail::SlotState::Active));
	pub.check_liveness();
	EXPECT_EQ(pub.active_subscribers(), 1u);
	send_value(pub, 9);
	EXPECT_EQ(sl->data_head.load(), 1u);
}

TEST(Devbus, SlotAbandonedMidClaimIsReclaimedAfterAGracePeriod) {
	auto pub = Publisher<Msg>::create(service_name(), ServiceConfig{.max_subscribers = 1});
	{
		detail::ShmSegment ctl = detail::ShmSegment::open(detail::shm_name(service_name()));
		auto* h = reinterpret_cast<detail::SegmentHeader*>(ctl.data());
		auto* sl = reinterpret_cast<detail::SubscriberSlot*>(ctl.data() + h->slots_offset);
		uint32_t expected = static_cast<uint32_t>(detail::SlotState::Free);
		ASSERT_TRUE(sl->state.compare_exchange_strong(expected, static_cast<uint32_t>(detail::SlotState::Claimed)));
	} // "died" between the claim and the pid store
	pub.check_liveness();
	EXPECT_THROW(Subscriber<Msg>{service_name()}, Error); // the only slot is still held
	std::this_thread::sleep_for(1100ms);
	EXPECT_EQ(pub.check_liveness(), 1u);
	EXPECT_NO_THROW(Subscriber<Msg>{service_name()});
}
