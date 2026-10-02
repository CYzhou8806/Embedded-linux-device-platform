#include "commands.hpp"

#include <unistd.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace acq {

std::optional<Event> parse_command(const std::string& cmd) {
	if (cmd == "start")
		return Event::Start;
	if (cmd == "pause")
		return Event::Pause;
	if (cmd == "resume")
		return Event::Resume;
	if (cmd == "calibrate")
		return Event::Calibrate;
	if (cmd == "reset")
		return Event::Reset;
	return std::nullopt;
}

// Read-only for anyone who could connect at all (root and the socket's
// group - the directory and socket permissions decide that); changing the
// machine's state only for root and this service's own uid. That split
// is what lets device-monitor, a separate unprivileged process with
// network access, serve status remotely without being able to pause or
// reset anything (docs/security/remote-monitoring.md).
std::string handle_command(Supervisor& supervisor, const std::string& cmd, const ucred& peer) {
	if (cmd == "status") {
		nlohmann::json j = supervisor.status();
		j["ok"] = true;
		return j.dump();
	}
	const auto event = parse_command(cmd);
	if (!event)
		return nlohmann::json{{"ok", false}, {"error", "unknown command '" + cmd + "'"}}.dump();
	if (peer.uid != 0 && peer.uid != ::geteuid()) {
		spdlog::warn("control: refused '{}' from uid {} pid {} (read-only peer)", cmd, peer.uid, peer.pid);
		return nlohmann::json{{"ok", false}, {"error", "permission denied: '" + cmd + "' changes state"}}.dump();
	}
	const auto reply = supervisor.request(*event, "operator: " + cmd);
	return nlohmann::json{{"ok", reply.ok}, {"message", reply.message}}.dump();
}

} // namespace acq
