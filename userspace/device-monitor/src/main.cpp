// device-monitor: read-only remote monitoring of device-service over mutual
// TLS (Plan.md V2/M7, docs/security/remote-monitoring.md).
//
// A separate process from device-service on purpose. device-service touches
// the hardware and runs with no network at all (PrivateNetwork=yes in its
// sandbox); this process has the network and touches nothing but
// device-service's control socket, where it is a read-only peer - the
// socket refuses it every command that changes state. Compromising the
// network-facing process therefore buys an attacker the status JSON, not
// the device.
//
//   GET /status   device-service's status, as JSON
//   GET /metrics  the same, Prometheus text format
//   GET /healthz  200 if the machine state is Running, 503 otherwise
//
// TLS 1.3 only. A client must present a certificate that chains to
// client_ca_file, carries the clientAuth purpose, and - if crl_file is set -
// is not revoked. No session tickets, so every connection is a full
// handshake and a revocation takes effect on the next one.
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "prometheus.hpp"

namespace {

std::atomic<bool> g_stop{false};

struct Config {
	std::string listen_address = "0.0.0.0";
	int listen_port = 8443;
	std::string cert_file;      // device identity certificate (chain)
	std::string key_file;       // its private key
	std::string client_ca_file; // operator CA: who may connect
	std::string crl_file;       // optional
	std::string control_socket = "/run/device-service/control.sock";
	int timeout_ms = 5000;

	static Config load(const std::string& path) {
		Config c;
		std::ifstream f(path);
		if (!f)
			throw std::runtime_error("cannot read config " + path);
		nlohmann::json j;
		f >> j;
		c.listen_address = j.value("listen_address", c.listen_address);
		c.listen_port = j.value("listen_port", c.listen_port);
		c.cert_file = j.value("cert_file", c.cert_file);
		c.key_file = j.value("key_file", c.key_file);
		c.client_ca_file = j.value("client_ca_file", c.client_ca_file);
		c.crl_file = j.value("crl_file", c.crl_file);
		c.control_socket = j.value("control_socket", c.control_socket);
		c.timeout_ms = j.value("timeout_ms", c.timeout_ms);
		return c;
	}
};

void log(const char* fmt, auto... args) {
	if constexpr (sizeof...(args) == 0)
		std::fputs(fmt, stderr);
	else
		std::fprintf(stderr, fmt, args...);
	std::fputc('\n', stderr);
}

std::string ssl_errors() {
	std::string out;
	char buf[256];
	while (unsigned long e = ERR_get_error()) {
		ERR_error_string_n(e, buf, sizeof(buf));
		if (!out.empty())
			out += "; ";
		out += buf;
	}
	return out.empty() ? "no OpenSSL error queued" : out;
}

SSL_CTX* make_context(const Config& c) {
	SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx)
		throw std::runtime_error("SSL_CTX_new: " + ssl_errors());
	auto fail = [&](const std::string& what) {
		const std::string msg = what + ": " + ssl_errors();
		SSL_CTX_free(ctx);
		throw std::runtime_error(msg);
	};
	if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION))
		fail("TLS 1.3 minimum");
	if (SSL_CTX_use_certificate_chain_file(ctx, c.cert_file.c_str()) != 1)
		fail("certificate " + c.cert_file);
	if (SSL_CTX_use_PrivateKey_file(ctx, c.key_file.c_str(), SSL_FILETYPE_PEM) != 1)
		fail("private key " + c.key_file);
	if (SSL_CTX_check_private_key(ctx) != 1)
		fail("certificate and key do not match");
	if (SSL_CTX_load_verify_locations(ctx, c.client_ca_file.c_str(), nullptr) != 1)
		fail("client CA " + c.client_ca_file);
	// A server certificate presented as a client certificate is refused:
	// the purpose check requires clientAuth in the extended key usage.
	if (SSL_CTX_set_purpose(ctx, X509_PURPOSE_SSL_CLIENT) != 1)
		fail("client purpose");
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
	SSL_CTX_set_num_tickets(ctx, 0);
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
	if (!c.crl_file.empty()) {
		X509_STORE* store = SSL_CTX_get_cert_store(ctx);
		X509_LOOKUP* lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
		if (!lookup || X509_load_crl_file(lookup, c.crl_file.c_str(), X509_FILETYPE_PEM) < 1)
			fail("CRL " + c.crl_file);
		X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
	}
	return ctx;
}

