#include "supervisor.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>

#include <spdlog/spdlog.h>

#include "fault_evidence.hpp"

namespace acq {

namespace {

constexpr std::size_t kHistoryLen = 32;
// How often the supervisor thread wakes on an empty queue, to notice the
// acquisition thread having died. Nothing else needs a timer here.
constexpr auto kIdleTick = std::chrono::milliseconds(200);
constexpr auto kPollStep = std::chrono::milliseconds(20);

std::string utc_now() {
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&t, &tm);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

std::string error_text(const std::exception_ptr& p) {
	if (!p)
		return "unknown error";
	try {
		std::rethrow_exception(p);
	} catch (const std::exception& e) {
		return e.what();
	} catch (...) {
		return "non-standard exception";
	}
}

nlohmann::json calibration_json(const CalibrationResult& r) {
	nlohmann::json j{
		{"ok", r.ok},
		{"samples", r.samples},
		{"nominal_hz", r.nominal_hz},
		{"duration_s", r.duration_s},
		{"gaps", r.gaps},
		{"repeated_timestamps", r.repeated_timestamps},
	};
	if (r.ok) {
		j["measured_hz"] = r.measured_hz;
		j["drift_ppm"] = r.drift_ppm;
	} else {
		j["reason"] = r.reason;
	}
	return j;
}

} // namespace

Supervisor::Supervisor(Device& device, AcquisitionWorker& worker, SupervisorConfig cfg)
	: device_(device), worker_(worker), cfg_(std::move(cfg)) {}

Supervisor::~Supervisor() {
	stop();
}

void Supervisor::start() {
	thread_ = std::thread(&Supervisor::run, this);
}

void Supervisor::stop() {
	if (state_.load() != State::Stopped && thread_.joinable())
		post(Event::Stop, "shutdown");
	if (thread_.joinable())
		thread_.join();
}

Supervisor::Reply Supervisor::request(Event e, std::string reason) {
	const State now = state_.load();
	if (!transition(now, e)) {
		return {false, std::string(to_string(e)) + " not allowed in state " + std::string(to_string(now))};
	}
	post(e, std::move(reason));
	return {true, std::string(to_string(e)) + " accepted in state " + std::string(to_string(now))};
}

void Supervisor::post(Event e, std::string reason) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back({e, std::move(reason)});
		if (e == Event::Stop)
			stop_pending_ = true;
	}
	cv_.notify_all();
}

bool Supervisor::wait_for_state(State s, std::chrono::milliseconds timeout) const {
	std::unique_lock<std::mutex> lock(mutex_);
	const uint64_t seen = entries_[static_cast<int>(s)];
	return cv_.wait_for(lock, timeout, [&] {
		return !busy_ && (state_.load() == s || entries_[static_cast<int>(s)] > seen);
	});
}

void Supervisor::run() {
	while (true) {
		Queued q;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait_for(lock, kIdleTick, [this] { return !queue_.empty(); });
			if (queue_.empty()) {
				lock.unlock();
				// The acquisition thread can't report its own death; notice it here.
				const State s = state_.load();
				if (worker_.failed() && (s == State::Ready || s == State::Running || s == State::Paused))
					apply(Event::DeviceError, "acquisition thread: " + error_text(worker_.last_error()));
				continue;
			}
			q = std::move(queue_.front());
			queue_.pop_front();
		}
		apply(q.event, q.reason);
		if (state_.load() == State::Stopped)
			return;
	}
}

void Supervisor::apply(Event e, const std::string& reason) {
	auto set_busy = [this](bool b) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			busy_ = b;
		}
		cv_.notify_all();
	};

	set_busy(true);
	std::optional<Queued> next = Queued{e, reason};
	while (next) {
		const State from = state_.load();
		const auto to = transition(from, next->event);
		if (!to) {
			// Not an error: e.g. a watchdog Stall queued just before an
			// operator Pause was applied.
			spdlog::debug("supervisor: dropped {} in state {}", to_string(next->event), to_string(from));
			break;
		}
		try {
			prepare(*to, from);
		} catch (const std::exception& ex) {
			// The device refused the action that would have made `to`
			// true; the state never changes to `to`. In Recovering this
			// can't happen (run_recovery catches its own); everywhere else
			// DeviceError leads to Fault.
			spdlog::error("supervisor: {} -> {} failed: {}", to_string(from), to_string(*to), ex.what());
			next = Queued{Event::DeviceError, ex.what()};
			continue;
		}

		spdlog::info("supervisor: {} -> {} on {} ({})", to_string(from), to_string(*to),
			     to_string(next->event), next->reason);
		const bool long_procedure = *to == State::Recovering || *to == State::Calibrating;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			history_.push_back({utc_now(), from, *to, next->event, next->reason});
			if (history_.size() > kHistoryLen)
				history_.pop_front();
			state_.store(*to);
			state_since_ = std::chrono::steady_clock::now();
			++entries_[static_cast<int>(*to)];
			if (long_procedure)
				busy_ = false; // observable while it runs
		}
		cv_.notify_all();
		if (on_transition_)
			on_transition_(from, *to, next->event);

		try {
			next = enter(*to);
		} catch (const std::exception& ex) {
			next = Queued{Event::DeviceError, ex.what()};
		}
		if (long_procedure)
			set_busy(true);
	}
	set_busy(false);
}

