#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace qod_qdrant {

using Clock = std::chrono::steady_clock;

struct CallOptions {
	//! The request fails with Failure::Timeout once this passes.
	Clock::time_point deadline;
	//! The receive aborts (Failure::TooLarge) once the body would pass this.
	size_t max_response_bytes = 0;
	//! Polled while the request runs; true closes the socket (Failure::Cancelled).
	std::function<bool()> cancelled;
	//! Held until the request's socket work has ended, which may be after Post returned (at the deadline or
	//! on cancellation the caller returns at once): the caller's limiter slot.
	std::shared_ptr<void> lease;
};

enum class Failure { None, Connect, Timeout, Cancelled, TooLarge, Redirect, Tls, Other };
const char *FailureName(Failure failure);

struct TransportResult {
	Failure failure = Failure::None;
	//! The HTTP status when a response arrived (also for Failure::Redirect).
	int status = 0;
	std::string body;
	//! Privileged detail (library error text); never shown to a tenant.
	std::string detail;
};

//! One endpoint's POST of a JSON body. Implementations never follow redirects and always verify the
//! server certificate.
class Transport {
public:
	virtual ~Transport() = default;
	virtual TransportResult Post(const std::string &path, const std::string &body, const CallOptions &options) = 0;
};

//! cpp-httplib + OpenSSL to `endpoint` (an allowlisted https origin), header api-key. Trusts the first
//! readable known system CA bundle; `ca_file`, when not empty, adds its certificates.
std::unique_ptr<Transport> MakeHttpsTransport(const std::string &endpoint, const std::string &api_key,
                                              const std::string &ca_file);

//! The node's transport: MakeHttpsTransport with QOD_QDRANT_CA_FILE, or the testing override.
std::unique_ptr<Transport> MakeTransport(const std::string &endpoint, const std::string &api_key);

#ifdef QOD_QDRANT_TESTING
using TransportFactory = std::function<std::unique_ptr<Transport>(const std::string &endpoint, const std::string &api_key)>;
//! Test builds only: every later MakeTransport uses `factory` (nullptr restores HTTPS).
void SetTransportFactoryForTesting(TransportFactory factory);
//! Test builds only: appends `line` to the file named by QOD_QDRANT_TEST_REQUEST_LOG, when set.
void AppendRequestLog(const std::string &line);
#endif

} // namespace qod_qdrant
