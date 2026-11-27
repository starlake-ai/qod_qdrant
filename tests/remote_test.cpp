// One remote read: allowlist recheck, request budget, retries (at most 2, retryable statuses and
// connection failures only, never once output started), status mapping, the concurrency limiter and the
// test request log.
#include "check.hpp"
#include "duckdb.hpp"
#include "fake_transport.hpp"
#include "settings.hpp"
#include "transport/remote.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

using namespace qod_qdrant;
using namespace qod_qdrant_test;

namespace {
//! What `f` writes to stderr (fd 2) while it runs.
template <class F>
std::string StderrOf(F f) {
	char path[] = "/tmp/qod_qdrant_remote_stderr_XXXXXX";
	int fd = mkstemp(path);
	int saved = dup(2);
	dup2(fd, 2);
	f();
	dup2(saved, 2);
	close(saved);
	close(fd);
	auto text = ReadFile(path);
	std::remove(path);
	return text;
}
} // namespace

int main() {
	duckdb::DuckDB db(nullptr);
	duckdb::Connection con(db);
	auto &context = *con.context;
	InstallFakeTransport();
	char log_path[] = "/tmp/qod_qdrant_remote_test_XXXXXX";
	int fd = mkstemp(log_path);
	close(fd);
	setenv("QOD_QDRANT_TEST_REQUEST_LOG", log_path, 1);

	RemoteTarget target {"fs-1", "https://qdrant.internal:6333", "key-abc", {"https://qdrant.internal:6333"}};
	SearchLimits limits;
	auto log_lines = [&] {
		auto text = ReadFile(log_path);
		return std::to_string(text.empty() ? 0 : std::count(text.begin(), text.end(), '\n') + 1);
	};
	auto post = [&](bool may_retry) { return RemotePost(context, target, limits, "/collections/c/points/scroll", "{}", may_retry); };

	Backend().Reset();
	Backend().replies = {Reply(200, "{\"ok\":1}")};
	Expect("2xx body", post(true), "{\"ok\":1}");
	Expect("api key handed to the transport", Backend().api_keys.at(0), "key-abc");
	Expect("request logged as one JSON line before sending", ReadFile(log_path),
	       "{\"method\":\"POST\",\"path\":\"/collections/c/points/scroll\",\"body\":{}}");

	Backend().Reset();
	Backend().replies = {Reply(503, "busy"), Reply(200, "{}")};
	Expect("retried 503", post(true), "{}");
	Expect("two calls", std::to_string(Backend().CallCount()), "2");
	Expect("one log line per attempt", log_lines(), "3");

	Backend().Reset();
	Backend().replies = {Reply(503, "a"), Reply(502, "b"), Reply(504, "c"), Reply(200, "{}")};
	Contains("at most 2 retries", ErrorOf([&] { post(true); }), "search_source_unavailable: ");
	Expect("three calls", std::to_string(Backend().CallCount()), "3");

	Backend().Reset();
	Backend().replies = {Reply(429, "slow down"), Reply(200, "{}")};
	Contains("no retry once output started", ErrorOf([&] { post(false); }), "search_source_unavailable: ");
	Expect("one call", std::to_string(Backend().CallCount()), "1");

	Backend().Reset();
	Backend().replies = {Broken(Failure::Connect), Reply(200, "{}")};
	Expect("connection failure retried", post(true), "{}");

	Backend().Reset();
	Backend().replies = {Reply(400, "{\"status\":{\"error\":\"bad key-abc contracts_v3\"}}"), Reply(200, "{}")};
	auto refused = ErrorOf([&] { post(true); });
	Contains("400 is not retried", refused, "search_source_unavailable: ", "key-abc");
	Contains("no backend text for the tenant", refused, "search_source_unavailable: ", "contracts_v3");
	Expect("400 once", std::to_string(Backend().CallCount()), "1");

	Backend().Reset();
	Backend().replies = {Broken(Failure::Other), Reply(200, "{}")};
	Contains("other transport failures are not retried", ErrorOf([&] { post(true); }), "search_source_unavailable: ");
	Expect("other once", std::to_string(Backend().CallCount()), "1");

	Backend().Reset();
	std::string straddling(510, 'x');
	Backend().replies = {Reply(400, straddling + "key-abc")};
	std::string straddled;
	auto logged_cut = StderrOf([&] { straddled = ErrorOf([&] { post(true); }); });
	Contains("a key across the excerpt cut", straddled, "search_source_unavailable: ");
	Contains("is redacted before the cut, never logged in part", logged_cut, "detail=HTTP 400: xxx", "xke");

	Backend().Reset();
	limits.deadline_ms = 30;
	Backend().replies = {Reply(503, "busy"), Reply(200, "{}")};
	Contains("no time left for the retry pause", ErrorOf([&] { post(true); }), "search_deadline_exceeded: ");
	Expect("no retry past the deadline", std::to_string(Backend().CallCount()), "1");
	limits = SearchLimits();

	Backend().Reset();
	Backend().hold_leases = true;
	Backend().replies = {Reply(200, "{}")};
	post(true);
	Expect("the slot stays held while the transport holds the lease", std::to_string(RequestLimiter::Instance().InFlight()),
	       "1");
	Backend().Reset();
	Expect("and is freed with it", std::to_string(RequestLimiter::Instance().InFlight()), "0");

	Backend().Reset();
	Backend().replies = {Broken(Failure::Timeout)};
	Contains("timeout", ErrorOf([&] { post(true); }), "search_deadline_exceeded: ");
	Backend().replies = {Broken(Failure::TooLarge)};
	Contains("too large", ErrorOf([&] { post(true); }), "search_budget_exceeded: ");
	Backend().replies = {Broken(Failure::Redirect)};
	Contains("redirect", ErrorOf([&] { post(true); }), "search_source_unavailable: ");
	Backend().replies = {Broken(Failure::Tls)};
	Contains("tls", ErrorOf([&] { post(true); }), "search_source_unavailable: ");
	Backend().replies = {Broken(Failure::Cancelled), Reply(200, "{}")};
	Expect("cancelled is DuckDB's interrupt, not retried", ErrorOf([&] { post(true); }), "Interrupted!");
	Backend().Reset();
	Backend().replies = {Reply(401, "{\"status\":{\"error\":\"Unauthorized\"}}"), Reply(200, "{}")};
	Contains("401 is source unavailable, not retried", ErrorOf([&] { post(true); }), "search_source_unavailable: ", "key-abc");
	Backend().replies = {Reply(403, "forbidden")};
	Contains("403", ErrorOf([&] { post(true); }), "search_source_unavailable: ");
	Expect("one call each", std::to_string(Backend().CallCount()), "2");

	Backend().Reset();
	auto logged = log_lines();
	limits.max_request_bytes = 1;
	Contains("request budget", ErrorOf([&] { post(true); }), "search_budget_exceeded: ");
	limits = SearchLimits();
	RemoteTarget outside = target;
	outside.allowlist = {"https://other.internal:6333"};
	Contains("endpoint rechecked", ErrorOf([&] { RemotePost(context, outside, limits, "/x", "{}", true); }),
	         "search_source_unavailable: ");
	Expect("no request sent for either", std::to_string(Backend().CallCount()), "0");
	Expect("nor logged", log_lines(), logged);
	Contains("the request log never holds the api key", ReadFile(log_path), "\"method\":\"POST\"", "key-abc");

	auto &limiter = RequestLimiter::Instance();
	auto soon = [](int ms) { return Clock::now() + std::chrono::milliseconds(ms); };
	ExpectTrue("first slot", limiter.Acquire(1, soon(100), nullptr));
	ExpectTrue("limit reached until the deadline", !limiter.Acquire(1, soon(50), nullptr));
	std::thread releaser([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		limiter.Release();
	});
	ExpectTrue("a released slot is handed on", limiter.Acquire(1, soon(1000), nullptr));
	releaser.join();
	auto start = Clock::now();
	ExpectTrue("cancelled while waiting for a slot", !limiter.Acquire(1, soon(5000), [] { return true; }));
	ExpectTrue("and at once", Clock::now() - start < std::chrono::milliseconds(1000));
	limiter.Release();
	Expect("nothing in flight", std::to_string(limiter.InFlight()), "0");
	std::remove(log_path);
	return Finish();
}
