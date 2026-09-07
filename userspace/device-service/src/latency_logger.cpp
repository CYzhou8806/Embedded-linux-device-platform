#include "latency_logger.hpp"

namespace acq {

LatencyLogger::LatencyLogger(const std::string& path) {
	if (path.empty())
		return;
	file_.open(path, std::ios::out | std::ios::trunc);
	if (file_)
		file_ << "seq,irq_ts_ns,recv_ts_ns,latency_ns\n";
}

void LatencyLogger::log(uint32_t seq, int64_t irq_ts_ns, int64_t recv_ts_ns) {
	if (!file_.is_open())
		return;
	file_ << seq << ',' << irq_ts_ns << ',' << recv_ts_ns << ',' << (recv_ts_ns - irq_ts_ns) << '\n';
}

} // namespace acq
