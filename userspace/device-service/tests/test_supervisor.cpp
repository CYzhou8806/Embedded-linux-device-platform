// End-to-end tests of the M5 orchestration layer against a simulated MCU:
// the real Device, AcquisitionWorker, Watchdog and Supervisor classes, with
// /dev/acq0 replaced by a named pipe and the sysfs directory by a temp
// directory of plain files. Device only ever does open/poll/read on the
// one and plain file reads/writes on the other, so nothing in the code
// under test knows the difference.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "acquisition_worker.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "ring_buffer.hpp"
#include "supervisor.hpp"
#include "watchdog.hpp"

using namespace std::chrono_literals;
using acq::State;

namespace {

constexpr uint32_t kDeviceId = 0xac00acc0;

int64_t mono_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

// Behaves like the v1.4 firmware as far as the service can tell: produces
// samples while control=1, restarts its sequence counter on every 0->1
// write (case-05), and can be made to misbehave.
class FakeMcu {
public:
	// stuck: stops producing, but a soft reset (control 0 -> 1) clears it -
	//        the failure the old Watchdog's soft reset was written for.
	// dead:  stops producing and nothing in software brings it back.
	std::atomic<bool> stuck{false};
	std::atomic<bool> dead{false};
	std::atomic<double> drift_ppm{0.0};

	FakeMcu() {
		char tmpl[] = "/tmp/ds-sup-XXXXXX";
		dir_ = ::mkdtemp(tmpl);
		set("device_id", "0xac00acc0");
		set("fw_version", "0x00010004");
		set("sample_rate", "1000");
		set("kfifo_overflow", "0");
		set("control", "0");
		::mkfifo(dev().c_str(), 0600);
		// O_RDWR so this open doesn't block waiting for a reader, and so
		// the pipe never sees "all writers gone" until close_pipe().
		wfd_ = ::open(dev().c_str(), O_RDWR | O_NONBLOCK);
		thread_ = std::thread([this] { run(); });
	}
	~FakeMcu() {
		stop_ = true;
		thread_.join();
		close_pipe();
		std::filesystem::remove_all(dir_);
	}

	std::string dir() const { return dir_; }
	std::string sysfs() const { return dir_ + "/"; }
	std::string dev() const { return dir_ + "/acq0"; }

	void set(const std::string& name, const std::string& value) { std::ofstream(dir_ + "/" + name) << value; }
	std::string get(const std::string& name) const {
		std::string v;
		std::ifstream(dir_ + "/" + name) >> v;
		return v;
	}
	// What a real driver read error looks like to the service: read() on
	// /dev/acq0 hits EOF, read_exact() throws, the worker thread dies.
	void close_pipe() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (wfd_ >= 0)
			::close(wfd_);
		wfd_ = -1;
	}
	void reopen_pipe() {
		std::lock_guard<std::mutex> lock(mutex_);
		if (wfd_ < 0)
			wfd_ = ::open(dev().c_str(), O_RDWR | O_NONBLOCK);
	}

private:
	// The real MCU sees every control write as its own SPI transaction;
	// polling a file can't see a "0" that was immediately overwritten by
	// "1". So any new write (mtime moved) that leaves the file at "1"
	// counts as a start command - which is what the firmware acts on.
	int64_t control_mtime() const {
		struct stat st{};
		if (::stat((dir_ + "/control").c_str(), &st) != 0)
			return 0;
		return st.st_mtim.tv_sec * 1'000'000'000LL + st.st_mtim.tv_nsec;
	}

	void run() {
		char prev = '0';
		int64_t prev_mtime = control_mtime();
		uint32_t seq = 0;
		int64_t base = mono_ns();
		while (!stop_) {
			const std::string c = get("control");
			const char now = c.empty() ? prev : c[0]; // a write in progress reads empty
			const int64_t mtime = control_mtime();
			if (now == '1' && (prev == '0' || mtime != prev_mtime)) {
				seq = 0;
				base = mono_ns();
				stuck = false;
			}
			prev = now;
			prev_mtime = mtime;
			if (now == '1' && !stuck && !dead) {
				// Synthetic timestamps on an exact (1 kHz + drift) grid, so a
				// calibration has a known right answer.
				const double spacing = 1e6 / (1.0 + drift_ppm.load() * 1e-6);
				acq::Sample s{seq, seq * 3, base + static_cast<int64_t>(seq * spacing)};
				std::lock_guard<std::mutex> lock(mutex_);
				if (wfd_ >= 0 && ::write(wfd_, &s, sizeof(s)) == sizeof(s))
					++seq;
			}
			std::this_thread::sleep_for(1ms);
		}
	}

