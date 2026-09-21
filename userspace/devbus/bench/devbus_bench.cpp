// devbus-bench: the experiments behind docs/devbus-experiments.md.
//
//   devbus-bench latency   --transport devbus-loan|devbus-copy|uds --size BYTES
//                          [--rate HZ] [--count N] [--wait spin|yield|futex]
//   devbus-bench queue     --impl mutex-cv|devbus-spin|devbus-futex [--count N] [--rate HZ]
//   devbus-bench isolation --slow-policy drop-oldest|drop-newest|block [--rate HZ] [--seconds S]
//
// Any command also takes the RT knobs --prio, --pub-cpu, --sub-cpu, --mlock
// and --dma-latency (see RtOptions).
//
// Every run prints one CSV row (prefixed by a "#" header row). Latency is
// one-way, measured with CLOCK_MONOTONIC on both ends - valid because both
// ends run on the same machine and read the same clock.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "devbus/devbus.hpp"
#include "devbus/introspect.hpp"
#include "ring_buffer.hpp" // device-service's existing mutex+condvar SPSC buffer, as the baseline

using namespace std::chrono_literals;

namespace {

using devbus::detail::monotonic_ns;

// ---------------------------------------------------------------- helpers

struct Args {
	std::map<std::string, std::string> kv;
	std::string get(const std::string& k, const std::string& def) const {
		auto it = kv.find(k);
		return it == kv.end() ? def : it->second;
	}
	long num(const std::string& k, long def) const { return std::stol(get(k, std::to_string(def))); }
};

Args parse(int argc, char** argv, int first) {
	Args a;
	for (int i = first; i + 1 < argc; i += 2)
		a.kv[std::string(argv[i]).substr(2)] = argv[i + 1];
	return a;
}

struct Stats {
	double p50, p90, p99, p999, max;
};

Stats percentiles(std::vector<int64_t> v) {
	if (v.empty())
		return {0, 0, 0, 0, 0};
	std::sort(v.begin(), v.end());
	auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))] / 1000.0; };
	return {at(0.50), at(0.90), at(0.99), at(0.999), v.back() / 1000.0};
}

double cpu_seconds_self() {
	rusage ru{};
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

double cpu_seconds_thread() {
	rusage ru{};
	getrusage(RUSAGE_THREAD, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

// Absolute-deadline pacing: no drift accumulates from loop overhead.
class Pacer {
public:
	explicit Pacer(long rate_hz) : period_ns_(rate_hz > 0 ? 1'000'000'000 / rate_hz : 0) {
		clock_gettime(CLOCK_MONOTONIC, &next_);
	}
	void wait() {
		if (period_ns_ == 0)
			return;
		next_.tv_nsec += period_ns_;
		while (next_.tv_nsec >= 1'000'000'000) {
			next_.tv_nsec -= 1'000'000'000;
			++next_.tv_sec;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_, nullptr) == EINTR) {
		}
	}

private:
	long period_ns_;
	timespec next_{};
};

devbus::WaitMode wait_mode(const std::string& s) {
	if (s == "spin")
		return devbus::WaitMode::BusySpin;
	if (s == "yield")
		return devbus::WaitMode::Yield;
	return devbus::WaitMode::Futex;
}

bool write_all(int fd, const void* buf, std::size_t n) {
	auto* p = static_cast<const char*>(buf);
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += w;
		n -= static_cast<std::size_t>(w);
	}
	return true;
}

bool read_all(int fd, void* buf, std::size_t n) {
	auto* p = static_cast<char*>(buf);
	while (n) {
		ssize_t r = read(fd, p, n);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			return false;
		}
		p += r;
		n -= static_cast<std::size_t>(r);
	}
	return true;
}

std::string service(const char* what) {
	return std::string("bench/") + what + "." + std::to_string(getpid());
}

// ---------------------------------------------------------------- RT tuning knobs
//
// The same knobs device-service already exposes (Plan.md V7 matrix), applied
// per role so publisher and subscriber can sit on different isolated cores:
//   --prio N        SCHED_FIFO priority N for both roles (0 = SCHED_OTHER)
//   --pub-cpu C     pin the publisher to CPU C   (-1 = no pinning)
//   --sub-cpu C     pin the (critical) subscriber to CPU C
//   --mlock 1       mlockall(MCL_CURRENT | MCL_FUTURE) in both roles
//   --dma-latency N hold /dev/cpu_dma_latency at N us for the whole run
//                   (0 = forbid deep C-states, the classic RT knob)
struct RtOptions {
	int prio = 0;
	int pub_cpu = -1;
	int sub_cpu = -1;
	bool mlock = false;
	int dma_latency = -1;
};
RtOptions g_rt;
int g_dma_fd = -1;

