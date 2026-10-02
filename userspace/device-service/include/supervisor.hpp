#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "acquisition_worker.hpp"
#include "clock_calibrator.hpp"
#include "device.hpp"
#include "device_state.hpp"

namespace acq {

struct SupervisorConfig {
	// The DEVICE_ID read at startup. Recovery compares against it: on this
	// hardware a dead MCU does not make the device_id read fail - the SPI
	// transfer completes and returns 0x00000000 (seen on the Pi,
	// 2026-10-01, with fw_version=EIO next to it). "The probe didn't throw"
	// is therefore not proof of life; "the probe returned the right ID" is.
	uint32_t expected_device_id = 0;

	// Recovery: up to max_recovery_attempts soft resets, each given
	// recovery_timeout to show samples flowing again, with exponential
	// backoff (recovery_backoff, x2, x4...) between attempts. Out of
	// attempts = Fault, latched until an operator reset.
	int max_recovery_attempts = 3;
	std::chrono::milliseconds recovery_timeout{2000};
	std::chrono::milliseconds recovery_backoff{500};

	std::chrono::milliseconds calibration_duration{10000};
	uint64_t calibration_min_samples = 1000;

	// Where fault evidence (fault-*.json) and calibration.json go. Empty =
	// log only, nothing written.
	std::string state_dir;
	std::size_t evidence_keep = 20;
};

// Plan.md V2/M5: owns the machine-level State (device_state.hpp) and is
// the only thing that changes it. Every input - operator commands from
// the control socket, a stall from the Watchdog, the acquisition thread
// dying, SIGTERM - becomes an Event on one queue, and one thread applies
// them in order. That is the whole concurrency story: the procedures that
// touch the device (start, pause, soft reset, calibration) never run
// concurrently with each other, which is exactly what the pre-M5 design
// could not promise.
//
// Long procedures (recovery, calibration) run on that same thread. They
// poll for a pending Stop so shutdown is never stuck behind them; any
// other event that arrives meanwhile waits its turn and is re-checked
// against whatever state the procedure left behind.
class Supervisor {
public:
	Supervisor(Device& device, AcquisitionWorker& worker, SupervisorConfig cfg);
	~Supervisor();

	void start();
	// Posts Stop (if not already stopped) and joins.
	void stop();

	struct Reply {
		bool ok;
		std::string message;
	};
	// Operator command: checked against the current state right away, so
	// an illegal one is refused with a reason instead of queued. Accepted
	// = queued; the transition happens on the supervisor thread.
	Reply request(Event e, std::string reason = "operator");

	// Internal sources (watchdog, signal thread): fire-and-forget. Still
	// validated when dequeued - a Stall that raced a Pause is dropped.
	void post(Event e, std::string reason);

	State state() const { return state_.load(); }
	nlohmann::json status() const;

	// Blocks until the supervisor has settled in `s` - or has entered `s`
	// at any point since the call, so a state that only lasts a moment
	// (Recovering that succeeds at once) is not missed - or the timeout
	// passes. "Settled" means the state's entry action has finished, so
	// once this returns for Running, control=1 really has been written.
	bool wait_for_state(State s, std::chrono::milliseconds timeout) const;

	// Called on the supervisor thread after every transition (sd_notify
	// STATUS= in main.cpp).
	void set_on_transition(std::function<void(State from, State to, Event e)> cb) { on_transition_ = std::move(cb); }

private:
	struct Queued {
		Event event;
		std::string reason;
	};
	struct HistoryEntry {
		std::string at;
		State from;
		State to;
		Event event;
		std::string reason;
	};

	void run();
	void apply(Event e, const std::string& reason);
	// Device actions that make `to` true, done *before* the state is
	// published: Running means control=1 has been written, not that it is
	// about to be. Throws DeviceError if the device refuses.
	void prepare(State to, State from);
	// Work done after `to` is published; may return a follow-up event that
	// is applied before anything else on the queue (Recovering ->
	// Recovered, Calibrating -> CalibrationDone).
	std::optional<Queued> enter(State to);
	std::optional<Queued> run_recovery();
	std::optional<Queued> run_calibration();
	void enter_fault(const std::string& reason);

	// Sleeps up to d, returning false early if Stop was posted.
	bool sleep_unless_stopping(std::chrono::milliseconds d);
	bool stopping() const { return stop_pending_.load(); }
	nlohmann::json history_json() const;

	Device& device_;
	AcquisitionWorker& worker_;
	SupervisorConfig cfg_;
	ClockCalibrator calibrator_;

	std::atomic<State> state_{State::Init};
	std::atomic<bool> stop_pending_{false};
	// True while a transition is being applied, except while a long
	// procedure runs (that state is observable for its whole duration).
	bool busy_ = false;                 // guarded by mutex_
	uint64_t entries_[8] = {};          // per-State entry counts, guarded by mutex_
	std::chrono::steady_clock::time_point state_since_ = std::chrono::steady_clock::now();

	mutable std::mutex mutex_;
	mutable std::condition_variable cv_;
	std::deque<Queued> queue_;
	std::deque<HistoryEntry> history_;
	// Status fields, guarded by mutex_ (written on the supervisor thread,
	// read by status() from the control thread).
	uint64_t recoveries_ = 0;
	uint64_t recovery_attempts_ = 0;
	uint64_t faults_ = 0;
	std::string last_fault_reason_;
	std::string last_evidence_path_;
	std::optional<CalibrationResult> last_calibration_;

	std::thread thread_;
	std::function<void(State, State, Event)> on_transition_;
};

} // namespace acq
