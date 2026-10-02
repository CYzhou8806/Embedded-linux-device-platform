// devbus-record SERVICE FILE [--seconds N] [--count N]
// Records a running service to FILE until Ctrl-C, N seconds, N samples, or
// the publisher goes away. Prints how many samples were recorded and how
// many it missed (a nonzero gap count means the disk fell behind for longer
// than the block timeout).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "devbus/record.hpp"

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: %s SERVICE FILE [--seconds N] [--count N]\n", argv[0]);
		return 2;
	}
	double seconds = 0;
	unsigned long long count = 0;
	for (int i = 3; i + 1 < argc; i += 2) {
		if (!std::strcmp(argv[i], "--seconds"))
			seconds = std::atof(argv[i + 1]);
		else if (!std::strcmp(argv[i], "--count"))
			count = std::strtoull(argv[i + 1], nullptr, 10);
	}
	std::signal(SIGINT, [](int) { g_stop = true; });
	std::signal(SIGTERM, [](int) { g_stop = true; });
	try {
		devbus::Recorder rec(argv[1], argv[2]);
		const auto start = std::chrono::steady_clock::now();
		while (!g_stop && rec.publisher_alive()) {
			rec.poll(std::chrono::milliseconds(100));
			if (count && rec.stats().samples >= count)
				break;
			if (seconds > 0 && std::chrono::steady_clock::now() - start >= std::chrono::duration<double>(seconds))
				break;
		}
		rec.poll(std::chrono::nanoseconds(0));
		rec.close();
		const auto s = rec.stats();
		std::printf("recorded %llu samples (%llu bytes) from %s, gaps %llu\n", static_cast<unsigned long long>(s.samples),
			    static_cast<unsigned long long>(s.bytes), argv[1], static_cast<unsigned long long>(s.gaps));
		return s.gaps ? 1 : 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "devbus-record: %s\n", e.what());
		return 2;
	}
}
