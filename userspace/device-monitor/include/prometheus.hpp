#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace devmon {

// device-service's `status` JSON (Supervisor::status()) in Prometheus text
// exposition format. Pure, so the mapping is unit-tested on its own.
// Counters keep their names from the status JSON with a _total suffix;
// the machine state is one gauge per state with the current one at 1, the
// usual way to export an enum so an alert can be `device_state{state="Fault"} == 1`.
std::string to_prometheus(const nlohmann::json& status);

} // namespace devmon
