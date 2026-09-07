// Phase 1+2 (Plan.md V4): end-to-end loop — open /dev/acq0, start
// acquisition, drain samples on a worker thread into a ring buffer, print
// them, shut down cleanly on SIGINT/SIGTERM. Phase 2 adds Configuration,
// spdlog logging, MetricsReporter, Watchdog (ErrorRecovery), and systemd
// readiness/watchdog notifications (sd_notify) — GoogleTest unit tests
// live under tests/.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>

#include <sched.h>
#include <sys/mman.h>

#include <spdlog/spdlog.h>
#include <systemd/sd-daemon.h>

#include "acquisition_worker.hpp"
#include "config.hpp"
#include "device.hpp"
#include "latency_logger.hpp"
#include "metrics.hpp"
#include "ring_buffer.hpp"
#include "watchdog.hpp"

namespace {

// See docs/systems-programming-patterns.md's "信号（signal）机制" section
// for the full reasoning: a classic signal handler runs in a context too
// restricted to safely do any of stop_acquisition()/worker.stop()/
// buffer.stop() (file I/O, joining a thread, locking a mutex - none of it
// async-signal-safe). Blocking the signals on every thread and having one
// dedicated thread sit in sigwait() for them means that thread wakes up in
// ordinary thread context, not signal context, so it can call whatever it
// needs to.
void block_shutdown_signals(sigset_t& set) {
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void init_logging(const std::string& log_level) {
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
	spdlog::set_level(spdlog::level::from_str(log_level));
}

} // namespace

int main(int argc, char** argv) {
	// Config path is an optional first CLI arg; no arg means "just run on
	// defaults", not "look for a default filename that may not exist".
	acq::Config cfg = (argc > 1) ? acq::Config::load(argv[1]) : acq::Config{};
	init_logging(cfg.log_level);

	// mlockall() before any real work starts, and non-fatal on failure -
	// a scheduler-comparison knob (Plan.md V7), not something ordinary
	// runs should depend on. MCL_FUTURE covers thread stacks/heap growth
	// from here on, not just what's already mapped.
	if (cfg.lock_memory) {
		if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
			spdlog::warn("mlockall failed (need CAP_IPC_LOCK / root?): {}", std::strerror(errno));
		else
			spdlog::info("mlockall: locked");
	}

	// Applies to every thread this process later spawns too - both are
	// process-wide, set before the worker/watchdog/metrics threads start.
	if (cfg.sched_fifo_priority > 0) {
		struct sched_param param{};
		param.sched_priority = cfg.sched_fifo_priority;
		if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
			spdlog::warn("sched_setscheduler(SCHED_FIFO, {}) failed (need CAP_SYS_NICE / root?): {}",
				     cfg.sched_fifo_priority, std::strerror(errno));
		else
			spdlog::info("scheduler: SCHED_FIFO priority {}", cfg.sched_fifo_priority);
	}

	if (cfg.cpu_affinity_core >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cfg.cpu_affinity_core, &set);
		if (sched_setaffinity(0, sizeof(set), &set) != 0)
			spdlog::warn("sched_setaffinity(core {}) failed: {}",
				     cfg.cpu_affinity_core, std::strerror(errno));
		else
			spdlog::info("CPU affinity: pinned to core {}", cfg.cpu_affinity_core);
	}

	sigset_t shutdown_set;
	block_shutdown_signals(shutdown_set);

	acq::Device device(cfg.dev_path, cfg.sysfs_dir);
	acq::RingBuffer<acq::Sample> buffer(cfg.buffer_capacity);

	try {
		device.open();

		// Liveness check before touching acquisition at all - reuses the
		// existing read-only sysfs attrs rather than inventing a new
		// probe path (see docs/learning-qa.md Q25/Q26).
		uint32_t device_id = device.read_device_id();
		uint32_t fw_version = device.read_fw_version();
		spdlog::info("device online: DEVICE_ID=0x{:08x} FW_VERSION=0x{:08x}", device_id, fw_version);
		// No-op when not run under systemd (NOTIFY_SOCKET unset) - safe to
		// call unconditionally rather than guarding it on how we were launched.
		sd_notify(0, "READY=1");
	} catch (const std::exception& e) {
		spdlog::error("startup failed: {}", e.what());
		return 1;
	}

	acq::LatencyLogger latency_logger(cfg.latency_log_path);
	if (latency_logger.enabled())
		spdlog::info("latency logging enabled: {}", cfg.latency_log_path);
	acq::AcquisitionWorker worker(device, buffer, &latency_logger);

	std::thread signal_thread([&] {
		int sig = 0;
		sigwait(&shutdown_set, &sig);
		spdlog::info("received signal {}, shutting down...", sig);
		try {
			device.stop_acquisition();
		} catch (const std::exception& e) {
			spdlog::error("stop_acquisition failed: {}", e.what());
		}
		// Unblock the buffer first: if the worker happens to be stuck
		// inside buffer_.push() (buffer momentarily full), worker.stop()
		// alone would deadlock waiting to join a thread that's waiting on
		// a condition only buffer_.stop() can satisfy.
		buffer.stop();
		worker.stop();
	});

	acq::MetricsReporter metrics(worker, buffer, device, std::chrono::milliseconds(cfg.metrics_interval_ms));
	// Reuse the metrics heartbeat for the systemd watchdog ping instead of
	// running a second timer purely for sd_notify - see MetricsReporter's
	// header comment on set_on_tick().
	metrics.set_on_tick([] { sd_notify(0, "WATCHDOG=1"); });
	acq::Watchdog watchdog(device, worker, std::chrono::milliseconds(cfg.liveness_timeout_ms));

	try {
		device.start_acquisition();
	} catch (const std::exception& e) {
		spdlog::error("start_acquisition failed: {}", e.what());
		pthread_kill(signal_thread.native_handle(), SIGTERM);
		signal_thread.join();
		return 1;
	}
	worker.start();
	metrics.start();
	watchdog.start();

	uint64_t printed = 0;
	acq::Sample s{};
	while (buffer.pop(s)) {
		if (printed < 20 || printed % 500 == 0)
			spdlog::info("seq={} value={}", s.seq, s.value);
		++printed;
	}

	signal_thread.join();
	watchdog.stop();
	metrics.stop();

	if (auto err = worker.last_error()) {
		try {
			std::rethrow_exception(err);
		} catch (const std::exception& e) {
			spdlog::error("acquisition worker stopped on error: {}", e.what());
		}
	}

	metrics.report_once(); // final snapshot, independent of the periodic tick timing
	return 0;
}