void Supervisor::prepare(State to, State from) {
	switch (to) {
	case State::Running:
		if (from == State::Ready || from == State::Paused) {
			worker_.rearm();
			device_.start_acquisition();
		}
		break;
	case State::Paused:
		device_.stop_acquisition();
		break;
	case State::Stopped:
		// Shutdown never fails: a device that can't be told to stop is
		// logged, not escalated.
		try {
			device_.stop_acquisition();
		} catch (const std::exception& e) {
			spdlog::warn("supervisor: stop_acquisition on shutdown failed: {}", e.what());
		}
		break;
	default:
		break;
	}
}

std::optional<Supervisor::Queued> Supervisor::enter(State to) {
	switch (to) {
	case State::Calibrating:
		return run_calibration();
	case State::Recovering:
		return run_recovery();
	case State::Fault: {
		std::string reason;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			reason = history_.empty() ? "" : history_.back().reason;
		}
		enter_fault(reason);
		return std::nullopt;
	}
	default:
		return std::nullopt;
	}
}

std::optional<Supervisor::Queued> Supervisor::run_recovery() {
	std::string last_problem = "no attempt made";
	for (int attempt = 1; attempt <= cfg_.max_recovery_attempts; ++attempt) {
		if (stopping())
			return std::nullopt;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			++recovery_attempts_;
		}
		spdlog::info("supervisor: recovery attempt {}/{}", attempt, cfg_.max_recovery_attempts);
		try {
			const uint32_t id = device_.read_device_id();
			if (cfg_.expected_device_id != 0 && id != cfg_.expected_device_id) {
				char buf[96];
				std::snprintf(buf, sizeof(buf), "probe read DEVICE_ID=0x%08x, expected 0x%08x - MCU not responding",
					      id, cfg_.expected_device_id);
				throw DeviceError(buf);
			}
			// The read loop died on an I/O error: its fd is suspect, and
			// the thread is gone. Start both again.
			if (worker_.failed()) {
				spdlog::info("supervisor: acquisition thread had died ({}), restarting it",
					     error_text(worker_.last_error()));
				worker_.stop();
				device_.reopen();
				worker_.start();
			}
			const uint64_t before = worker_.samples_read();
			// Soft reset: the MCU firmware clears its seq_counter and FIFO
			// on control=1 (case-05).
			device_.stop_acquisition();
			worker_.rearm();
			device_.start_acquisition();

			const auto deadline = std::chrono::steady_clock::now() + cfg_.recovery_timeout;
			while (std::chrono::steady_clock::now() < deadline) {
				if (worker_.samples_read() > before) {
					std::lock_guard<std::mutex> lock(mutex_);
					++recoveries_;
					return Queued{Event::Recovered, "samples flowing after attempt " + std::to_string(attempt)};
				}
				if (worker_.failed())
					break;
				if (!sleep_unless_stopping(kPollStep))
					return std::nullopt;
			}
			last_problem = worker_.failed() ? "acquisition thread died again: " + error_text(worker_.last_error())
							: "no samples within " + std::to_string(cfg_.recovery_timeout.count()) +
								  " ms of a soft reset";
		} catch (const std::exception& e) {
			last_problem = e.what();
		}
		spdlog::warn("supervisor: recovery attempt {} failed: {}", attempt, last_problem);
		if (attempt < cfg_.max_recovery_attempts &&
		    !sleep_unless_stopping(cfg_.recovery_backoff * (1 << (attempt - 1))))
			return std::nullopt;
	}
	return Queued{Event::RecoveryFailed, last_problem};
}

