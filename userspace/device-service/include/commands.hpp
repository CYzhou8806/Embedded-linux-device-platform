#pragma once

#include <optional>
#include <string>

#include <sys/socket.h>

#include "device_state.hpp"
#include "supervisor.hpp"

namespace acq {

// The control socket's command set (ControlServer is only the transport).
// "status" for any peer that could connect; the rest change the machine's
// state and are refused unless the peer is root or this process's uid.
std::optional<Event> parse_command(const std::string& cmd);
std::string handle_command(Supervisor& supervisor, const std::string& cmd, const ucred& peer);

} // namespace acq
