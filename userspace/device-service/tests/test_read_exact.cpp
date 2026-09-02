#include "read_exact.hpp"
#include "device.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using acq::DeviceError;
using acq::read_exact;

namespace {

// RAII pipe: [0] is the read end (what read_exact() reads from), [1] is
// the write end the test uses to feed bytes in.
struct Pipe {
	int fds[2];
	Pipe() { EXPECT_EQ(::pipe(fds), 0); }
	~Pipe() {
		if (fds[0] >= 0) ::close(fds[0]);
		if (fds[1] >= 0) ::close(fds[1]);
	}
	int read_end() const { return fds[0]; }
	int write_end() const { return fds[1]; }
};

} // namespace

TEST(ReadExact, ReadsAWholeBufferWrittenInOneShot) {
	Pipe p;
	const char msg[] = "01234567";
	ASSERT_EQ(::write(p.write_end(), msg, 8), 8);

	char buf[8] = {};
	read_exact(p.read_end(), buf, sizeof(buf), "test-pipe");
	EXPECT_EQ(std::string(buf, 8), "01234567");
}

TEST(ReadExact, ReassemblesBytesArrivingInSeparatePieces) {
	Pipe p;
	// Simulates a partial ::read() - e.g. only half the sample has
	// arrived by the time the syscall returns.
	ASSERT_EQ(::write(p.write_end(), "abcd", 4), 4);
	ASSERT_EQ(::write(p.write_end(), "efgh", 4), 4);

	char buf[8] = {};
	read_exact(p.read_end(), buf, sizeof(buf), "test-pipe");
	EXPECT_EQ(std::string(buf, 8), "abcdefgh");
}

TEST(ReadExact, ThrowsOnPrematureEof) {
	Pipe p;
	ASSERT_EQ(::write(p.write_end(), "abcd", 4), 4);
	::close(p.fds[1]);
	p.fds[1] = -1; // EOF on the read end: writer closed with only 4/8 bytes sent

	char buf[8] = {};
	EXPECT_THROW(read_exact(p.read_end(), buf, sizeof(buf), "test-pipe"), DeviceError);
}