	std::string dir_;
	int wfd_ = -1;
	std::mutex mutex_;
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

// The service's runtime wiring from main.cpp, minus the parts that are
// not under test (signals, metrics, devbus, backpressure).
struct Rig {
	FakeMcu mcu;
	acq::Device device{mcu.dev(), mcu.sysfs()};
	acq::RingBuffer<acq::Sample> buffer{4096};
	acq::AcquisitionWorker worker{device, buffer};
	std::thread drain;
	std::unique_ptr<acq::Supervisor> sup;
	std::unique_ptr<acq::Watchdog> watchdog;

	explicit Rig(acq::SupervisorConfig cfg = {}) {
		device.open();
		cfg.expected_device_id = kDeviceId;
		if (cfg.recovery_timeout == acq::SupervisorConfig{}.recovery_timeout)
			cfg.recovery_timeout = 300ms;
		cfg.recovery_backoff = 50ms;
		cfg.max_recovery_attempts = 2;
		cfg.state_dir = mcu.dir() + "/state";
		sup = std::make_unique<acq::Supervisor>(device, worker, cfg);
		watchdog = std::make_unique<acq::Watchdog>(
			worker, 300ms, [this] { return sup->state() == State::Running; },
			[this](std::chrono::milliseconds) { sup->post(acq::Event::Stall, "test watchdog"); });
		drain = std::thread([this] {
			acq::Sample s;
			while (buffer.pop(s)) {
			}
		});
		worker.start();
		sup->start();
		watchdog->start();
		sup->post(acq::Event::InitOk, "test");
	}
	~Rig() {
		watchdog->stop();
		sup->stop();
		buffer.stop();
		worker.stop();
		drain.join();
	}

	// Returns once samples are actually flowing, i.e. the fake MCU has
	// seen control=1 - injecting a fault before that would race its
	// start-command handling, which clears `stuck`.
	void start_running() {
		ASSERT_TRUE(sup->wait_for_state(State::Ready, 1s));
		ASSERT_TRUE(sup->request(acq::Event::Start).ok);
		ASSERT_TRUE(sup->wait_for_state(State::Running, 1s));
		ASSERT_TRUE(samples_advance(1s));
	}
	bool samples_advance(std::chrono::milliseconds within) {
		const auto before = worker.samples_read();
		const auto deadline = std::chrono::steady_clock::now() + within;
		while (std::chrono::steady_clock::now() < deadline) {
			if (worker.samples_read() > before + 10)
				return true;
			std::this_thread::sleep_for(10ms);
		}
		return false;
	}
};

} // namespace

TEST(Supervisor, StartsAndAcquires) {
	Rig rig;
	rig.start_running();
	EXPECT_EQ(rig.mcu.get("control"), "1");
	EXPECT_TRUE(rig.samples_advance(1s));
}

TEST(Supervisor, IllegalCommandIsRefusedWithAReason) {
	Rig rig;
	ASSERT_TRUE(rig.sup->wait_for_state(State::Ready, 1s));
	const auto r = rig.sup->request(acq::Event::Pause);
	EXPECT_FALSE(r.ok);
	EXPECT_NE(r.message.find("not allowed in state Ready"), std::string::npos);
	EXPECT_EQ(rig.sup->state(), State::Ready);
}

// Pausing must not look like a stall: before M5 the watchdog had no idea a
// pause existed and would soft-reset the MCU straight out of it.
TEST(Supervisor, PauseIsNotMistakenForAStall) {
	Rig rig;
	rig.start_running();
	ASSERT_TRUE(rig.sup->request(acq::Event::Pause).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Paused, 1s));
	EXPECT_EQ(rig.mcu.get("control"), "0");
	std::this_thread::sleep_for(900ms); // three watchdog timeouts
	EXPECT_EQ(rig.sup->state(), State::Paused);
	EXPECT_EQ(rig.sup->status()["recovery_attempts"], 0);

	ASSERT_TRUE(rig.sup->request(acq::Event::Resume).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Running, 1s));
	EXPECT_TRUE(rig.samples_advance(1s));
	// Resuming after a long pause must not trip the watchdog either
	// (AcquisitionWorker::rearm()).
	std::this_thread::sleep_for(400ms);
	EXPECT_EQ(rig.sup->state(), State::Running);
}

