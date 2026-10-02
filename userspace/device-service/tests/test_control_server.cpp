#include <gtest/gtest.h>

#include <string>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "control_server.hpp"

namespace {
std::string sock_path() {
	return "/tmp/ds-ctl-test-" + std::to_string(::getpid()) + ".sock";
}

std::string send(const std::string& path, const std::string& cmd) {
	int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
	if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		::close(fd);
		return "connect failed";
	}
	(void)!::write(fd, cmd.data(), cmd.size());
	std::string out;
	char buf[256];
	ssize_t n;
	while ((n = ::read(fd, buf, sizeof(buf))) > 0)
		out.append(buf, static_cast<std::size_t>(n));
	::close(fd);
	return out;
}
} // namespace

TEST(ControlServer, OneCommandOneReply) {
	const auto path = sock_path();
	acq::ControlServer srv(path, [](const std::string& cmd, const ucred&) { return "got:" + cmd; });
	srv.start();
	EXPECT_EQ(send(path, "status\n"), "got:status\n");
	EXPECT_EQ(send(path, "pause\r\n"), "got:pause\n");
	srv.stop();
}

TEST(ControlServer, HandlerSeesThePeersCredentials) {
	const auto path = sock_path();
	acq::ControlServer srv(path, [](const std::string&, const ucred& peer) {
		return std::to_string(peer.uid) + "/" + std::to_string(peer.pid);
	});
	srv.start();
	EXPECT_EQ(send(path, "status\n"), std::to_string(::getuid()) + "/" + std::to_string(::getpid()) + "\n");
	srv.stop();
}

TEST(ControlServer, SocketIsGroupOnlyAndRemovedOnStop) {
	const auto path = sock_path();
	acq::ControlServer srv(path, [](const std::string&, const ucred&) { return "x"; });
	srv.start();
	struct stat st{};
	ASSERT_EQ(::stat(path.c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0660u);
	srv.stop();
	EXPECT_NE(::stat(path.c_str(), &st), 0);
}

TEST(ControlServer, ReplacesAStaleSocketFile) {
	const auto path = sock_path();
	{
		acq::ControlServer first(path, [](const std::string&, const ucred&) { return "1"; });
		first.start();
		// Simulate a crash: the file stays behind because stop() never runs
		// to unlink it. Here we just leave it by copying the path aside.
	}
	// A plain leftover file at the path must not stop the next start().
	FILE* f = std::fopen(path.c_str(), "w");
	ASSERT_NE(f, nullptr);
	std::fclose(f);
	acq::ControlServer second(path, [](const std::string&, const ucred&) { return "2"; });
	ASSERT_NO_THROW(second.start());
	EXPECT_EQ(send(path, "x\n"), "2\n");
}