// One request to device-service's control socket.
std::string query_control(const Config& c, const char* command) {
	const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", c.control_socket.c_str());
	timeval tv{c.timeout_ms / 1000, (c.timeout_ms % 1000) * 1000};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
		const std::string err = std::strerror(errno);
		::close(fd);
		throw std::runtime_error("device-service control socket: " + err);
	}
	const std::string line = std::string(command) + "\n";
	(void)!::write(fd, line.data(), line.size());
	std::string reply;
	char buf[4096];
	ssize_t n;
	while ((n = ::read(fd, buf, sizeof(buf))) > 0)
		reply.append(buf, static_cast<std::size_t>(n));
	::close(fd);
	return reply;
}

// Expiry times of the two things that turn this service off when they run
// out, read once at startup: the device certificate (clients stop trusting
// the device) and the CRL (OpenSSL then refuses every client - revocation
// fails closed, docs/security/remote-monitoring.md §4). Exported as
// seconds-remaining gauges so an alert can fire days before either happens.
struct Expiry {
	long long cert_not_after = 0;  // unix time, 0 = unknown
	long long crl_next_update = 0; // unix time, 0 = no CRL
} g_expiry;

long long asn1_to_unix(const ASN1_TIME* t) {
	struct tm tm{};
	if (!t || ASN1_TIME_to_tm(t, &tm) != 1)
		return 0;
	return static_cast<long long>(timegm(&tm));
}

void load_expiry(const Config& c, SSL_CTX* ctx) {
	if (X509* cert = SSL_CTX_get0_certificate(ctx))
		g_expiry.cert_not_after = asn1_to_unix(X509_get0_notAfter(cert));
	if (c.crl_file.empty())
		return;
	if (FILE* f = std::fopen(c.crl_file.c_str(), "r")) {
		if (X509_CRL* crl = PEM_read_X509_CRL(f, nullptr, nullptr, nullptr)) {
			g_expiry.crl_next_update = asn1_to_unix(X509_CRL_get0_nextUpdate(crl));
			X509_CRL_free(crl);
		}
		std::fclose(f);
	}
}

std::string expiry_metrics() {
	const long long now = static_cast<long long>(std::time(nullptr));
	std::string out;
	if (g_expiry.cert_not_after)
		out += "# HELP device_monitor_certificate_expiry_seconds Seconds until the device's TLS certificate expires.\n"
		       "# TYPE device_monitor_certificate_expiry_seconds gauge\n"
		       "device_monitor_certificate_expiry_seconds " + std::to_string(g_expiry.cert_not_after - now) + "\n";
	if (g_expiry.crl_next_update)
		out += "# HELP device_monitor_crl_expiry_seconds Seconds until the CRL's nextUpdate; at 0 every client is refused.\n"
		       "# TYPE device_monitor_crl_expiry_seconds gauge\n"
		       "device_monitor_crl_expiry_seconds " + std::to_string(g_expiry.crl_next_update - now) + "\n";
	return out;
}

std::string response(int code, const char* reason, const char* type, const std::string& body) {
	return "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type +
	       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

std::string route(const Config& c, const std::string& method, const std::string& path, int& code) {
	if (method != "GET") {
		code = 405;
		return response(405, "Method Not Allowed", "text/plain", "read-only: GET only\n");
	}
	if (path != "/status" && path != "/metrics" && path != "/healthz") {
		code = 404;
		return response(404, "Not Found", "text/plain", "try /status, /metrics, /healthz\n");
	}
	nlohmann::json status;
	try {
		status = nlohmann::json::parse(query_control(c, "status"));
	} catch (const std::exception& e) {
		code = 503;
		return response(503, "Service Unavailable", "text/plain", std::string(e.what()) + "\n");
	}
	status.erase("ok");
	if (path == "/status") {
		code = 200;
		return response(200, "OK", "application/json", status.dump(2) + "\n");
	}
	if (path == "/metrics") {
		code = 200;
		return response(200, "OK", "text/plain; version=0.0.4", devmon::to_prometheus(status) + expiry_metrics());
	}
	const std::string state = status.value("state", "?");
	code = state == "Running" ? 200 : 503;
	return response(code, code == 200 ? "OK" : "Service Unavailable", "text/plain", state + "\n");
}

std::string peer_name(SSL* ssl) {
	X509* cert = SSL_get_peer_certificate(ssl);
	if (!cert)
		return "?";
	char cn[256] = "?";
	X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn));
	X509_free(cert);
	return cn;
}