enum class Role { Publisher, Subscriber };

void apply_rt(Role role) {
	const int cpu = role == Role::Publisher ? g_rt.pub_cpu : g_rt.sub_cpu;
	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0)
			std::fprintf(stderr, "sched_setaffinity(%d): %s\n", cpu, std::strerror(errno));
	}
	if (g_rt.prio > 0) {
		sched_param sp{};
		sp.sched_priority = g_rt.prio;
		if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
			std::fprintf(stderr, "SCHED_FIFO %d: %s\n", g_rt.prio, std::strerror(errno));
	}
	if (g_rt.mlock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		std::fprintf(stderr, "mlockall: %s\n", std::strerror(errno));
}

void hold_dma_latency() {
	if (g_rt.dma_latency < 0)
		return;
	g_dma_fd = open("/dev/cpu_dma_latency", O_WRONLY);
	int32_t v = g_rt.dma_latency;
	if (g_dma_fd < 0 || write(g_dma_fd, &v, sizeof(v)) != sizeof(v))
		std::fprintf(stderr, "/dev/cpu_dma_latency: %s\n", std::strerror(errno));
	// kept open until exit: the constraint lasts as long as the fd does
}

// ---------------------------------------------------------------- latency

template <std::size_t N>
struct Payload {
	int64_t ts_ns; // -1 = end of run
	std::byte data[N - sizeof(int64_t)];
};

void print_latency_row(const std::string& transport, std::size_t size, const std::string& wait, long rate,
		       const std::vector<int64_t>& lat, double cpu_s, double wall_s) {
	Stats s = percentiles(lat);
	std::printf("latency,%s,%zu,%s,%ld,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f\n", transport.c_str(), size, wait.c_str(),
		    rate, lat.size(), s.p50, s.p90, s.p99, s.p999, s.max, 100.0 * cpu_s / wall_s);
	std::fflush(stdout);
}

template <std::size_t N>
int latency_devbus(bool zero_copy, const std::string& wait, long rate, long count) {
	using P = Payload<N>;
	const std::string name = service("latency");
	auto pub = devbus::Publisher<P>::create(name, {.max_subscribers = 1, .queue_capacity = 16, .max_borrowed = 1});
	int ready[2];
	if (pipe(ready) != 0)
		return 1;

	pid_t child = fork();
	if (child == 0) {
		apply_rt(Role::Subscriber);
		devbus::Subscriber<P> sub(name, {.overflow = devbus::Overflow::Block,
						 .block_timeout = std::chrono::microseconds(1'000'000),
						 .wait_mode = wait_mode(wait)});
		std::vector<int64_t> lat;
		lat.reserve(static_cast<std::size_t>(count));
		char c = 1;
		(void)!write(ready[1], &c, 1);
		const double cpu0 = cpu_seconds_self();
		const int64_t t0 = monotonic_ns();
		std::byte sink{};
		for (bool done = false; !done;) {
			sub.wait(std::chrono::milliseconds(-1));
			while (auto s = sub.receive()) {
				const int64_t now = monotonic_ns();
				if ((*s)->ts_ns < 0) {
					done = true;
					break;
				}
				sink ^= (*s)->data[sizeof((*s)->data) - 1]; // touch it where it lies
				lat.push_back(now - (*s)->ts_ns);
			}
		}
		const double wall = (monotonic_ns() - t0) / 1e9;
		asm volatile("" : : "r"(sink) : "memory"); // keep the payload reads
		print_latency_row(zero_copy ? "devbus-loan" : "devbus-copy", N, wait, rate, lat, cpu_seconds_self() - cpu0, wall);
		std::_Exit(0);
	}

	char c;
	if (read(ready[0], &c, 1) != 1)
		return 1;
	apply_rt(Role::Publisher);
	while (pub.active_subscribers() == 0) {
		pub.check_liveness();
		std::this_thread::sleep_for(1ms);
	}
	auto staging = std::make_unique<P>(); // for the copy variant: data produced in private memory first
	std::memset(staging->data, 0x5a, sizeof(staging->data));
	Pacer pacer(rate);
	for (long i = 0; i <= count; ++i) {
		pacer.wait();
		auto loan = pub.loan();
		if (!loan)
			continue;
		if (zero_copy) {
			// data is produced in place; only the timestamp is written at send time
			(*loan)->ts_ns = i == count ? -1 : monotonic_ns();
		} else {
			staging->ts_ns = i == count ? -1 : monotonic_ns();
			std::memcpy(&**loan, staging.get(), sizeof(P));
		}
		pub.send(std::move(*loan));
	}
	waitpid(child, nullptr, 0);
	return 0;
}

