// devbus-replay FILE [--service NAME] [--speed X] [--subscribers N] [--wait-ms N]
// Republishes a recording: same payload type, same bytes, the recorded
// timing scaled by --speed (1 = original, 0 = as fast as possible).
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "devbus/record.hpp"

static std::atomic<bool> g_stop{false};

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s FILE [--service NAME] [--speed X] [--subscribers N] [--wait-ms N]\n", argv[0]);
		return 2;
	}
	std::string service;
	double speed = 1.0;
	unsigned subscribers = 0;
	long wait_ms = 5000;
	for (int i = 2; i + 1 < argc; i += 2) {
		if (!std::strcmp(argv[i], "--service"))
			service = argv[i + 1];
		else if (!std::strcmp(argv[i], "--speed"))
			speed = std::atof(argv[i + 1]);
		else if (!std::strcmp(argv[i], "--subscribers"))
			subscribers = static_cast<unsigned>(std::atoi(argv[i + 1]));
		else if (!std::strcmp(argv[i], "--wait-ms"))
			wait_ms = std::atol(argv[i + 1]);
	}
	std::signal(SIGINT, [](int) { g_stop = true; });
	std::signal(SIGTERM, [](int) { g_stop = true; });
	try {
		devbus::Replayer rp(argv[1]);
		const std::string target = service.empty() ? rp.recorded_service() : service;
		std::printf("replaying %s as '%s' (payload %zu bytes) at speed %g\n", argv[1], target.c_str(), rp.type().size, speed);
		const auto st = rp.run(target, speed, {}, subscribers, std::chrono::milliseconds(wait_ms), [] { return !g_stop; });
		std::printf("sent %llu, loan failures %llu, lateness mean %.1f us max %.1f us\n",
			    static_cast<unsigned long long>(st.sent), static_cast<unsigned long long>(st.loan_failures),
			    st.mean_lateness_us, st.max_lateness_us);
		return st.loan_failures ? 1 : 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "devbus-replay: %s\n", e.what());
		return 2;
	}
}