TEST(Supervisor, StallIsRecoveredBySoftReset) {
	Rig rig;
	rig.start_running();
	rig.mcu.stuck = true;
	ASSERT_TRUE(rig.sup->wait_for_state(State::Recovering, 2s));
	ASSERT_TRUE(rig.sup->wait_for_state(State::Running, 2s));
	EXPECT_TRUE(rig.samples_advance(1s));
	const auto st = rig.sup->status();
	EXPECT_EQ(st["recoveries"], 1);
	EXPECT_EQ(st["faults"], 0);
}

// The 2026-10-01 hardware observation: a dead MCU still "answers" the
// device_id probe - with zeros. Recovery must treat that as dead, give up
// after its attempts, and leave evidence behind.
TEST(Supervisor, DeadMcuLatchesFaultWithEvidence) {
	Rig rig;
	rig.start_running();
	rig.mcu.dead = true;
	rig.mcu.set("device_id", "0x00000000");
	std::filesystem::remove(rig.mcu.dir() + "/fw_version"); // the EIO that came with it
	ASSERT_TRUE(rig.sup->wait_for_state(State::Fault, 3s));

	const auto st = rig.sup->status();
	EXPECT_EQ(st["recovery_attempts"], 2);
	ASSERT_TRUE(st.contains("last_fault"));
	EXPECT_NE(st["last_fault"]["reason"].get<std::string>().find("expected 0xac00acc0"), std::string::npos);

	const std::string path = st["last_fault"]["evidence"];
	ASSERT_FALSE(path.empty());
	nlohmann::json ev;
	std::ifstream(path) >> ev;
	EXPECT_EQ(ev["driver"]["device_id"], "0x00000000");
	EXPECT_NE(ev["driver"]["fw_version"].get<std::string>().find("error"), std::string::npos);
	EXPECT_EQ(ev["service"]["state"], "Fault");
	EXPECT_TRUE(ev["service"]["history"].is_array());
	EXPECT_TRUE(ev.contains("kernel")); // lines, or why they were unavailable
	EXPECT_EQ(rig.mcu.get("control"), "0"); // Fault stops acquisition
}

TEST(Supervisor, FaultIsLatchedThenClearedByOperatorReset) {
	Rig rig;
	rig.start_running();
	rig.mcu.dead = true;
	ASSERT_TRUE(rig.sup->wait_for_state(State::Fault, 3s));
	// Nothing but reset gets it out.
	EXPECT_FALSE(rig.sup->request(acq::Event::Start).ok);
	EXPECT_FALSE(rig.sup->request(acq::Event::Resume).ok);
	std::this_thread::sleep_for(500ms);
	EXPECT_EQ(rig.sup->state(), State::Fault);

	rig.mcu.dead = false; // someone pressed the reset button
	ASSERT_TRUE(rig.sup->request(acq::Event::Reset).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Running, 2s));
	EXPECT_TRUE(rig.samples_advance(1s));
}

TEST(Supervisor, AcquisitionThreadDeathIsAFaultAndResetRestartsIt) {
	Rig rig;
	rig.start_running();
	rig.mcu.close_pipe();
	ASSERT_TRUE(rig.sup->wait_for_state(State::Fault, 2s));
	EXPECT_NE(rig.sup->status()["last_fault"]["reason"].get<std::string>().find("acquisition thread"),
		  std::string::npos);

	rig.mcu.reopen_pipe();
	ASSERT_TRUE(rig.sup->request(acq::Event::Reset).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Running, 2s));
	EXPECT_FALSE(rig.worker.failed());
	EXPECT_TRUE(rig.samples_advance(1s));
}

TEST(Supervisor, CalibrationMeasuresDriftAndPersistsIt) {
	acq::SupervisorConfig cfg;
	cfg.calibration_duration = 600ms;
	cfg.calibration_min_samples = 100;
	Rig rig(cfg);
	rig.mcu.drift_ppm = -64.42;
	ASSERT_TRUE(rig.sup->wait_for_state(State::Ready, 1s));
	ASSERT_TRUE(rig.sup->request(acq::Event::Calibrate).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Calibrating, 1s));
	EXPECT_FALSE(rig.sup->request(acq::Event::Pause).ok); // can't race it
	ASSERT_TRUE(rig.sup->wait_for_state(State::Ready, 3s));
	EXPECT_EQ(rig.mcu.get("control"), "0");

	const auto st = rig.sup->status();
	ASSERT_TRUE(st.contains("last_calibration"));
	ASSERT_TRUE(st["last_calibration"]["ok"].get<bool>()) << st.dump();
	EXPECT_NEAR(st["last_calibration"]["drift_ppm"].get<double>(), -64.42, 0.5);

	nlohmann::json saved;
	std::ifstream(rig.mcu.dir() + "/state/calibration.json") >> saved;
	EXPECT_NEAR(saved["drift_ppm"].get<double>(), -64.42, 0.5);
}

