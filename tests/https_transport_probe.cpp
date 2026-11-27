// Drives MakeHttpsTransport against https_stub.py and silent_stub.py.
// usage: https_transport_probe <port> <ca-file> <mismatch-port> <ip-port> <silent-port>
//        https_transport_probe exit <silent-port> <ca-file>
// <port> serves a certificate for localhost, <mismatch-port> one for another DNS name, <ip-port> one for
// the IP address 127.0.0.1, all issued by the CA in <ca-file>; <silent-port> accepts TCP and never answers
// the ClientHello. Prints one line per case: "<case> failure=<name> status=<http status>", the case name
// suffixed "_late" when a case that must return promptly did not, then the leases still held. The exit
// mode abandons a request holding a limiter slot and returns from main at once: the process must exit
// normally, after the worker ended ("exit in_flight=0", printed by a handler that runs after the
// transport's own exit handler).
#include "transport/remote.hpp"
#include "transport/transport.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace qod_qdrant;

namespace {

//! Leases handed to the transport and not yet released (a released lease is a freed limiter slot).
std::atomic<int> outstanding {0};

std::shared_ptr<void> Lease() {
	++outstanding;
	return std::shared_ptr<void>(static_cast<void *>(&outstanding), [](void *) { --outstanding; });
}

void Print(const std::string &name, const TransportResult &result) {
	std::cout << name << " failure=" << FailureName(result.failure) << " status=" << result.status << std::endl;
}

struct Spec {
	std::string endpoint;
	std::string key = "test-key";
	std::string ca;
	std::string collection = "c";
	int deadline_ms = 5000;
	size_t max_bytes = 1 << 20;
	//! Cancels this long after the start, when not negative.
	int cancel_after_ms = -1;
	//! The case must return within this, when not negative.
	int within_ms = -1;
};

void Case(const std::string &name, const Spec &spec) {
	auto transport = MakeHttpsTransport(spec.endpoint, spec.key, spec.ca);
	auto start = Clock::now();
	auto cancel_after = std::chrono::milliseconds(spec.cancel_after_ms);
	std::function<bool()> cancelled = [start, cancel_after] {
		return cancel_after.count() >= 0 && Clock::now() - start >= cancel_after;
	};
	CallOptions options {start + std::chrono::milliseconds(spec.deadline_ms), spec.max_bytes, cancelled, Lease()};
	auto result = transport->Post("/collections/" + spec.collection + "/points/scroll", "{\"limit\":1}", options);
	options.lease.reset();
	auto late = spec.within_ms >= 0 && Clock::now() - start > std::chrono::milliseconds(spec.within_ms);
	Print(late ? name + "_late" : name, result);
}

//! MakeTransport reads the extra CA file from QOD_QDRANT_CA_FILE.
void EnvCase(const std::string &endpoint, const std::string &ca) {
	setenv("QOD_QDRANT_CA_FILE", ca.c_str(), 1);
	auto transport = MakeTransport(endpoint, "test-key");
	unsetenv("QOD_QDRANT_CA_FILE");
	CallOptions options {Clock::now() + std::chrono::milliseconds(5000), 1 << 20, [] { return false; }};
	Print("envca", transport->Post("/collections/c/points/scroll", "{\"limit\":1}", options));
}

void PrintInFlightAtExit() {
	std::printf("exit in_flight=%lld\n", static_cast<long long>(RequestLimiter::Instance().InFlight()));
	std::fflush(stdout);
}

//! Cancelled at 100 ms during a handshake that times out at 1.5 s (half the 3 s deadline), so the worker
//! is still alive when main returns; the slot it holds shows whether exit waited for it.
int ExitMode(const std::string &silent_port, const std::string &ca) {
	// Registered before the first SSLClient, so it runs after the transport's exit handler.
	std::atexit(PrintInFlightAtExit);
	auto &limiter = RequestLimiter::Instance();
	limiter.Acquire(1, Clock::now() + std::chrono::seconds(1), nullptr);
	auto transport = MakeHttpsTransport("https://localhost:" + silent_port, "test-key", ca);
	auto start = Clock::now();
	CallOptions options {start + std::chrono::milliseconds(3000), 1 << 20,
	                     [start] { return Clock::now() - start >= std::chrono::milliseconds(100); },
	                     std::shared_ptr<void>(static_cast<void *>(&limiter),
	                                           [](void *p) { static_cast<RequestLimiter *>(p)->Release(); })};
	Print("abandoned", transport->Post("/collections/c/points/scroll", "{\"limit\":1}", options));
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc == 4 && std::string(argv[1]) == "exit") {
		return ExitMode(argv[2], argv[3]);
	}
	if (argc != 6) {
		std::cerr << "usage: https_transport_probe <port> <ca-file> <mismatch-port> <ip-port> <silent-port>"
		          << std::endl;
		return 2;
	}
	std::string ca = argv[2];
	std::string local = "https://localhost:" + std::string(argv[1]);
	std::string silent = "https://localhost:" + std::string(argv[5]);
	// Prompt cases return within their deadline or cancellation plus this.
	const int slack = 400;
	Case("ok", {local, "test-key", ca});
	Case("badkey", {local, "wrong", ca});
	Case("redirect", {local, "test-key", ca, "redirect"});
	Case("big", {local, "test-key", ca, "big", 5000, 1024});
	Case("slow", {local, "test-key", ca, "slow", 300, 1 << 20, -1, 300 + slack});
	Case("untrusted", {local, "test-key", ""});
	Case("wronghost", {"https://127.0.0.1:" + std::string(argv[1]), "test-key", ca});
	Case("wrongname", {"https://localhost:" + std::string(argv[3]), "test-key", ca});
	Case("ipsan", {"https://127.0.0.1:" + std::string(argv[4]), "test-key", ca});
	Case("closed", {"https://localhost:1", "test-key", ca});
	Case("cancelled", {local, "test-key", ca, "slow", 5000, 1 << 20, 200, 200 + slack});
	Case("handshake_cancelled", {silent, "test-key", ca, "c", 5000, 1 << 20, 200, 200 + slack});
	Case("handshake_deadline", {silent, "test-key", ca, "c", 300, 1 << 20, -1, 300 + slack});
	EnvCase(local, ca);
	// Requests abandoned at the deadline or on cancellation finish on their own, within their timeouts, and
	// only then release their lease.
	for (int i = 0; i < 200 && outstanding > 0; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	std::cout << "leases outstanding=" << outstanding << std::endl;
	return 0;
}
