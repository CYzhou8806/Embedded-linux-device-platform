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
#include <functional>
#include <memory>
#include <thread>

#include <sched.h>
#include <sys/mman.h>

#include <spdlog/spdlog.h>
#include <systemd/sd-daemon.h>

#include "acquisition_worker.hpp"
#include "backpressure_controller.hpp"
#include "config.hpp"
#include "device.hpp"
#include "latency_logger.hpp"
#include "metrics.hpp"
#include "ring_buffer.hpp"
#include "sample_publisher.hpp"
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

	// Optional: republish every sample on devbus so other processes can
	// consume the stream (only one process can hold /dev/acq0). A failure
	// here is not fatal - acquisition is the job, publishing is a bonus.
	std::unique_ptr<acq::SamplePublisher> sample_publisher;
	if (!cfg.devbus_service.empty()) {
		try {
			sample_publisher = std::make_unique<acq::SamplePublisher>(
				cfg.devbus_service, cfg.devbus_max_subscribers, cfg.devbus_queue_capacity);
			spdlog::info("publishing samples on devbus service '{}'", cfg.devbus_service);
		} catch (const std::exception& e) {
			spdlog::warn("devbus publishing disabled: {}", e.what());
		}
	}
	acq::AcquisitionWorker worker(device, buffer, &latency_logger, sample_publisher.get());

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
	// The leading congestion signal, assembled from whichever sources are
	// configured: sample age covers the driver/SPI side falling behind,
	// devbus pressure covers a consumer falling behind. Null when neither
	// is configured, which leaves the controller reacting to
	// kfifo_overflow alone, exactly as before.
	std::function<bool()> early_warning;
	if (cfg.backpressure_max_sample_age_us > 0 || cfg.backpressure_max_devbus_pressure > 0.0) {
		const auto max_age = std::chrono::microseconds(cfg.backpressure_max_sample_age_us);
		const double max_pressure = cfg.backpressure_max_devbus_pressure;
		acq::SamplePublisher* pub = sample_publisher.get();
		early_warning = [&worker, pub, max_age, max_pressure] {
			// The smoothed average, not the instantaneous reading and not
			// the window peak - see AcquisitionWorker::sample_age_ewma()
			// for what each of those measured on hardware.
			if (max_age.count() > 0 && worker.sample_age_ewma() > max_age)
				return true;
			if (pub && max_pressure > 0.0 && static_cast<double>(pub->pressure()) > max_pressure)
				return true;
			return false;
		};
		spdlog::info("backpressure leading signal: max sample age {} us, max devbus pressure {}",
			      cfg.backpressure_max_sample_age_us, cfg.backpressure_max_devbus_pressure);
	}
	acq::BackpressureController backpressure(device, std::chrono::milliseconds(cfg.backpressure_check_interval_ms),
						  cfg.backpressure_min_hz, cfg.backpressure_target_hz,
						  cfg.backpressure_backoff_divisor, cfg.backpressure_recovery_step_hz,
						  std::move(early_warning));

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
	if (cfg.backpressure_enabled)
		backpressure.start();

	uint64_t printed = 0;
	acq::Sample s{};
	while (buffer.pop(s)) {
		if (printed < 20 || printed % 500 == 0)
			spdlog::info("seq={} value={}", s.seq, s.value);
		++printed;
	}

	signal_thread.join();
	if (cfg.backpressure_enabled)
		backpressure.stop();
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
