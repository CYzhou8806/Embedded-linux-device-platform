// devbus-ls: list devbus services and per-subscriber queue/drop counters.
// Reads shared memory directly, so it works while everything is running
// and needs no cooperation from the processes involved.
//
//   devbus-ls          one snapshot
//   devbus-ls -w 1     refresh every second
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "devbus/introspect.hpp"

namespace {

void print_once() {
	auto services = devbus::list_services();
	if (services.empty()) {
		std::printf("no devbus services\n");
		return;
	}
	for (const auto& s : services) {
		std::printf("%s  publisher=%d%s  published=%llu  loan_failures=%llu  payload=%uB  queue=%u  chunks=%u  shm=%.1fKiB\n",
			    s.shm_name.c_str(), s.publisher_pid, s.publisher_alive ? "" : " (DEAD)",
			    static_cast<unsigned long long>(s.published), static_cast<unsigned long long>(s.loan_failures),
			    s.type_size, s.queue_capacity, s.chunk_count, s.segment_bytes / 1024.0);
		for (const auto& sub : s.subscribers) {
			std::printf("  [%u] pid=%-7d %-8s %-12s queued=%-4llu delivered=%-10llu drop_old=%-8llu drop_new=%-8llu "
				    "block_to=%-6llu blocked=%.1fms\n",
				    sub.slot, sub.pid, sub.state.c_str(), sub.overflow.c_str(),
				    static_cast<unsigned long long>(sub.queued), static_cast<unsigned long long>(sub.delivered),
				    static_cast<unsigned long long>(sub.dropped_oldest),
				    static_cast<unsigned long long>(sub.dropped_newest),
				    static_cast<unsigned long long>(sub.block_timeouts), sub.blocked_ns / 1e6);
		}
	}
}

} // namespace

int main(int argc, char** argv) {
	int watch_s = 0;
	if (argc == 3 && std::strcmp(argv[1], "-w") == 0)
		watch_s = std::atoi(argv[2]);
	do {
		if (watch_s)
			std::printf("\033[H\033[2J");
		print_once();
		if (watch_s)
			std::this_thread::sleep_for(std::chrono::seconds(watch_s));
	} while (watch_s);
	return 0;
}