TEST(Supervisor, FailedCalibrationIsNotAFault) {
	acq::SupervisorConfig cfg;
	cfg.calibration_duration = 300ms;
	cfg.calibration_min_samples = 1'000'000; // unreachable
	Rig rig(cfg);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Ready, 1s));
	ASSERT_TRUE(rig.sup->request(acq::Event::Calibrate).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Calibrating, 1s));
	ASSERT_TRUE(rig.sup->wait_for_state(State::Ready, 3s));
	const auto st = rig.sup->status();
	EXPECT_FALSE(st["last_calibration"]["ok"].get<bool>());
	EXPECT_EQ(st["faults"], 0);
}

TEST(Supervisor, StopInterruptsALongProcedure) {
	acq::SupervisorConfig cfg;
	cfg.calibration_duration = 60s;
	Rig rig(cfg);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Ready, 1s));
	ASSERT_TRUE(rig.sup->request(acq::Event::Calibrate).ok);
	ASSERT_TRUE(rig.sup->wait_for_state(State::Calibrating, 1s));
	const auto t0 = std::chrono::steady_clock::now();
	rig.sup->stop();
	EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
	EXPECT_EQ(rig.sup->state(), State::Stopped);
	EXPECT_EQ(rig.mcu.get("control"), "0");
}

TEST(Supervisor, HistoryRecordsEveryTransitionWithItsReason) {
	Rig rig;
	rig.start_running();
	rig.mcu.stuck = true;
	ASSERT_TRUE(rig.sup->wait_for_state(State::Recovering, 2s));
	ASSERT_TRUE(rig.sup->wait_for_state(State::Running, 2s));
	const auto h = rig.sup->status()["history"];
	ASSERT_GE(h.size(), 4u);
	EXPECT_EQ(h[0]["to"], "Ready");
	EXPECT_EQ(h[1]["to"], "Running");
	EXPECT_EQ(h[2]["event"], "Stall");
	EXPECT_EQ(h[3]["event"], "Recovered");
}

// The control socket's permission split: device-monitor connects as a
// different uid in the service's group, and must be able to read status
// but not change anything.
TEST(Commands, ReadOnlyPeerGetsStatusButCannotChangeState) {
	Rig rig;
	rig.start_running();
	const ucred monitor{.pid = 4242, .uid = ::geteuid() + 1000, .gid = 0};
	auto status = nlohmann::json::parse(acq::handle_command(*rig.sup, "status", monitor));
	EXPECT_TRUE(status["ok"].get<bool>());
	EXPECT_EQ(status["state"], "Running");

	for (const char* cmd : {"pause", "reset", "calibrate", "start", "resume"}) {
		auto r = nlohmann::json::parse(acq::handle_command(*rig.sup, cmd, monitor));
		EXPECT_FALSE(r["ok"].get<bool>()) << cmd;
		EXPECT_NE(r["error"].get<std::string>().find("permission denied"), std::string::npos) << cmd;
	}
	EXPECT_EQ(rig.sup->state(), State::Running);
}

TEST(Commands, OwnUidAndRootMayChangeState) {
	Rig rig;
	rig.start_running();
	const ucred self{.pid = ::getpid(), .uid = ::geteuid(), .gid = 0};
	EXPECT_TRUE(nlohmann::json::parse(acq::handle_command(*rig.sup, "pause", self))["ok"].get<bool>());
	ASSERT_TRUE(rig.sup->wait_for_state(State::Paused, 1s));
	const ucred root{.pid = 1, .uid = 0, .gid = 0};
	EXPECT_TRUE(nlohmann::json::parse(acq::handle_command(*rig.sup, "resume", root))["ok"].get<bool>());
	ASSERT_TRUE(rig.sup->wait_for_state(State::Running, 1s));
}

TEST(Commands, UnknownCommandIsAnErrorNotACrash) {
	Rig rig;
	const ucred self{.pid = ::getpid(), .uid = ::geteuid(), .gid = 0};
	auto r = nlohmann::json::parse(acq::handle_command(*rig.sup, "rm -rf /", self));
	EXPECT_FALSE(r["ok"].get<bool>());
}