template <std::size_t N>
int latency_uds(long rate, long count) {
	using P = Payload<N>;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return 1;
	pid_t child = fork();
	if (child == 0) {
		apply_rt(Role::Subscriber);
		close(sv[0]);
		auto buf = std::make_unique<P>();
		std::vector<int64_t> lat;
		lat.reserve(static_cast<std::size_t>(count));
		const double cpu0 = cpu_seconds_self();
		const int64_t t0 = monotonic_ns();
		while (read_all(sv[1], buf.get(), sizeof(P))) {
			const int64_t now = monotonic_ns();
			if (buf->ts_ns < 0)
				break;
			lat.push_back(now - buf->ts_ns);
		}
		const double wall = (monotonic_ns() - t0) / 1e9;
		print_latency_row("uds", N, "blocking-read", rate, lat, cpu_seconds_self() - cpu0, wall);
		std::_Exit(0);
	}
	close(sv[1]);
	apply_rt(Role::Publisher);
	auto buf = std::make_unique<P>();
	std::memset(buf->data, 0x5a, sizeof(buf->data));
	std::this_thread::sleep_for(20ms);
	Pacer pacer(rate);
	for (long i = 0; i <= count; ++i) {
		pacer.wait();
		buf->ts_ns = i == count ? -1 : monotonic_ns();
		if (!write_all(sv[0], buf.get(), sizeof(P)))
			break;
	}
	waitpid(child, nullptr, 0);
	close(sv[0]);
	return 0;
}

template <std::size_t N>
int latency_dispatch(const std::string& transport, const std::string& wait, long rate, long count) {
	if (transport == "uds")
		return latency_uds<N>(rate, count);
	return latency_devbus<N>(transport == "devbus-loan", wait, rate, count);
}

int cmd_latency(const Args& a) {
	const std::string transport = a.get("transport", "devbus-loan");
	const std::string wait = a.get("wait", "futex");
	const long size = a.num("size", 64);
	const long rate = a.num("rate", 1000);
	const long count = a.num("count", 3000);
	switch (size) {
	case 64:
		return latency_dispatch<64>(transport, wait, rate, count);
	case 4096:
		return latency_dispatch<4096>(transport, wait, rate, count);
	case 65536:
		return latency_dispatch<65536>(transport, wait, rate, count);
	case 1048576:
		return latency_dispatch<1048576>(transport, wait, rate, count);
	case 4194304:
		return latency_dispatch<4194304>(transport, wait, rate, count);
	}
	std::fprintf(stderr, "size must be one of 64 4096 65536 1048576 4194304\n");
	return 2;
}

// ---------------------------------------------------------------- queue

