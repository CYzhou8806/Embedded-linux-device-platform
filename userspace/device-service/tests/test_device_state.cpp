#include <gtest/gtest.h>

#include "device_state.hpp"

using acq::Event;
using acq::State;
using acq::transition;

TEST(DeviceState, HappyPath) {
	EXPECT_EQ(transition(State::Init, Event::InitOk), State::Ready);
	EXPECT_EQ(transition(State::Ready, Event::Start), State::Running);
	EXPECT_EQ(transition(State::Running, Event::Pause), State::Paused);
	EXPECT_EQ(transition(State::Paused, Event::Resume), State::Running);
}

TEST(DeviceState, StopIsAcceptedFromEveryLiveState) {
	for (State s : {State::Init, State::Ready, State::Running, State::Paused, State::Calibrating, State::Recovering,
			State::Fault})
		EXPECT_EQ(transition(s, Event::Stop), State::Stopped) << to_string(s);
	EXPECT_FALSE(transition(State::Stopped, Event::Stop));
}

TEST(DeviceState, StoppedIsTerminal) {
	for (Event e : {Event::InitOk, Event::Start, Event::Resume, Event::Reset, Event::Recovered})
		EXPECT_FALSE(transition(State::Stopped, e)) << to_string(e);
}

TEST(DeviceState, StallOnlyMattersWhileRunning) {
	EXPECT_EQ(transition(State::Running, Event::Stall), State::Recovering);
	// Silence is expected in these - the watchdog is disarmed there too,
	// but a Stall that was already queued must still be dropped.
	for (State s : {State::Ready, State::Paused, State::Calibrating, State::Recovering, State::Fault})
		EXPECT_FALSE(transition(s, Event::Stall)) << to_string(s);
}

TEST(DeviceState, RecoveryEndsRunningOrLatchedFault) {
	EXPECT_EQ(transition(State::Recovering, Event::Recovered), State::Running);
	EXPECT_EQ(transition(State::Recovering, Event::RecoveryFailed), State::Fault);
	// Recovery handles device errors itself by spending an attempt.
	EXPECT_FALSE(transition(State::Recovering, Event::DeviceError));
}

TEST(DeviceState, FaultIsLatchedUntilReset) {
	for (Event e : {Event::Start, Event::Resume, Event::Calibrate, Event::Stall, Event::Recovered})
		EXPECT_FALSE(transition(State::Fault, e)) << to_string(e);
	EXPECT_EQ(transition(State::Fault, Event::Reset), State::Recovering);
}

TEST(DeviceState, OperatorCannotRaceRecoveryOrCalibration) {
	for (State s : {State::Recovering, State::Calibrating})
		for (Event e : {Event::Pause, Event::Resume, Event::Start, Event::Calibrate})
			EXPECT_FALSE(transition(s, e)) << to_string(s) << " " << to_string(e);
}

TEST(DeviceState, CalibrationOnlyFromIdleStatesAndAlwaysEndsReady) {
	EXPECT_EQ(transition(State::Ready, Event::Calibrate), State::Calibrating);
	EXPECT_EQ(transition(State::Paused, Event::Calibrate), State::Calibrating);
	EXPECT_FALSE(transition(State::Running, Event::Calibrate));
	EXPECT_EQ(transition(State::Calibrating, Event::CalibrationDone), State::Ready);
	EXPECT_EQ(transition(State::Calibrating, Event::DeviceError), State::Fault);
}

TEST(DeviceState, DeviceErrorFaultsFromOperationalStates) {
	for (State s : {State::Ready, State::Running, State::Paused, State::Calibrating})
		EXPECT_EQ(transition(s, Event::DeviceError), State::Fault) << to_string(s);
}
