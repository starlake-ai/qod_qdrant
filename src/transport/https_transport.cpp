// cpp-httplib v0.64.0 over OpenSSL. One client per request, no keep-alive, no redirects, certificate and
// hostname verification always on. The request runs on a worker thread, so the caller returns at the
// deadline or on cancellation, in every phase, while the socket is closed (SSLClient::stop) and what
// arrives is discarded.
#include "httplib.h"
#include "transport/transport.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#ifndef _WIN32
#include <pthread.h>
#include <sys/socket.h>
#endif
#include <system_error>
#include <thread>

namespace qod_qdrant {

namespace {

constexpr const char *kPemBegin = "-----BEGIN CERTIFICATE-----";

std::string ReadPem(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return "";
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

//! The first readable known CA bundle, read once per process.
const std::string &SystemBundle() {
	static const std::string bundle = [] {
		for (auto path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
		                  "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", "/etc/ssl/cert.pem"}) {
			auto pem = ReadPem(path);
			if (pem.find(kPemBegin) != std::string::npos) {
				return pem;
			}
		}
		return std::string();
	}();
	return bundle;
}

//! The exchanges alive, abandoned ones included. Leaked, so it outlives static destruction.
struct LiveExchanges {
	std::mutex mutex;
	std::condition_variable none;
	int64_t count = 0;
};

LiveExchanges &Live() {
	static auto *live = new LiveExchanges();
	return *live;
}

//! At exit (registered after the first SSLClient, so before OpenSSL's own cleanup handler runs): waits,
//! at most 2 s, for abandoned workers to end, so none wakes up after static destructors or
//! OPENSSL_cleanup ran.
void WaitForExchanges() {
	auto &live = Live();
	std::unique_lock<std::mutex> lock(live.mutex);
	live.none.wait_for(lock, std::chrono::seconds(2), [&live] { return live.count == 0; });
}

//! Everything the request's worker thread touches. Shared, so the caller can return at the deadline or on
//! cancellation while the worker finishes on its own, bounded by httplib's timeouts.
struct Exchange {
	Exchange() {
		std::lock_guard<std::mutex> lock(Live().mutex);
		++Live().count;
	}
	~Exchange() {
		// The client, then the lease (the limiter slot), then the count WaitForExchanges waits on.
		client.reset();
		lease.reset();
		auto &live = Live();
		{
			std::lock_guard<std::mutex> lock(live.mutex);
			--live.count;
		}
		live.none.notify_all();
	}
	Exchange(const Exchange &) = delete;
	Exchange &operator=(const Exchange &) = delete;