// In-process, one producer thread, one consumer thread. Throughput mode
// (rate 0) pushes as fast as possible; paced mode measures latency.
int cmd_queue(const Args& a) {
	const std::string impl = a.get("impl", "mutex-cv");
	const long count = a.num("count", 2'000'000);
	const long rate = a.num("rate", 0);
	std::vector<int64_t> lat;
	lat.reserve(static_cast<std::size_t>(count));
	double consumer_cpu = 0;
	const int64_t t0 = monotonic_ns();
	apply_rt(Role::Publisher); // producer = this thread; consumer threads re-apply their own role

	if (impl == "mutex-cv") {
		acq::RingBuffer<int64_t> rb(16);
		std::thread consumer([&] {
			apply_rt(Role::Subscriber);
			const double c0 = cpu_seconds_thread();
			int64_t ts;
			while (rb.pop(ts)) {
				if (ts < 0)
					break;
				lat.push_back(monotonic_ns() - ts);
			}
			consumer_cpu = cpu_seconds_thread() - c0;
		});
		Pacer pacer(rate);
		for (long i = 0; i < count; ++i) {
			pacer.wait();
			rb.push(monotonic_ns());
		}
		rb.push(-1);
		consumer.join();
	} else {
		const std::string name = service("queue");
		auto pub = devbus::Publisher<int64_t>::create(name, {.max_subscribers = 1, .queue_capacity = 16, .max_borrowed = 1});
		std::atomic<bool> ready{false};
		std::thread consumer([&] {
			apply_rt(Role::Subscriber);
			devbus::Subscriber<int64_t> sub(name, {.overflow = devbus::Overflow::Block,
								.block_timeout = std::chrono::microseconds(1'000'000),
								.wait_mode = impl == "devbus-spin" ? devbus::WaitMode::BusySpin
												  : devbus::WaitMode::Futex});
			ready = true;
			const double c0 = cpu_seconds_thread();
			for (bool done = false; !done;) {
				sub.wait(std::chrono::milliseconds(-1));
				while (auto s = sub.receive()) {
					if (**s < 0) {
						done = true;
						break;
					}
					lat.push_back(monotonic_ns() - **s);
				}
			}
			consumer_cpu = cpu_seconds_thread() - c0;
		});
		while (!ready)
			std::this_thread::yield();
		pub.check_liveness();
		Pacer pacer(rate);
		for (long i = 0; i <= count; ++i) {
			pacer.wait();
			auto l = pub.loan();
			**l = i == count ? -1 : monotonic_ns();
			pub.send(std::move(*l));
		}
		consumer.join();
	}
	const double wall = (monotonic_ns() - t0) / 1e9;
	Stats s = percentiles(lat);
	std::printf("queue,%s,%ld,%zu,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f\n", impl.c_str(), rate, lat.size(),
		    lat.size() / wall, s.p50, s.p90, s.p99, s.p999, s.max, 100.0 * consumer_cpu / wall);
	return 0;
}

// ---------------------------------------------------------------- isolation

struct Tick {
	int64_t ts_ns; // -1 = end
	uint64_t n;
};

