// The extension's load surface: the qdrant secret (redacted), the search_* settings (trusted init only)
// and the error helpers.
#include "catalog/secret.hpp"
#include "check.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "errors.hpp"
#include "extension.hpp"
#include "node_hooks.hpp"
#include "settings.hpp"

using namespace qod_qdrant_test;
using namespace duckdb;

namespace {
struct QuackSession : ClientContextState {};

std::string Run(Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		return "ERROR: " + result->GetError();
	}
	std::string out;
	while (auto chunk = result->FetchRaw()) {
		for (idx_t row = 0; row < chunk->size(); ++row) {
			out += (out.empty() ? "" : ",") + chunk->GetValue(0, row).ToString();
		}
	}
	return out;
}
} // namespace

int main() {
	DuckDB db(nullptr);
	db.LoadStaticExtension(&duckdb_extension_qod_qdrant_describe);
	Connection con(db);

	Expect("secret", Run(con, "CREATE SECRET qod_qdrant_docs (TYPE qdrant, API_KEY 'k-123-secret')"), "true");
	Contains("secret redacted", Run(con, "SELECT secret_string FROM duckdb_secrets() WHERE name = 'qod_qdrant_docs'"),
	         "api_key", "k-123-secret");
	// ReadApiKey runs inside a statement (ATTACH); outside one the test opens the transaction it needs.
	auto read_key = [&](const std::string &name) {
		std::string key;
		con.context->RunFunctionInTransaction([&] { key = qod_qdrant::ReadApiKey(*con.context, name); });
		return key;
	};
	Expect("api key read back", read_key("qod_qdrant_docs"), "k-123-secret");
	Contains("unknown secret", ErrorOf([&] { read_key("nope"); }), "search_config_invalid: ");
	Contains("unknown parameter", Run(con, "CREATE SECRET s2 (TYPE qdrant, TOKEN 'x')"), "ERROR");
	Contains("missing api_key", Run(con, "CREATE SECRET s3 (TYPE qdrant)"), "search_config_invalid: ");
	Contains("empty api_key", Run(con, "CREATE SECRET s4 (TYPE qdrant, API_KEY '')"), "search_config_invalid: ");

	Expect("default k", Run(con, "SELECT current_setting('search_default_k')"), "10");
	Expect("max k", Run(con, "SELECT current_setting('search_max_k')"), "1000");
	Expect("request bytes", Run(con, "SELECT current_setting('search_max_request_bytes')"), "262144");
	Expect("filter nodes", Run(con, "SELECT current_setting('search_max_filter_nodes')"), "1024");
	Expect("deadline", Run(con, "SELECT current_setting('search_deadline_ms')"), "10000");
	Expect("response bytes", Run(con, "SELECT current_setting('search_max_response_bytes')"), "16777216");
	Expect("scan budget", Run(con, "SELECT current_setting('search_scan_budget_points')"), "1000000");
	Expect("concurrency", Run(con, "SELECT current_setting('search_max_concurrent_requests')"), "16");
	Expect("limits read", std::to_string(qod_qdrant::ReadLimits(*con.context).max_k), "1000");
	Expect("query text bytes", Run(con, "SELECT current_setting('search_max_query_text_bytes')"), "4096");
	Expect("prefetch k", Run(con, "SELECT current_setting('search_max_prefetch_k')"), "1000");
	Expect("fusion branches", Run(con, "SELECT current_setting('search_max_fusion_branches')"), "4");
	auto limits = qod_qdrant::ReadLimits(*con.context);
	Expect("slice 2 limits read", std::to_string(limits.max_query_text_bytes) + " " + std::to_string(limits.max_prefetch_k) +
	                                  " " + std::to_string(limits.max_fusion_branches),
	       "4096 1000 4");
	for (auto sql : {"SET GLOBAL search_max_query_text_bytes = 262144", "SET GLOBAL search_max_query_text_bytes = 1",
	                 "SET GLOBAL search_max_prefetch_k = 100000", "SET GLOBAL search_max_prefetch_k = 1",
	                 "SET GLOBAL search_max_fusion_branches = 16", "SET GLOBAL search_max_fusion_branches = 2"}) {
		Expect(std::string("in range: ") + sql, Run(con, sql), "");
	}
	Expect("query text bytes too large", Run(con, "SET GLOBAL search_max_query_text_bytes = 262145"),
	       "ERROR: Invalid Input Error: search_config_invalid: search_max_query_text_bytes must be in 1..262144");
	Contains("query text bytes zero", Run(con, "SET GLOBAL search_max_query_text_bytes = 0"), "search_config_invalid: ");
	Expect("prefetch k too large", Run(con, "SET GLOBAL search_max_prefetch_k = 100001"),
	       "ERROR: Invalid Input Error: search_config_invalid: search_max_prefetch_k must be in 1..100000");
	Contains("prefetch k zero", Run(con, "SET GLOBAL search_max_prefetch_k = 0"), "search_config_invalid: ");
	Expect("one fusion branch", Run(con, "SET GLOBAL search_max_fusion_branches = 1"),
	       "ERROR: Invalid Input Error: search_config_invalid: search_max_fusion_branches must be in 2..16");
	Contains("17 fusion branches", Run(con, "SET GLOBAL search_max_fusion_branches = 17"), "search_config_invalid: ");
	for (auto name : {"search_max_query_text_bytes", "search_max_prefetch_k", "search_max_fusion_branches"}) {
		Expect(std::string("reset ") + name, Run(con, std::string("RESET ") + name), "");
	}

	Expect("trusted init may set", Run(con, "SET GLOBAL search_max_k = 500"), "");
	Expect("value set", std::to_string(qod_qdrant::ReadLimits(*con.context).max_k), "500");
	Contains("out of range", Run(con, "SET GLOBAL search_max_k = 0"), "search_config_invalid: ");
	Contains("no session scope", Run(con, "SET SESSION search_max_k = 5"), "ERROR");

	Connection tenant(db);
	tenant.context->registered_state->Insert(qod::kQuackSessionKey, make_shared_ptr<QuackSession>());
	ExpectTrue("tenant connection detected", qod_qdrant::IsTenantConnection(*tenant.context));
	ExpectTrue("internal connection", !qod_qdrant::IsTenantConnection(*con.context));
	Contains("tenant SET refused", Run(tenant, "SET GLOBAL search_max_k = 900"), "search_config_invalid: ");
	Contains("tenant plain SET refused", Run(tenant, "SET search_deadline_ms = 1"), "search_config_invalid: ");
	Contains("tenant RESET refused", Run(tenant, "RESET search_max_k"), "search_config_invalid: ");
	Expect("value unchanged", std::to_string(qod_qdrant::ReadLimits(*con.context).max_k), "500");
	for (auto sql : {"SET GLOBAL search_max_query_text_bytes = 10", "SET search_max_prefetch_k = 5",
	                 "RESET search_max_fusion_branches"}) {
		Contains(std::string("tenant refused: ") + sql, Run(tenant, sql), "search_config_invalid: ");
	}
	Expect("slice 2 limits unchanged by a tenant",
	       std::to_string(qod_qdrant::ReadLimits(*con.context).max_query_text_bytes) + " " +
	           std::to_string(qod_qdrant::ReadLimits(*con.context).max_prefetch_k),
	       "4096 1000");
	// Every other spelling that reaches the setting: PRAGMA, SET ... TO DEFAULT, RESET GLOBAL, SET to NULL.
	for (auto sql : {"PRAGMA search_max_k=7", "SET search_max_k TO DEFAULT", "RESET GLOBAL search_max_k",
	                 "SET GLOBAL search_max_k = NULL"}) {
		Contains(std::string("tenant refused: ") + sql, Run(tenant, sql), "search_config_invalid: ");
		Expect(std::string("value unchanged after: ") + sql, std::to_string(qod_qdrant::ReadLimits(*con.context).max_k),
		       "500");
	}

	Expect("raise", ErrorOf([] { qod_qdrant::Raise(qod_qdrant::SearchCode::QueryUnsupported, "k must be 1..1000"); }),
	       "search_query_unsupported: k must be 1..1000");
	Contains("permission class", ErrorOf([] { qod_qdrant::Raise(qod_qdrant::SearchCode::PolicyChanged, "x"); }),
	         "search_policy_changed: x");
	Expect("code names",
	       std::string(qod_qdrant::CodeName(qod_qdrant::SearchCode::BudgetExceeded)) + "," +
	           qod_qdrant::CodeName(qod_qdrant::SearchCode::SourceUnavailable) + "," +
	           qod_qdrant::CodeName(qod_qdrant::SearchCode::DeadlineExceeded) + "," +
	           qod_qdrant::CodeName(qod_qdrant::SearchCode::ConfigInvalid) + "," +
	           qod_qdrant::CodeName(qod_qdrant::SearchCode::FieldDenied) + "," +
	           qod_qdrant::CodeName(qod_qdrant::SearchCode::SchemaMismatch) + "," +
	           qod_qdrant::CodeName(qod_qdrant::SearchCode::PolicyUnsupported),
	       "search_budget_exceeded,search_source_unavailable,search_deadline_exceeded,search_config_invalid,"
	       "search_field_denied,search_schema_mismatch,search_policy_unsupported");
	Expect("redact", qod_qdrant::Redact("api-key: k1 and k1", "k1"), "api-key: [redacted] and [redacted]");
	Expect("detail line", qod_qdrant::DetailLine("fs-1", qod_qdrant::SearchCode::SourceUnavailable, "HTTP 500\nbody k1", "k1"),
	       "[qod_qdrant] source=fs-1 code=search_source_unavailable detail=HTTP 500 body [redacted]");
	Expect("control bytes never reach the log",
	       qod_qdrant::DetailLine("fs-1", qod_qdrant::SearchCode::SchemaMismatch, std::string("a\0b\x1b[2Jc\x7f", 9), ""),
	       "[qod_qdrant] source=fs-1 code=search_schema_mismatch detail=a b [2Jc ");
	return Finish();
}
