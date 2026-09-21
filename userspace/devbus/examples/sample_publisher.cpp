// Publishes acquisition samples on "acq/samples" at a fixed rate - a
// stand-in for device-service until it publishes the real /dev/acq0 stream.
//
//   devbus-example-pub [rate_hz]
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "devbus/acq_sample.hpp"
#include "devbus/devbus.hpp"

// AcqSample now lives in one place, shared by every program that
// publishes or consumes this service.

namespace {
volatile std::sig_atomic_t stop = 0;
}

int main(int argc, char** argv) {
	// Line-buffered: these print progress once a second, and that is useless
	// if it sits in a 4 KiB buffer until the process is killed.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const long rate = argc > 1 ? std::atol(argv[1]) : 1000;
	std::signal(SIGINT, [](int) { stop = 1; });
	std::signal(SIGTERM, [](int) { stop = 1; });

	auto pub = devbus::Publisher<AcqSample>::create("acq/samples",
							 {.max_subscribers = 4, .queue_capacity = 256, .lock_memory = true});
	std::printf("publishing acq/samples at %ld Hz (memory %slocked)\n", rate, pub.memory_locked() ? "" : "NOT ");

	auto next = std::chrono::steady_clock::now();
	for (uint32_t seq = 0; !stop; ++seq) {
		next += std::chrono::nanoseconds(1'000'000'000 / rate);
		std::this_thread::sleep_until(next);
		auto loan = pub.loan();
		if (!loan)
			continue; // can't happen with the default chunk budget; see README
		(*loan)->seq = seq;
		(*loan)->value = seq * 7;
		(*loan)->irq_ts_ns = devbus::detail::monotonic_ns();
		pub.send(std::move(*loan));
	}
	std::printf("stopped\n");
	return 0;
}
