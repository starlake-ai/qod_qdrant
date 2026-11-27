// A scripted Transport: records every call and answers from a queue or a handler. Needs a build with
// QOD_QDRANT_TESTING (the factory override).
#pragma once

#include "transport/transport.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qod_qdrant_test {

struct FakeCall {
	std::string endpoint;
	std::string path;
	std::string body;
};

struct FakeBackend {
	std::mutex mutex;
	std::vector<FakeCall> calls;
	std::vector<std::string> api_keys;
	std::deque<qod_qdrant::TransportResult> replies;
	//! When set, answers instead of the queue.
	std::function<qod_qdrant::TransportResult(const FakeCall &)> handler;
	//! When set, keeps every call's lease, like an HTTPS worker still running after Post returned.
	bool hold_leases = false;
	std::vector<std::shared_ptr<void>> leases;

	void Reset() {
		std::lock_guard<std::mutex> lock(mutex);
		calls.clear();
		api_keys.clear();
		replies.clear();
		handler = nullptr;
		hold_leases = false;
		leases.clear();
	}
	size_t CallCount() {
		std::lock_guard<std::mutex> lock(mutex);
		return calls.size();
	}
};

inline FakeBackend &Backend() {
	static FakeBackend backend;
	return backend;
}

inline qod_qdrant::TransportResult Reply(int status, std::string body) {
	qod_qdrant::TransportResult r;
	r.status = status;
	r.body = std::move(body);
	return r;
}

inline qod_qdrant::TransportResult Broken(qod_qdrant::Failure failure) {
	qod_qdrant::TransportResult r;
	r.failure = failure;
	r.detail = "scripted failure";
	return r;
}

class FakeTransport : public qod_qdrant::Transport {
public:
	explicit FakeTransport(std::string endpoint) : endpoint_(std::move(endpoint)) {
	}
	qod_qdrant::TransportResult Post(const std::string &path, const std::string &body,
	                                 const qod_qdrant::CallOptions &options) override {
		auto &b = Backend();
		FakeCall call {endpoint_, path, body};
		std::function<qod_qdrant::TransportResult(const FakeCall &)> handler;
		{
			std::lock_guard<std::mutex> lock(b.mutex);
			b.calls.push_back(call);
			if (b.hold_leases) {
				b.leases.push_back(options.lease);
			}
			handler = b.handler;
			if (!handler) {
				if (b.replies.empty()) {
					return Reply(500, "{\"status\":{\"error\":\"no scripted reply\"}}");
				}
				auto r = b.replies.front();
				b.replies.pop_front();
				return r;
			}
		}
		return handler(call);
	}

private:
	std::string endpoint_;
};

inline void InstallFakeTransport() {
	qod_qdrant::SetTransportFactoryForTesting([](const std::string &endpoint, const std::string &api_key) {
		{
			std::lock_guard<std::mutex> lock(Backend().mutex);
			Backend().api_keys.push_back(api_key);
		}
		return std::unique_ptr<qod_qdrant::Transport>(new FakeTransport(endpoint));
	});
}

} // namespace qod_qdrant_test