// One publisher, a fast "critical" subscriber (Block policy, i.e. it must
// see everything) and a slow one that needs 1 ms per sample. Only the slow
// subscriber's policy changes between runs. Question: how much does the
// slow one hurt the publisher and the fast one?
int cmd_isolation(const Args& a) {
	const std::string policy = a.get("slow-policy", "drop-oldest");
	const long rate = a.num("rate", 5000);
	const long seconds = a.num("seconds", 3);
	const long count = rate * seconds;
	const std::string name = service("isolation");
	auto pub = devbus::Publisher<Tick>::create(name, {.max_subscribers = 2, .queue_capacity = 64, .max_borrowed = 1});

	devbus::Overflow slow_overflow = policy == "block"         ? devbus::Overflow::Block
					 : policy == "drop-newest" ? devbus::Overflow::DropNewest
								   : devbus::Overflow::DropOldest;
	int pipes[2];
	if (pipe(pipes) != 0)
		return 1;

	auto spawn = [&](bool fast) {
		pid_t pid = fork();
		if (pid != 0)
			return pid;
		if (fast)
			apply_rt(Role::Subscriber);
		else if (g_rt.prio > 0) {
			sched_param sp{}; // the slow "best effort" consumer stays SCHED_OTHER
			sched_setscheduler(0, SCHED_OTHER, &sp);
		}
		devbus::Subscriber<Tick> sub(
			name, fast ? devbus::SubscriberConfig{.overflow = devbus::Overflow::Block,
							     .block_timeout = std::chrono::microseconds(1'000'000),
							     .wait_mode = devbus::WaitMode::Futex}
				   : devbus::SubscriberConfig{.overflow = slow_overflow,
							      .block_timeout = std::chrono::microseconds(5000),
							      .wait_mode = devbus::WaitMode::Futex});
		char c = 1;
		(void)!write(pipes[1], &c, 1);
		std::vector<int64_t> lat;
		lat.reserve(static_cast<std::size_t>(count));
		for (bool done = false; !done;) {
			if (!sub.wait(2s))
				break;
			while (auto s = sub.receive()) {
				if ((*s)->ts_ns < 0) {
					done = true;
					break;
				}
				lat.push_back(monotonic_ns() - (*s)->ts_ns);
				if (!fast)
					std::this_thread::sleep_for(1ms); // "processing"
			}
		}
		// Publisher-side counters for this subscriber, read before closing resets them.
		uint64_t dropped = 0;
		for (const auto& svc : devbus::list_services())
			if (svc.shm_name == devbus::detail::shm_name(name))
				for (const auto& info : svc.subscribers)
					if (info.pid == getpid())
						dropped = info.dropped_oldest + info.dropped_newest + info.block_timeouts;
		Stats s = percentiles(lat);
		std::printf("isolation-sub,%s,%s,%ld,%llu,%llu,%llu,%.2f,%.2f,%.2f\n", policy.c_str(), fast ? "fast" : "slow",
			    rate, static_cast<unsigned long long>(sub.received()),
			    static_cast<unsigned long long>(sub.observed_gaps()), static_cast<unsigned long long>(dropped),
			    s.p50, s.p99, s.max);
		std::fflush(stdout);
		std::_Exit(0);
	};
	pid_t fast = spawn(true);
	pid_t slow = spawn(false);
	char c;
	for (int i = 0; i < 2; ++i)
		if (read(pipes[0], &c, 1) != 1)
			return 1;
	apply_rt(Role::Publisher);
	while (pub.active_subscribers() < 2) {
		pub.check_liveness();
		std::this_thread::sleep_for(1ms);
	}

	std::vector<int64_t> send_ns;
	send_ns.reserve(static_cast<std::size_t>(count));
	Pacer pacer(rate);
	const int64_t t0 = monotonic_ns();
	for (long i = 0; i < count; ++i) {
		pacer.wait();
		const int64_t before = monotonic_ns();
		auto l = pub.loan();
		(*l)->ts_ns = before;
		(*l)->n = static_cast<uint64_t>(i);
		pub.send(std::move(*l));
		send_ns.push_back(monotonic_ns() - before);
	}
	const double wall = (monotonic_ns() - t0) / 1e9;
	// End marker for both; the Block-policy fast one is guaranteed to get it.
	for (int i = 0; i < 64 + 2; ++i) {
		auto l = pub.loan();
		(*l)->ts_ns = -1;
		pub.send(std::move(*l));
	}
	waitpid(fast, nullptr, 0);
	waitpid(slow, nullptr, 0);
	Stats s = percentiles(send_ns);
	std::printf("isolation-pub,%s,%ld,%.0f,%.2f,%.2f,%.2f\n", policy.c_str(), rate, count / wall, s.p50, s.p99, s.max);
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: devbus-bench latency|queue|isolation [--key value]...\n");
		return 2;
	}
	const std::string cmd = argv[1];
	const Args a = parse(argc, argv, 2);
	g_rt.prio = static_cast<int>(a.num("prio", 0));
	g_rt.pub_cpu = static_cast<int>(a.num("pub-cpu", -1));
	g_rt.sub_cpu = static_cast<int>(a.num("sub-cpu", -1));
	g_rt.mlock = a.num("mlock", 0) != 0;
	g_rt.dma_latency = static_cast<int>(a.num("dma-latency", -1));
	hold_dma_latency();
	// Headers are printed before any fork(); flush so children don't inherit
	// (and print again) a buffered copy.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if (cmd == "latency") {
		if (a.get("header", "0") != "0")
			std::printf("#latency,transport,size_bytes,wait,rate_hz,samples,p50_us,p90_us,p99_us,p999_us,max_us,sub_cpu_pct\n");
		if (a.get("header", "0") == "only")
			return 0;
		return cmd_latency(a);
	}
	if (cmd == "queue") {
		if (a.get("header", "0") != "0")
			std::printf("#queue,impl,rate_hz,samples,msgs_per_s,p50_us,p90_us,p99_us,p999_us,max_us,consumer_cpu_pct\n");
		if (a.get("header", "0") == "only")
			return 0;
		return cmd_queue(a);
	}
	if (cmd == "isolation") {
		if (a.get("header", "0") != "0") {
			std::printf("#isolation-sub,slow_policy,role,rate_hz,received,observed_gaps,publisher_side_drops,p50_us,p99_us,max_us\n");
			std::printf("#isolation-pub,slow_policy,rate_hz,achieved_hz,send_p50_us,send_p99_us,send_max_us\n");
		}
		if (a.get("header", "0") == "only")
			return 0;
		return cmd_isolation(a);
	}
	std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 2;
}
