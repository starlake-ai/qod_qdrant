#pragma once

#include "duckdb.hpp"
#include "settings.hpp"
#include "transport/transport.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace qod_qdrant {

//! Where a remote read goes: the attached source's id (privileged log key), endpoint and key, and the
//! deployment allowlist snapshot the endpoint is rechecked against before every request.
struct RemoteTarget {
	std::string source_id;
	std::string endpoint;
	std::string api_key;
	std::set<std::string> allowlist;
};

//! The node-wide cap on concurrent remote requests (search_max_concurrent_requests).
class RequestLimiter {
public:
	static RequestLimiter &Instance();
	//! Waits for a slot below `limit`; false once `deadline` passes or `cancelled` says so.
	bool Acquire(int64_t limit, Clock::time_point deadline, const std::function<bool()> &cancelled);
	void Release();
	int64_t InFlight();

private:
	std::mutex mutex_;
	std::condition_variable released_;
	int64_t in_flight_ = 0;
};

//! One idempotent read (scroll page or query): checks the allowlist and the request budget, waits for a
//! limiter slot, sends under the search_deadline_ms deadline, and retries at most twice (429, 502, 503,
//! 504, connection failures) while `may_retry`. Returns the 2xx body; raises a coded error otherwise,
//! with the privileged detail logged. Cancellation raises DuckDB's InterruptException.
std::string RemotePost(duckdb::ClientContext &context, const RemoteTarget &target, const SearchLimits &limits,
                       const std::string &path, const std::string &body, bool may_retry);

} // namespace qod_qdrant
