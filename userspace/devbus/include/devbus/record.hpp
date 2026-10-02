#pragma once

// Record and replay a devbus service (roadmap item 4; Plan.md V2/M4's
// "record and replay"). Both work on raw bytes, without knowing the
// payload type: the type's identity (hash, size, alignment) is read from
// the live service and stored in the file, and replay creates a service
// with exactly that identity. Subscribers built against the original type
// therefore open a replayed service unchanged - and ones built against a
// different type are refused, the same as with a live publisher.
//
// The point of it on this device: a captured run of real acquisition data
// can be fed back through every consumer (and later a model) without the
// MCU, the camera or the bench being on - reproducibly, with the original
// timing or as fast as possible.
//
// File format, native byte order (same machine or same architecture):
//   FileHeader, then service name (name_len bytes), then records of
//   { uint64 seq; int64 publish_ns; payload[type_size] }.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "devbus/config.hpp"
#include "devbus/detail/core.hpp"

namespace devbus {

struct RecordFileHeader {
	char magic[8];          // "DBUSREC1"
	uint32_t version;       // 1
	uint32_t type_size;
	uint64_t type_hash;
	uint32_t type_align;
	uint32_t name_len;
};

// The payload type of a running service, as its publisher declared it.
// Throws devbus::Error if there is no such service.
TypeInfo service_type(std::string_view service);

struct RecordStats {
	uint64_t samples = 0;
	uint64_t gaps = 0; // samples the recorder never saw (Block timed out)
	uint64_t bytes = 0;
};

// A subscriber that appends everything it receives to a file. It uses the
// Block overflow policy: if the disk falls behind, the publisher waits up
// to block_timeout before dropping - so a recording is complete unless a
// gap is reported, never silently partial.
class Recorder {
public:
	Recorder(std::string_view service, const std::string& path,
		 std::chrono::microseconds block_timeout = std::chrono::milliseconds(10));
	~Recorder();
	Recorder(const Recorder&) = delete;
	Recorder& operator=(const Recorder&) = delete;

	// Waits up to `timeout` for data, then writes everything queued.
	// Returns the number of samples written by this call.
	std::size_t poll(std::chrono::nanoseconds timeout);
	// Flushes and fsyncs. Called by the destructor if not called before.
	void close();

	bool publisher_alive() const noexcept { return core_->publisher_alive(); }
	RecordStats stats() const noexcept;

private:
	TypeInfo type_;
	std::unique_ptr<detail::SubscriberCore> core_;
	std::FILE* file_ = nullptr;
	RecordStats stats_;
};

struct ReplayStats {
	uint64_t sent = 0;
	uint64_t loan_failures = 0;
	// How far the replay drifted from the recorded timing at worst, and on
	// average. Only meaningful when speed > 0.
	double max_lateness_us = 0;
	double mean_lateness_us = 0;
};

class Replayer {
public:
	// Reads and checks the header. Throws devbus::Error on a bad file.
	explicit Replayer(const std::string& path);
	~Replayer();
	Replayer(const Replayer&) = delete;
	Replayer& operator=(const Replayer&) = delete;

	const TypeInfo& type() const noexcept { return type_; }
	const std::string& recorded_service() const noexcept { return service_; }

	// Publishes every record on `service` (the recorded name if empty).
	// speed 1.0 = the recorded inter-sample timing, 2.0 = twice as fast,
	// 0 = as fast as the subscribers take it. Before the first sample it
	// waits up to `wait_for` for `subscribers` subscribers to attach; after
	// the last, until their queues are empty (or `wait_for` passes), so a
	// consumer is never cut off mid-stream by the publisher going away.
	// keep_going is polled between samples (SIGINT handling in the tool).
	ReplayStats run(std::string_view service, double speed, const ServiceConfig& cfg = {},
			uint32_t subscribers = 0, std::chrono::milliseconds wait_for = std::chrono::seconds(5),
			const std::function<bool()>& keep_going = nullptr);

private:
	std::FILE* file_ = nullptr;
	TypeInfo type_{};
	std::string service_;
};

} // namespace devbus
