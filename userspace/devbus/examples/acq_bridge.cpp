// acq-bridge: the real data path onto devbus.
//
//   MCU --SPI--> custom-acq driver --/dev/acq0--> acq-bridge --devbus "acq/samples"--> N consumers
//
// read() lands each sample directly in a loaned shared-memory chunk, so the
// only copy in the whole path is the kernel's copy_to_user(); every
// consumer then reads that same memory in place.
//
//   acq-bridge [/dev/acq0] [/sys/bus/spi/devices/spi0.0/] [--prio N] [--cpu C]
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#include "devbus/acq_sample.hpp"
#include "devbus/devbus.hpp"

// AcqSample now lives in one place, shared by every program that
// publishes or consumes this service.

namespace {

volatile std::sig_atomic_t stop = 0;

bool write_sysfs(const std::string& path, const char* value) {
	std::ofstream f(path);
	f << value;
	return static_cast<bool>(f);
}

} // namespace

int main(int argc, char** argv) {
	// Line-buffered: these print progress once a second, and that is useless
	// if it sits in a 4 KiB buffer until the process is killed.
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::string dev = "/dev/acq0";
	std::string sysfs = "/sys/bus/spi/devices/spi0.0/";
	int prio = 0, cpu = -1, positional = 0;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--prio") && i + 1 < argc)
			prio = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = std::atoi(argv[++i]);
		else if (positional++ == 0)
			dev = argv[i];
		else
			sysfs = argv[i];
	}
	std::signal(SIGINT, [](int) { stop = 1; });
	std::signal(SIGTERM, [](int) { stop = 1; });

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

	int fd = open(dev.c_str(), O_RDONLY);
	if (fd < 0) {
		std::fprintf(stderr, "open %s: %s\n", dev.c_str(), std::strerror(errno));
		return 1;
	}
	auto pub = devbus::Publisher<AcqSample>::create("acq/samples", {.max_subscribers = 4, .queue_capacity = 1024,
									 .lock_memory = true});
	if (!write_sysfs(sysfs + "control", "1"))
		std::fprintf(stderr, "warning: could not start acquisition via %scontrol\n", sysfs.c_str());
	std::printf("bridging %s -> devbus acq/samples\n", dev.c_str());

	uint64_t bridged = 0, short_reads = 0;
	pollfd p{fd, POLLIN, 0};
	while (!stop) {
		// poll with a timeout instead of blocking in read(): lets SIGTERM end the loop cleanly
		if (poll(&p, 1, 200) <= 0)
			continue;
		auto loan = pub.loan();
		if (!loan)
			continue; // can't happen with the default chunk budget
		ssize_t n = read(fd, &**loan, sizeof(AcqSample)); // kernel -> shared memory, directly
		if (n != static_cast<ssize_t>(sizeof(AcqSample))) {
			++short_reads;
			continue; // loan returns to the pool when it goes out of scope
		}
		pub.send(std::move(*loan));
		if (++bridged % 10000 == 0)
			std::printf("bridged %llu samples, %u subscribers\n", static_cast<unsigned long long>(bridged),
				    pub.active_subscribers());
	}
	write_sysfs(sysfs + "control", "0");
	std::printf("stopped: %llu samples bridged, %llu short reads\n", static_cast<unsigned long long>(bridged),
		    static_cast<unsigned long long>(short_reads));
	return 0;
}
