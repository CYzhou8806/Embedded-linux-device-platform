// Subscribes to "acq/samples" and prints a summary once a second.
//
//   devbus-example-sub [drop-oldest|drop-newest|block] [work_us_per_sample]
//                      [--prio N] [--cpu C]
//
// Run two of them with different policies and a large work_us on one,
// then watch `devbus-ls -w 1`: the slow one drops, the fast one doesn't.
//
// The reported latency starts at the sample's `irq_ts_ns`, which the
// kernel driver stamps in its hard-IRQ handler, so it covers the whole
// path: hard IRQ -> threaded SPI read -> /dev/acq0 -> acq-bridge -> devbus
// -> this process.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#include <sched.h>
#include <sys/mman.h>

#include "devbus/acq_sample.hpp"
#include "devbus/devbus.hpp"

// AcqSample now lives in one place, shared by every program that
// publishes or consumes this service.

namespace {

void report(std::vector<int64_t>& lat, const devbus::Subscriber<AcqSample>& sub) {
	if (lat.empty()) {
		std::printf("rx 0/s  total %llu  gaps %llu\n", static_cast<unsigned long long>(sub.received()),
			    static_cast<unsigned long long>(sub.observed_gaps()));
		return;
	}
	std::sort(lat.begin(), lat.end());
	auto at = [&](double q) { return lat[std::min(lat.size() - 1, static_cast<std::size_t>(q * lat.size()))] / 1e3; };
	std::printf("rx %zu/s  total %llu  gaps %llu  latency p50 %.0f us  p99 %.0f us  max %.0f us\n", lat.size(),
		    static_cast<unsigned long long>(sub.received()), static_cast<unsigned long long>(sub.observed_gaps()),
		    at(0.5), at(0.99), lat.back() / 1e3);
	lat.clear();
}

} // namespace

int main(int argc, char** argv) {
	using namespace std::chrono_literals;
	// Line-buffered: this prints progress once a second, which is useless
	// if it sits in a 4 KiB buffer until the process is killed.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);

	devbus::Overflow policy = devbus::Overflow::DropOldest;
	long work_us = 0;
	int prio = 0, cpu = -1, positional = 0;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--prio") && i + 1 < argc)
			prio = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = std::atoi(argv[++i]);
		else if (positional++ == 0) {
			if (!std::strcmp(argv[i], "drop-newest"))
				policy = devbus::Overflow::DropNewest;
			else if (!std::strcmp(argv[i], "block"))
				policy = devbus::Overflow::Block;
		} else {
			work_us = std::atol(argv[i]);
		}
	}
	const auto work = std::chrono::microseconds(work_us);

	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		sched_setaffinity(0, sizeof(set), &set);
	}
	if (prio > 0) {
		sched_param sp{};
		sp.sched_priority = prio;
		if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
			std::perror("SCHED_FIFO");
	}
	mlockall(MCL_CURRENT | MCL_FUTURE);

	std::optional<devbus::Subscriber<AcqSample>> sub;
	while (!sub) {
		try {
			sub.emplace("acq/samples", devbus::SubscriberConfig{.overflow = policy});
		} catch (const devbus::Error& e) {
			std::printf("waiting for publisher: %s\n", e.what());
			std::this_thread::sleep_for(1s);
		}
	}

	std::vector<int64_t> lat;
	lat.reserve(4096);
	auto next_report = std::chrono::steady_clock::now() + 1s;
	while (sub->publisher_alive()) {
		if (!sub->wait(200ms) && std::chrono::steady_clock::now() >= next_report) {
			report(lat, *sub);
			next_report += 1s;
		}
		while (auto s = sub->receive()) {
			lat.push_back(devbus::detail::monotonic_ns() - (*s)->irq_ts_ns);
			if (work.count())
				std::this_thread::sleep_for(work);
			// Checked in here too: a slow consumer whose queue is
			// permanently full never leaves this loop, so a report only
			// outside it would never happen.
			if (std::chrono::steady_clock::now() >= next_report) {
				report(lat, *sub);
				next_report += 1s;
			}
		}
	}
	std::printf("publisher gone\n");
	return 0;
}
