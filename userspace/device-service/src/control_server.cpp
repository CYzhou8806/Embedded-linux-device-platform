#include "control_server.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace acq {

namespace {
constexpr int kAcceptPollMs = 200;
constexpr int kClientTimeoutMs = 2000;
constexpr std::size_t kMaxCommand = 256;
} // namespace

ControlServer::ControlServer(std::string path, Handler handler) : path_(std::move(path)), handler_(std::move(handler)) {}

ControlServer::~ControlServer() {
	stop();
}

void ControlServer::start() {
	sockaddr_un addr{};
	if (path_.size() >= sizeof(addr.sun_path))
		throw std::runtime_error("control socket path too long: " + path_);

	listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listen_fd_ < 0)
		throw std::runtime_error(std::string("control socket: ") + std::strerror(errno));

	// A stale socket file from a previous run (crash, SIGKILL) would make
	// bind() fail with EADDRINUSE forever.
	::unlink(path_.c_str());
	addr.sun_family = AF_UNIX;
	std::memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);
	if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
	    ::chmod(path_.c_str(), 0660) != 0 || ::listen(listen_fd_, 4) != 0) {
		const std::string err = std::strerror(errno);
		::close(listen_fd_);
		listen_fd_ = -1;
		throw std::runtime_error("control socket " + path_ + ": " + err);
	}
	thread_ = std::thread(&ControlServer::run, this);
}

void ControlServer::stop() {
	stop_requested_ = true;
	if (thread_.joinable())
		thread_.join();
	if (listen_fd_ >= 0) {
		::close(listen_fd_);
		listen_fd_ = -1;
		::unlink(path_.c_str());
	}
}

void ControlServer::run() {
	while (!stop_requested_) {
		pollfd pfd{listen_fd_, POLLIN, 0};
		const int ret = ::poll(&pfd, 1, kAcceptPollMs);
		if (ret <= 0)
			continue; // timeout (re-check stop) or EINTR
		const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
		if (client < 0)
			continue;
		serve(client);
		::close(client);
	}
}

void ControlServer::serve(int client) {
	ucred cred{};
	socklen_t len = sizeof(cred);
	if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
		spdlog::warn("control: SO_PEERCRED failed, dropping connection");
		return;
	}

	// One command per connection, newline- or EOF-terminated. A client
	// that connects and says nothing is dropped after a timeout rather
	// than holding the (single) control thread.
	std::string command;
	char buf[64];
	while (command.size() < kMaxCommand && command.find('\n') == std::string::npos) {
		pollfd pfd{client, POLLIN, 0};
		if (::poll(&pfd, 1, kClientTimeoutMs) <= 0)
			break;
		const ssize_t n = ::read(client, buf, sizeof(buf));
		if (n <= 0)
			break;
		command.append(buf, static_cast<std::size_t>(n));
	}
	if (auto nl = command.find_first_of("\r\n"); nl != std::string::npos)
		command.resize(nl);

	std::string reply = handler_(command, cred);
	reply.push_back('\n');
	std::size_t off = 0;
	while (off < reply.size()) {
		const ssize_t n = ::write(client, reply.data() + off, reply.size() - off);
		if (n <= 0)
			break;
		off += static_cast<std::size_t>(n);
	}
}

} // namespace acq
