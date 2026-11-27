#include "transport/remote.hpp"

#include "catalog/config.hpp"

#include "ast/json.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "errors.hpp"

#include <thread>

namespace qod_qdrant {

RequestLimiter &RequestLimiter::Instance() {
	// Leaked: an abandoned HTTPS worker may release its slot while the process exits, after static
	// destructors ran.
	static auto *limiter = new RequestLimiter();
	return *limiter;
}

bool RequestLimiter::Acquire(int64_t limit, Clock::time_point deadline, const std::function<bool()> &cancelled) {
	std::unique_lock<std::mutex> lock(mutex_);
	while (in_flight_ >= limit) {
		if (Clock::now() >= deadline || (cancelled && cancelled())) {
			return false;
		}
		released_.wait_for(lock, std::chrono::milliseconds(5));
	}
	++in_flight_;
	return true;
}

void RequestLimiter::Release() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		--in_flight_;
	}
	released_.notify_one();
}

int64_t RequestLimiter::InFlight() {
	std::lock_guard<std::mutex> lock(mutex_);
	return in_flight_;
}

namespace {

constexpr const char *kUnavailable = "the search source is not available";

bool RetryableStatus(int status) {
	return status == 429 || status == 502 || status == 503 || status == 504;
}

std::string Excerpt(const std::string &body) {
	return body.size() > 512 ? body.substr(0, 512) + "..." : body;
}

} // namespace

std::string RemotePost(duckdb::ClientContext &context, const RemoteTarget &target, const SearchLimits &limits,
                       const std::string &path, const std::string &body, bool may_retry) {
	if (!target.allowlist.count(target.endpoint) && !target.allowlist.count(kAnyHttpsOrigin)) {
		Fail(target.source_id, SearchCode::SourceUnavailable, kUnavailable,
		     "endpoint is not in the deployment allowlist snapshot", target.api_key);
	}
	if (int64_t(body.size()) > limits.max_request_bytes) {
		Raise(SearchCode::BudgetExceeded, "the search request is larger than search_max_request_bytes");
	}
	auto deadline = Clock::now() + std::chrono::milliseconds(limits.deadline_ms);
	std::function<bool()> cancelled = [&context] { return context.IsInterrupted(); };
	auto transport = MakeTransport(target.endpoint, target.api_key);
	int attempts = may_retry ? 3 : 1;
	std::string last_detail;
	for (int attempt = 1; attempt <= attempts; ++attempt) {
		auto &limiter = RequestLimiter::Instance();
		if (!limiter.Acquire(limits.max_concurrent_requests, deadline, cancelled)) {
			if (cancelled()) {
				throw duckdb::InterruptException();
			}
			Fail(target.source_id, SearchCode::DeadlineExceeded, "the search source did not answer in time",
			     "no request slot before the deadline", target.api_key);
		}
		TransportResult result;
		{
			// The slot is released when the last holder lets go: here, or the HTTPS worker that still runs
			// after Post returned at the deadline or on cancellation, so the cap counts every open socket.
			CallOptions call {deadline, size_t(limits.max_response_bytes), cancelled,
			                  std::shared_ptr<void>(static_cast<void *>(&limiter),
			                                        [](void *p) { static_cast<RequestLimiter *>(p)->Release(); })};
#ifdef QOD_QDRANT_TESTING
			// Plan 4's format: one JSON object per line, before the request is sent, never a header.
			AppendRequestLog("{\"method\":\"POST\",\"path\":" + WriteJson(JsonNode::String(path)) + ",\"body\":" + body + "}");
#endif
			result = transport->Post(path, body, call);
		}

		bool retryable = false;
		switch (result.failure) {
		case Failure::None:
			if (result.status >= 200 && result.status < 300) {
				return std::move(result.body);
			}
			// Redacted before the cut, so a key straddling it is never logged in part.
			last_detail = "HTTP " + std::to_string(result.status) + ": " + Excerpt(Redact(result.body, target.api_key));
			if (!RetryableStatus(result.status)) {
				Fail(target.source_id, SearchCode::SourceUnavailable, kUnavailable, last_detail, target.api_key);
			}
			retryable = true;
			break;
		case Failure::Connect:
			last_detail = std::string(FailureName(result.failure)) + ": " + result.detail;
			retryable = true;
			break;
		case Failure::Other:
			Fail(target.source_id, SearchCode::SourceUnavailable, kUnavailable, "other: " + result.detail,
			     target.api_key);
		case Failure::Cancelled:
			throw duckdb::InterruptException();
		case Failure::Timeout:
			Fail(target.source_id, SearchCode::DeadlineExceeded, "the search source did not answer in time",
			     "timeout: " + result.detail, target.api_key);
		case Failure::TooLarge:
			Fail(target.source_id, SearchCode::BudgetExceeded,
			     "the search response is larger than search_max_response_bytes", "response too large", target.api_key);
		case Failure::Redirect:
		case Failure::Tls:
			Fail(target.source_id, SearchCode::SourceUnavailable, kUnavailable,
			     std::string(FailureName(result.failure)) + ": " + result.detail, target.api_key);
		}
		if (!retryable || attempt == attempts) {
			break;
		}
		auto pause = Clock::now() + std::chrono::milliseconds(50 * attempt);
		if (pause >= deadline) {
			Fail(target.source_id, SearchCode::DeadlineExceeded, "the search source did not answer in time",
			     "no time left to retry after " + last_detail, target.api_key);
		}
		std::this_thread::sleep_until(pause);
	}
	Fail(target.source_id, SearchCode::SourceUnavailable, kUnavailable, last_detail, target.api_key);
}

} // namespace qod_qdrant
