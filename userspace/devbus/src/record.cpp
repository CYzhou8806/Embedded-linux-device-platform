#include "devbus/record.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "devbus/detail/layout.hpp"
#include "devbus/detail/os.hpp"

namespace devbus {

namespace {

constexpr char kRecordMagic[8] = {'D', 'B', 'U', 'S', 'R', 'E', 'C', '1'};

struct RecordPrefix {
	uint64_t seq;
	int64_t publish_ns;
};

[[noreturn]] void fail(const std::string& what) {
	throw Error(what + (errno ? std::string(": ") + std::strerror(errno) : std::string()));
}

} // namespace

TypeInfo service_type(std::string_view service) {
	// Read-only peek at the control segment's header: the recorder learns
	// the type the same way a typed subscriber's check would see it.
	detail::ShmSegment seg = detail::ShmSegment::open(detail::shm_name(service), /*writable=*/false);
	if (seg.size() < sizeof(detail::SegmentHeader))
		throw Error("devbus: segment for '" + std::string(service) + "' is truncated");
	const auto* h = std::launder(reinterpret_cast<const detail::SegmentHeader*>(seg.data()));
	if (h->magic != detail::kMagic || h->layout_version != detail::kLayoutVersion)
		throw Error("devbus: '" + std::string(service) + "' is not a compatible devbus segment");
	return TypeInfo{h->type_hash, h->type_size, h->type_align};
}

// ---------------------------------------------------------------------------

Recorder::Recorder(std::string_view service, const std::string& path, std::chrono::microseconds block_timeout)
    : type_(service_type(service)) {
	SubscriberConfig cfg;
	cfg.overflow = Overflow::Block;
	cfg.block_timeout = block_timeout;
	cfg.wait_mode = WaitMode::Futex;
	core_ = std::make_unique<detail::SubscriberCore>(service, type_, cfg);

	errno = 0;
	file_ = std::fopen(path.c_str(), "wbx"); // never overwrite an existing recording
	if (!file_)
		fail("devbus: cannot create recording " + path);
	RecordFileHeader h{};
	std::memcpy(h.magic, kRecordMagic, sizeof(h.magic));
	h.version = 1;
	h.type_size = static_cast<uint32_t>(type_.size);
	h.type_hash = type_.hash;
	h.type_align = static_cast<uint32_t>(type_.align);
	h.name_len = static_cast<uint32_t>(service.size());
	if (std::fwrite(&h, sizeof(h), 1, file_) != 1 || std::fwrite(service.data(), 1, service.size(), file_) != service.size())
		fail("devbus: writing recording header");
	stats_.bytes = sizeof(h) + service.size();
}

Recorder::~Recorder() {
	try {
		close();
	} catch (...) {
	}
}

std::size_t Recorder::poll(std::chrono::nanoseconds timeout) {
	if (!file_)
		return 0;
	core_->wait(timeout);
	std::size_t n = 0;
	uint32_t chunk = 0;
	while (core_->try_receive(chunk) == ReceiveStatus::Ok) {
		const auto& ch = core_->header(chunk);
		const RecordPrefix prefix{ch.seq, ch.publish_ns};
		const bool ok = std::fwrite(&prefix, sizeof(prefix), 1, file_) == 1 &&
				std::fwrite(core_->payload(chunk), 1, type_.size, file_) == type_.size;
		core_->release(chunk); // straight back: the bytes are in stdio's buffer now
		if (!ok)
			fail("devbus: writing recording");
		++n;
	}
	stats_.samples += n;
	stats_.bytes += n * (sizeof(RecordPrefix) + type_.size);
	return n;
}

void Recorder::close() {
	if (!file_)
		return;
	std::FILE* f = std::exchange(file_, nullptr);
	const bool ok = std::fflush(f) == 0 && fsync(fileno(f)) == 0;
	std::fclose(f);
	if (!ok)
		fail("devbus: closing recording");
}

RecordStats Recorder::stats() const noexcept {
	RecordStats s = stats_;
	s.gaps = core_->observed_gaps();
	return s;
}

// ---------------------------------------------------------------------------

Replayer::Replayer(const std::string& path) {
	errno = 0;
	file_ = std::fopen(path.c_str(), "rb");
	if (!file_)
		fail("devbus: cannot open recording " + path);
	RecordFileHeader h{};
	if (std::fread(&h, sizeof(h), 1, file_) != 1 || std::memcmp(h.magic, kRecordMagic, sizeof(h.magic)) != 0 ||
	    h.version != 1 || h.type_size == 0 || h.name_len > 4096) {
		std::fclose(file_);
		file_ = nullptr;
		errno = 0;
		fail("devbus: " + path + " is not a devbus recording");
	}
	service_.resize(h.name_len);
	if (std::fread(service_.data(), 1, h.name_len, file_) != h.name_len) {
		std::fclose(file_);
		file_ = nullptr;
		errno = 0;
		fail("devbus: " + path + " is truncated");
	}
	type_ = TypeInfo{h.type_hash, h.type_size, h.type_align};
}

Replayer::~Replayer() {
	if (file_)
		std::fclose(file_);
}

ReplayStats Replayer::run(std::string_view service, double speed, const ServiceConfig& cfg, uint32_t subscribers,
			  std::chrono::milliseconds wait_for, const std::function<bool()>& keep_going) {
	if (service.empty())
		service = service_;
	detail::PublisherCore pub(service, type_, cfg);
	ReplayStats st;

	auto deadline = detail::monotonic_ns() + std::chrono::nanoseconds(wait_for).count();
	while (pub.active_subscribers() < subscribers && detail::monotonic_ns() < deadline) {
		pub.check_liveness(); // also onboards newly claimed slots
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::vector<std::byte> payload(type_.size);
	RecordPrefix prefix{};
	int64_t first_recorded = 0, start = 0;
	double lateness_sum = 0;
	while (std::fread(&prefix, sizeof(prefix), 1, file_) == 1 &&
	       std::fread(payload.data(), 1, payload.size(), file_) == payload.size()) {
		if (keep_going && !keep_going())
			break;
		if (speed > 0) {
			if (st.sent == 0) {
				first_recorded = prefix.publish_ns;
				start = detail::monotonic_ns();
			}
			const int64_t target = start + static_cast<int64_t>(static_cast<double>(prefix.publish_ns - first_recorded) / speed);
			int64_t now = detail::monotonic_ns();
			if (target > now + 200'000)
				std::this_thread::sleep_for(std::chrono::nanoseconds(target - now - 100'000));
			while ((now = detail::monotonic_ns()) < target) {
			} // the last stretch spun: sleep() alone overshoots by tens of us
			const double late = static_cast<double>(now - target) / 1000.0;
			lateness_sum += late;
			st.max_lateness_us = std::max(st.max_lateness_us, late);
		}
		auto chunk = pub.loan();
		if (!chunk) {
			++st.loan_failures;
			continue;
		}
		std::memcpy(pub.payload(*chunk), payload.data(), payload.size());
		pub.send(*chunk);
		++st.sent;
	}
	if (st.sent && speed > 0)
		st.mean_lateness_us = lateness_sum / static_cast<double>(st.sent);

	// Let the subscribers finish before the publisher - and with it the
	// service - goes away.
	deadline = detail::monotonic_ns() + std::chrono::nanoseconds(wait_for).count();
	while (pub.max_queued() > 0 && detail::monotonic_ns() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	return st;
}

} // namespace devbus
