#pragma once

#include <optional>
#include <string_view>

namespace acq {

// Plan.md V2/M5: the machine-level state of the acquisition device, as
// opposed to the per-component state each class already keeps (a running
// thread, a counter). Before M5 there was no single answer to "what is
// this device doing right now" - the watchdog could be soft-resetting the
// MCU while the backpressure controller was rewriting its sample rate and
// nobody owned the overall picture. Supervisor now owns it, and every
// change goes through transition() below.
enum class State {
	Init,        // process started, device not yet probed
	Ready,       // probed and healthy, acquisition not started
	Running,     // acquiring
	Paused,      // acquisition stopped on request; resumable
	Calibrating, // a managed calibration procedure owns the device
	Recovering,  // a stall or operator reset is being worked on
	Fault,       // latched: needs an operator reset (or a power cycle)
	Stopped,     // shutting down; terminal
};

enum class Event {
	InitOk,          // startup probe succeeded
	Start,           // operator / autostart
	Pause,           // operator
	Resume,          // operator
	Calibrate,       // operator
	CalibrationDone, // calibration finished, successfully or not
	Stall,           // watchdog: no sample within the liveness timeout
	DeviceError,     // an I/O operation on the device failed outright
	Recovered,       // recovery saw samples flowing again
	RecoveryFailed,  // recovery ran out of attempts
	Reset,           // operator, from Fault
	Stop,            // SIGTERM / SIGINT
};

// The whole transition table in one pure function, so it can be read and
// unit-tested in isolation (tests/test_device_state.cpp). nullopt means
// the event is not allowed in that state - the caller rejects it rather
// than guessing, e.g. "pause" while Recovering is refused instead of
// silently racing the recovery procedure.
std::optional<State> transition(State from, Event event);

std::string_view to_string(State s);
std::string_view to_string(Event e);

} // namespace acq