void serve(const Config& c, SSL_CTX* ctx, int client, const std::string& addr) {
	timeval tv{c.timeout_ms / 1000, (c.timeout_ms % 1000) * 1000};
	::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	SSL* ssl = SSL_new(ctx);
	SSL_set_fd(ssl, client);
	if (SSL_accept(ssl) != 1) {
		// Who tried, and why it failed: the verify result names the reason
		// (no certificate, unknown CA, revoked, wrong purpose...).
		const long v = SSL_get_verify_result(ssl);
		log("device-monitor: %s: handshake refused: %s (verify: %s)", addr.c_str(), ssl_errors().c_str(),
		    X509_verify_cert_error_string(v));
		SSL_free(ssl);
		return;
	}
	const std::string who = peer_name(ssl);

	std::string req;
	char buf[1024];
	while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos) {
		const int n = SSL_read(ssl, buf, sizeof(buf));
		if (n <= 0)
			break;
		req.append(buf, static_cast<std::size_t>(n));
	}
	const auto sp1 = req.find(' ');
	const auto sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
	int code = 400;
	std::string out;
	if (sp2 == std::string::npos)
		out = response(400, "Bad Request", "text/plain", "bad request\n");
	else
		out = route(c, req.substr(0, sp1), req.substr(sp1 + 1, sp2 - sp1 - 1), code);
	SSL_write(ssl, out.data(), static_cast<int>(out.size()));
	SSL_shutdown(ssl);
	SSL_free(ssl);
	log("device-monitor: %s CN=%s \"%s\" %d", addr.c_str(), who.c_str(),
	    req.substr(0, req.find('\r')).c_str(), code);
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s CONFIG.json\n", argv[0]);
		return 2;
	}
	Config cfg;
	SSL_CTX* ctx = nullptr;
	try {
		cfg = Config::load(argv[1]);
		ctx = make_context(cfg);
		load_expiry(cfg, ctx);
	} catch (const std::exception& e) {
		log("device-monitor: %s", e.what());
		return 1;
	}

	struct sigaction sa{};
	sa.sa_handler = [](int) { g_stop = true; };
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	std::signal(SIGPIPE, SIG_IGN); // a client hanging up mid-write is not fatal

	const int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;
	::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(cfg.listen_port));
	if (::inet_pton(AF_INET, cfg.listen_address.c_str(), &addr.sin_addr) != 1 ||
	    ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(lfd, 8) != 0) {
		log("device-monitor: cannot listen on %s:%d: %s", cfg.listen_address.c_str(), cfg.listen_port,
		    std::strerror(errno));
		return 1;
	}
	log("device-monitor: listening on %s:%d (TLS 1.3, client certificate required%s)", cfg.listen_address.c_str(),
	    cfg.listen_port, cfg.crl_file.empty() ? "" : ", CRL checked");

	// One connection at a time, each bounded by timeout_ms: this serves a
	// monitoring scraper, and serial handling means a flood of connections
	// queues instead of multiplying threads on a small device.
	while (!g_stop) {
		pollfd pfd{lfd, POLLIN, 0};
		if (::poll(&pfd, 1, 500) <= 0)
			continue;
		sockaddr_in peer{};
		socklen_t len = sizeof(peer);
		const int client = ::accept4(lfd, reinterpret_cast<sockaddr*>(&peer), &len, SOCK_CLOEXEC);
		if (client < 0)
			continue;
		char ip[INET_ADDRSTRLEN] = "?";
		::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		serve(cfg, ctx, client, std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port)));
		::close(client);
	}
	::close(lfd);
	SSL_CTX_free(ctx);
	log("device-monitor: stopped");
	return 0;
}
