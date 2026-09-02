// Phase 1 (Plan.md V4): minimal end-to-end loop — open /dev/acq0, start
// acquisition, drain samples on a worker thread into a ring buffer, print
// them, shut down cleanly on SIGINT/SIGTERM. No Logging/Configuration/
// Metrics/ErrorRecovery/systemd yet — see the plan file for what's
// deferred to Phase 2 and why.
#include <csignal>
#include <cstdio>
#include <exception>
#include <thread>

#include "acquisition_worker.hpp"
#include "device.hpp"
#include "ring_buffer.hpp"

namespace {

// Graceful shutdown via sigwait() on a dedicated thread rather than a
// classic signal handler: a real async signal handler can only safely
// touch a handful of primitives (no mutexes, no condition_variables,
// no I/O) — but shutting this service down means calling
// stop_acquisition() (does file I/O), worker.stop() (joins a thread) and
// buffer.stop() (locks a mutex), none of which are async-signal-safe.
// Blocking SIGINT/SIGTERM on every thread and having one dedicated thread
// sit in sigwait() for them sidesteps that entirely — by the time this
// thread wakes up, it's running in ordinary thread context, not signal
// context, so it can call whatever it needs to.
void block_shutdown_signals(sigset_t& set) {
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

} // namespace

int main() {
	sigset_t shutdown_set;
	block_shutdown_signals(shutdown_set);

	acq::Device device("/dev/acq0", "/sys/bus/spi/devices/spi0.0/");
	acq::RingBuffer<acq::Sample> buffer(4096);

	try {
		device.open();

		// Liveness check before touching acquisition at all — reuses the
		// existing read-only sysfs attrs rather than inventing a new
		// probe path (see docs/learning-qa.md Q25/Q26).
		uint32_t device_id = device.read_device_id();
		uint32_t fw_version = device.read_fw_version();
		std::printf("device online: DEVICE_ID=0x%08x FW_VERSION=0x%08x\n", device_id, fw_version);
	} catch (const std::exception& e) {
		std::fprintf(stderr, "startup failed: %s\n", e.what());
		return 1;
	}

	acq::AcquisitionWorker worker(device, buffer);

	std::thread signal_thread([&] {
		int sig = 0;
		sigwait(&shutdown_set, &sig);
		std::printf("\nreceived signal %d, shutting down...\n", sig);
		try {
			device.stop_acquisition();
		} catch (const std::exception& e) {
			std::fprintf(stderr, "stop_acquisition failed: %s\n", e.what());
		}
		// Unblock the buffer first: if the worker happens to be stuck
		// inside buffer_.push() (buffer momentarily full), worker.stop()
		// alone would deadlock waiting to join a thread that's waiting on
		// a condition only buffer_.stop() can satisfy.
		buffer.stop();
		worker.stop();
	});

	try {
		device.start_acquisition();
	} catch (const std::exception& e) {
		std::fprintf(stderr, "start_acquisition failed: %s\n", e.what());
		pthread_kill(signal_thread.native_handle(), SIGTERM);
		signal_thread.join();
		return 1;
	}
	worker.start();

	uint64_t printed = 0;
	acq::Sample s{};
	while (buffer.pop(s)) {
		if (printed < 20 || printed % 500 == 0)
			std::printf("seq=%u value=%u\n", s.seq, s.value);
		++printed;
	}

	signal_thread.join();

	if (auto err = worker.last_error()) {
		try {
			std::rethrow_exception(err);
		} catch (const std::exception& e) {
			std::fprintf(stderr, "acquisition worker stopped on error: %s\n", e.what());
		}
	}

	std::printf(
		"shutdown report: samples_read=%llu gap_count=%llu kfifo_overflow=%u\n",
		static_cast<unsigned long long>(worker.samples_read()),
		static_cast<unsigned long long>(worker.gap_count()),
		device.read_kfifo_overflow());

	return 0;
}
