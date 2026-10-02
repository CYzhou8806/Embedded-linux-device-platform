#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <sys/socket.h>

namespace acq {

// The control channel (Plan.md V2/M5): a Unix stream socket taking one
// text command per connection ("status", "pause", "calibrate"...) and
// answering with one line of JSON. Deliberately separate from the data
// path - samples go out on devbus shared memory, commands come in here -
// the same control/data split as the middleware this design borrows from
// (Plan.md 12.3): a flood of data never delays a command, and a slow
// operator never touches the data path.
//
// Who may connect: the socket file is 0660 in a 0750 directory, so only
// root and members of the service's group get as far as connect(). What
// each of them may then *do* is the handler's decision: it gets the
// peer's SO_PEERCRED along with the command. main.cpp lets the group read
// status (that is how device-monitor, a separate unprivileged process,
// gets it) and keeps every command that changes state to root and the
// service's own uid. AF_UNIX only, which is all the production sandbox
// allows anyway (RestrictAddressFamilies=AF_UNIX).
//
// Transport only: what a command means is entirely the handler's business.
class ControlServer {
public:
	using Handler = std::function<std::string(const std::string& command, const ucred& peer)>;

	ControlServer(std::string path, Handler handler);
	~ControlServer();

	// Throws std::runtime_error if the socket can't be created/bound.
	void start();
	void stop();

private:
	void run();
	void serve(int client);

	std::string path_;
	Handler handler_;
	int listen_fd_ = -1;
	std::atomic<bool> stop_requested_{false};
	std::thread thread_;
};

} // namespace acq