std::optional<Supervisor::Queued> Supervisor::run_calibration() {
	const uint32_t nominal = device_.read_sample_rate();
	calibrator_.reset(nominal);
	worker_.set_calibrator(&calibrator_);
	// Calibrating is only entered from Ready or Paused, where the MCU is
	// not producing; it has to be for this.
	device_.start_acquisition();
	spdlog::info("supervisor: calibrating against nominal {} Hz for {} ms", nominal,
		     cfg_.calibration_duration.count());

	const auto deadline = std::chrono::steady_clock::now() + cfg_.calibration_duration;
	bool interrupted = false;
	while (std::chrono::steady_clock::now() < deadline) {
		if (worker_.failed() || !sleep_unless_stopping(kPollStep * 5)) {
			interrupted = true;
			break;
		}
	}
	worker_.set_calibrator(nullptr);
	device_.stop_acquisition();
	if (worker_.failed())
		return Queued{Event::DeviceError, "acquisition thread died during calibration: " +
							  error_text(worker_.last_error())};
	if (interrupted)
		return std::nullopt; // Stop is next in the queue

	const CalibrationResult r = calibrator_.result(cfg_.calibration_min_samples);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		last_calibration_ = r;
	}
	if (!r.ok) {
		spdlog::warn("supervisor: calibration failed: {}", r.reason);
		return Queued{Event::CalibrationDone, "failed: " + r.reason};
	}
	spdlog::info("supervisor: calibration: {} samples over {:.1f} s, measured {:.4f} Hz, drift {:+.2f} ppm",
		     r.samples, r.duration_s, r.measured_hz, r.drift_ppm);

	if (!cfg_.state_dir.empty()) {
		try {
			std::filesystem::create_directories(cfg_.state_dir);
			nlohmann::json j = calibration_json(r);
			j["calibrated_at"] = utc_now();
			const std::string path = cfg_.state_dir + "/calibration.json";
			// Write-then-rename: a power cut mid-write must not leave a
			// truncated calibration behind for the next boot to trust.
			std::ofstream(path + ".tmp") << j.dump(2) << '\n';
			std::filesystem::rename(path + ".tmp", path);
		} catch (const std::exception& e) {
			spdlog::warn("supervisor: calibration result not saved: {}", e.what());
		}
	}
	char buf[64];
	std::snprintf(buf, sizeof(buf), "ok: drift %+.2f ppm", r.drift_ppm);
	return Queued{Event::CalibrationDone, buf};
}

void Supervisor::enter_fault(const std::string& reason) {
	try {
		device_.stop_acquisition();
	} catch (const std::exception& e) {
		spdlog::warn("supervisor: stop_acquisition entering Fault failed: {}", e.what());
	}
	nlohmann::json service = status();
	const std::string path = capture_fault_evidence(cfg_.state_dir, cfg_.evidence_keep, reason, service, device_);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		++faults_;
		last_fault_reason_ = reason;
		last_evidence_path_ = path;
	}
	if (path.empty())
		spdlog::error("supervisor: FAULT ({}) - latched until 'reset'", reason);
	else
		spdlog::error("supervisor: FAULT ({}) - evidence in {}; latched until 'reset'", reason, path);
}

bool Supervisor::sleep_unless_stopping(std::chrono::milliseconds d) {
	std::unique_lock<std::mutex> lock(mutex_);
	return !cv_.wait_for(lock, d, [this] { return stop_pending_.load(); });
}

nlohmann::json Supervisor::history_json() const {
	nlohmann::json h = nlohmann::json::array();
	for (const auto& e : history_)
		h.push_back({{"at", e.at},
			     {"from", to_string(e.from)},
			     {"to", to_string(e.to)},
			     {"event", to_string(e.event)},
			     {"reason", e.reason}});
	return h;
}

nlohmann::json Supervisor::status() const {
	std::lock_guard<std::mutex> lock(mutex_);
	nlohmann::json j;
	j["state"] = to_string(state_.load());
	j["in_state_s"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state_since_).count();
	j["samples_read"] = worker_.samples_read();
	j["gaps"] = worker_.gap_count();
	j["sample_age_ewma_us"] = std::chrono::duration_cast<std::chrono::microseconds>(worker_.sample_age_ewma()).count();
	j["recoveries"] = recoveries_;
	j["recovery_attempts"] = recovery_attempts_;
	j["faults"] = faults_;
	if (!last_fault_reason_.empty())
		j["last_fault"] = {{"reason", last_fault_reason_}, {"evidence", last_evidence_path_}};
	if (last_calibration_)
		j["last_calibration"] = calibration_json(*last_calibration_);
	j["history"] = history_json();
	return j;
}

} // namespace acq
