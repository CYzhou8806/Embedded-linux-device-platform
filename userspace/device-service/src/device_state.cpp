#include "device_state.hpp"

namespace acq {

std::optional<State> transition(State from, Event event) {
	// Stop is accepted everywhere except where we already are: shutdown
	// must never be refused because of what the device happens to be doing.
	if (event == Event::Stop)
		return from == State::Stopped ? std::nullopt : std::optional<State>(State::Stopped);

	switch (from) {
	case State::Init:
		if (event == Event::InitOk)
			return State::Ready;
		break;
	case State::Ready:
		switch (event) {
		case Event::Start: return State::Running;
		case Event::Calibrate: return State::Calibrating;
		case Event::DeviceError: return State::Fault;
		default: break;
		}
		break;
	case State::Running:
		switch (event) {
		case Event::Pause: return State::Paused;
		case Event::Stall: return State::Recovering;
		case Event::DeviceError: return State::Fault;
		default: break;
		}
		break;
	case State::Paused:
		switch (event) {
		case Event::Resume: return State::Running;
		case Event::Calibrate: return State::Calibrating;
		case Event::DeviceError: return State::Fault;
		default: break;
		}
		break;
	case State::Calibrating:
		// A calibration that fails (too few samples, a sequence gap) is
		// not a device fault - it reports its own failure and lands back
		// in Ready like a successful one. Only an outright I/O error
		// escalates.
		switch (event) {
		case Event::CalibrationDone: return State::Ready;
		case Event::DeviceError: return State::Fault;
		default: break;
		}
		break;
	case State::Recovering:
		// DeviceError is deliberately absent: recovery expects the device
		// to misbehave and handles that itself, by spending an attempt.
		switch (event) {
		case Event::Recovered: return State::Running;
		case Event::RecoveryFailed: return State::Fault;
		default: break;
		}
		break;
	case State::Fault:
		if (event == Event::Reset)
			return State::Recovering;
		break;
	case State::Stopped:
		break;
	}
	return std::nullopt;
}

std::string_view to_string(State s) {
	switch (s) {
	case State::Init: return "Init";
	case State::Ready: return "Ready";
	case State::Running: return "Running";
	case State::Paused: return "Paused";
	case State::Calibrating: return "Calibrating";
	case State::Recovering: return "Recovering";
	case State::Fault: return "Fault";
	case State::Stopped: return "Stopped";
	}
	return "?";
}

std::string_view to_string(Event e) {
	switch (e) {
	case Event::InitOk: return "InitOk";
	case Event::Start: return "Start";
	case Event::Pause: return "Pause";
	case Event::Resume: return "Resume";
	case Event::Calibrate: return "Calibrate";
	case Event::CalibrationDone: return "CalibrationDone";
	case Event::Stall: return "Stall";
	case Event::DeviceError: return "DeviceError";
	case Event::Recovered: return "Recovered";
	case Event::RecoveryFailed: return "RecoveryFailed";
	case Event::Reset: return "Reset";
	case Event::Stop: return "Stop";
	}
	return "?";
}

} // namespace acq
