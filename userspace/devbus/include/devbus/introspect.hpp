#pragma once

// Read-only view of every devbus service on this machine, straight from
// shared memory - no cooperation from the publisher or subscribers needed.
// This is what tools/devbus_ls.cpp prints, and what a metrics exporter
// would scrape.

#include <cstdint>
#include <string>
#include <vector>

namespace devbus {

struct SubscriberInfo {
	uint32_t slot = 0;
	int pid = 0;
	std::string state;    // "active", "claimed", "closing"
	std::string overflow; // "drop-oldest", "drop-newest", "block"
	uint64_t queued = 0;  // samples waiting in its queue right now
	uint64_t delivered = 0;
	uint64_t dropped_oldest = 0;
	uint64_t dropped_newest = 0;
	uint64_t block_timeouts = 0;
	uint64_t blocked_ns = 0;
};

struct ServiceInfo {
	std::string shm_name;
	int publisher_pid = 0;
	bool publisher_alive = false;
	uint32_t type_size = 0;
	uint32_t queue_capacity = 0;
	uint32_t chunk_count = 0;
	uint64_t segment_bytes = 0;
	uint64_t published = 0;
	uint64_t loan_failures = 0;
	std::vector<SubscriberInfo> subscribers; // non-free slots only
};

std::vector<ServiceInfo> list_services();

} // namespace devbus