	//! Declared first so it is released last, after the client: the caller's lease (its limiter slot)
	//! stays held until the socket work has really ended.
	std::shared_ptr<void> lease;
	std::unique_ptr<httplib::SSLClient> client;
	std::string path;
	std::string body;
	httplib::Headers headers;
	size_t limit = 0;
	std::string received;
	std::atomic<bool> too_large {false};
	std::atomic<bool> abandoned {false};
	std::atomic<bool> done {false};
	httplib::Error error = httplib::Error::Success;
	//! For a TLS failure, httplib's tls::ErrorCode.
	int ssl_error = 0;
	int status = 0;
	std::string exception;
};

//! The worker: runs the request, never throws.
void Run(Exchange &ex) {
	// Every write to the socket happens on this thread (stop() only shuts it down), so blocking SIGPIPE
	// here turns a write to a closed socket into EPIPE; the thread-directed signal left pending is drained
	// before the thread ends.
	// Windows sockets raise no SIGPIPE: a write to a closed socket already fails with an error there.
#ifndef _WIN32
	sigset_t pipe;
	sigemptyset(&pipe);
	sigaddset(&pipe, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
#endif
	try {
		auto res = ex.client->Post(ex.path, ex.headers, ex.body, "application/json", [&ex](const char *data, size_t length) {
			if (ex.abandoned) {
				return false;
			}
			if (ex.limit && ex.received.size() + length > ex.limit) {
				ex.too_large = true;
				return false;
			}
			ex.received.append(data, length);
			return true;
		});
		if (res) {
			ex.status = res->status;
		} else {
			ex.error = res.error();
			ex.ssl_error = res.ssl_error();
		}
	} catch (std::exception &e) {
		ex.exception = e.what();
	} catch (...) {
		ex.exception = "unknown exception";
	}
#ifdef __linux__
	struct timespec zero {0, 0};
	while (sigtimedwait(&pipe, nullptr, &zero) == SIGPIPE) {
	}
#endif
	ex.done = true;
}

//! A handshake that ran out of its connection timeout while waiting for the peer.
bool IsHandshakeTimeout(httplib::Error error, int ssl_error) {
	return error == httplib::Error::SSLConnection && (ssl_error == int(httplib::tls::ErrorCode::WantRead) ||
	                                                  ssl_error == int(httplib::tls::ErrorCode::WantWrite));
}

bool IsTlsError(httplib::Error error) {
	return error == httplib::Error::SSLConnection || error == httplib::Error::SSLLoadingCerts ||
	       error == httplib::Error::SSLServerVerification || error == httplib::Error::SSLServerHostnameVerification;
}

//! Failures of the connection itself, the only retryable transport failures.
bool IsConnectionError(httplib::Error error) {
	return error == httplib::Error::Connection || error == httplib::Error::Read || error == httplib::Error::Write ||
	       error == httplib::Error::ConnectionClosed;
}

class HttpsTransport : public Transport {
public:
	HttpsTransport(std::string host, int port, std::string api_key, std::string ca_file)
	    : host_(std::move(host)), port_(port), api_key_(std::move(api_key)), ca_file_(std::move(ca_file)) {
	}

	TransportResult Post(const std::string &path, const std::string &body, const CallOptions &options) override {
		TransportResult result;
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(options.deadline - Clock::now()).count();
		if (remaining <= 0) {
			result.failure = Failure::Timeout;
			result.detail = "deadline passed before the request";
			return result;
		}
		auto ex = std::make_shared<Exchange>();
		ex->lease = options.lease;
		ex->client.reset(new httplib::SSLClient(host_, port_));
		static std::once_flag at_exit;
		std::call_once(at_exit, [] { std::atexit(WaitForExchanges); });
		auto &client = *ex->client;
		client.enable_server_certificate_verification(true);
		client.enable_server_hostname_verification(true);
		// Never OpenSSL's compiled-in default directory (it differs between the manylinux build container
		// and the runtime image): the first known system bundle, plus QOD_QDRANT_CA_FILE, both read with the
		// C++ library, so DuckDB's file system (enable_external_access) is never involved. Loaded separately,
		// so a bad block in one source cannot drop the certificates of the other.
		client.enable_system_ca(false);
		auto extra = ca_file_.empty() ? std::string() : ReadPem(ca_file_);
		bool trusted = false;
		for (auto *pem : std::initializer_list<const std::string *> {&SystemBundle(), &extra}) {
			if (pem->find(kPemBegin) != std::string::npos) {
				client.load_ca_cert_store(pem->data(), pem->size());
				trusted = true;
			}
		}
		if (!trusted) {
			result.failure = Failure::Tls;
			result.detail = "no CA certificate: no system bundle found and QOD_QDRANT_CA_FILE unset or unreadable";
			return result;
		}
		client.set_follow_location(false);
		client.set_keep_alive(false);
		// The TCP connect and the TLS handshake each get the connection timeout: half the remaining time
		// each keeps both within the deadline.
		auto connect_ms = remaining / 2;
		client.set_connection_timeout(time_t(connect_ms / 1000), time_t((connect_ms % 1000) * 1000));
		auto seconds = time_t(remaining / 1000);
		auto micros = time_t((remaining % 1000) * 1000);
		client.set_read_timeout(seconds, micros);
		client.set_write_timeout(seconds, micros);
		client.set_max_timeout(time_t(remaining));
#ifdef SO_NOSIGPIPE
		// Writing to a socket the peer or stop() closed must fail with EPIPE, never kill the node with SIGPIPE
		// (OpenSSL writes with write(2), so MSG_NOSIGNAL does not apply). Linux: see the worker's mask.
		client.set_socket_options([](socket_t sock) {
			int one = 1;
			setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
		});
#endif
		ex->path = path;
		ex->body = body;
		ex->headers = {{"api-key", api_key_}, {"Accept", "application/json"}};
		ex->limit = options.max_response_bytes;

		std::thread worker;
		try {
			worker = std::thread([ex] { Run(*ex); });
		} catch (std::system_error &e) {
			// No thread: nothing was sent; the exchange, and with it the lease, is released on return.
			result.failure = Failure::Other;
			result.detail = std::string("no thread for the request: ") + e.what();
			return result;
		}
		bool cancelled = false;
		bool timed_out = false;
		while (!ex->done) {
			if (options.cancelled && options.cancelled()) {
				cancelled = true;
				break;
			}
			if (Clock::now() >= options.deadline) {
				timed_out = true;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		if (cancelled || timed_out) {
			// Return now and discard whatever arrives. stop() waits for httplib's socket mutex, which DNS,
			// connect and the handshake hold, so a detached reaper calls it until the worker ends: the socket
			// is shut down as soon as it can be, and the worker releases the lease when it is done.
			ex->abandoned = true;
			worker.detach();
			try {
				std::thread([ex] {
					while (!ex->done) {
						ex->client->stop();
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
				}).detach();
			} catch (std::system_error &e) {
				// No thread for the reaper: the worker still ends within httplib's timeouts.
				result.failure = Failure::Other;
				result.detail = std::string("no thread for the reaper: ") + e.what();
				return result;
			}
			result.failure = cancelled ? Failure::Cancelled : Failure::Timeout;
			result.detail = cancelled ? "cancelled during the request" : "deadline passed during the request";
			return result;
		}
		worker.join();

		auto error = ex->error;
		if (!ex->exception.empty()) {
			result.failure = Failure::Other;
			result.detail = "exception: " + ex->exception;
			return result;
		}
		if (ex->too_large) {
			result.failure = Failure::TooLarge;
		} else if (error == httplib::Error::Success) {
			result.failure = Failure::None;
		} else if (Clock::now() >= options.deadline || error == httplib::Error::ConnectionTimeout ||
		           error == httplib::Error::Timeout || IsHandshakeTimeout(error, ex->ssl_error)) {
			// httplib's own timeouts are set from the deadline and may fire before this loop sees it, reporting
			// a plain read failure: any failure once the deadline passed is the deadline.
			result.failure = Failure::Timeout;
		} else if (IsTlsError(error)) {
			result.failure = Failure::Tls;
		} else if (IsConnectionError(error)) {
			result.failure = Failure::Connect;
		} else {
			result.failure = Failure::Other;
		}
		if (result.failure != Failure::None) {
			result.detail = httplib::to_string(error);
			return result;
		}
		result.status = ex->status;
		if (result.status >= 300 && result.status < 400) {
			result.failure = Failure::Redirect;
			result.detail = "redirect refused, HTTP " + std::to_string(result.status);
			return result;
		}
		result.body = std::move(ex->received);
		return result;
	}

private:
	std::string host_;
	int port_;
	std::string api_key_;
	std::string ca_file_;
};

} // namespace

std::unique_ptr<Transport> MakeHttpsTransport(const std::string &endpoint, const std::string &api_key,
                                              const std::string &ca_file) {
	// endpoint is an allowlisted origin (EndpointSyntaxError passed): https://host[:port]
	auto rest = endpoint.substr(std::string("https://").size());
	auto colon = rest.find(':');
	auto host = rest.substr(0, colon);
	int port = colon == std::string::npos ? 443 : std::stoi(rest.substr(colon + 1));
	return std::unique_ptr<Transport>(new HttpsTransport(host, port, api_key, ca_file));
}

} // namespace qod_qdrant
