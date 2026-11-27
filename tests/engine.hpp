// A DuckDB with the real node hooks (core's qod_node_core: parse screen, post_bind policies, ProtectedScan
// obligation, describer) and the qod_qdrant extension loaded, as qod-node opens it. `setup` is an internal
// connection (trusted init); `user` carries a quack session and a signed handle, as a tenant connection.
#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "extension.hpp"
#include "node_hooks.hpp"
#include "test_handles.hpp"

#include <cstdlib>
#include <memory>
#include <string>

namespace qod_qdrant_test {

inline int64_t &Now() {
	static int64_t now = 1761200000000;
	return now;
}

inline std::string &LastDescription() {
	static std::string description;
	return description;
}

struct SessionState : duckdb::ClientContextState {};

inline std::string RunSql(duckdb::Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		return "ERROR: " + result->GetError();
	}
	std::string out;
	while (auto chunk = result->FetchRaw()) {
		for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
			std::string line;
			for (duckdb::idx_t col = 0; col < chunk->ColumnCount(); ++col) {
				auto v = chunk->GetValue(col, row);
				line += (col ? "|" : "") + (v.IsNull() ? std::string("NULL") : v.ToString());
			}
			out += (out.empty() ? "" : ",") + line;
		}
	}
	return out;
}

class Engine {
public:
	explicit Engine(const std::string &allowlist = "https://qdrant.internal:6333") {
		// Read by the extension at load, as the node reads the deployment's env.
		setenv("QOD_QDRANT_ALLOWED_ENDPOINTS", allowlist.c_str(), 1);
		db = std::make_unique<duckdb::DuckDB>(nullptr);
		db->LoadStaticExtension<qod_qdrant::QdrantExtension>();
		authz = std::make_shared<qod::NodeAuthz>(qod_test::kNode, qod_test::kKey, [] { return Now(); });
		qod::RegisterNodeAuthz(*db->instance, authz, [](const qod::DecideCall &call) {
			LastDescription() = call.description;
			qod::DecideResult result;
			result.status = qod::DecideResult::Status::Allow;
			result.has_verdict = true;
			result.verdict.expires_at = Now() + 60000;
			return result;
		});
		setup = std::make_unique<duckdb::Connection>(*db);
		user = std::make_unique<duckdb::Connection>(*db);
		user->context->registered_state->Insert(qod::kQuackSessionKey, duckdb::make_shared_ptr<SessionState>());
		handle.principal_id = "alice";
		handle.exp = Now() + 60000;
	}

	//! Signs and installs `handle` on the user connection (a fresh policy version each time).
	std::string Install() {
		++handle.policy_version;
		return RunSql(*user, qod_test::Prelude(qod_test::MintWith(handle)));
	}

	//! The two statements Plan 2 renders, run as trusted init. `options` replaces ", READ_ONLY" when set.
	std::string Attach(const std::string &alias, const std::string &config, const std::string &options = ", READ_ONLY",
	                   const std::string &path = "qdrant:fs-qdrant-1") {
		std::string doubled;
		for (auto c : config) {
			doubled += c == '\'' ? std::string("''") : std::string(1, c);
		}
		auto secret = RunSql(*setup, "CREATE OR REPLACE SECRET qod_qdrant_" + alias + " (TYPE qdrant, API_KEY 'test-key')");
		if (secret.rfind("ERROR", 0) == 0) {
			return secret;
		}
		// Contract section 2 (amended): the alias is double-quoted, the secret name is not.
		return RunSql(*setup, "ATTACH '" + path + "' AS \"" + alias + "\" (TYPE qdrant, SECRET qod_qdrant_" + alias +
		                          ", CONFIG '" + doubled + "'" + options + ")");
	}

	std::unique_ptr<duckdb::DuckDB> db;
	std::shared_ptr<qod::NodeAuthz> authz;
	std::unique_ptr<duckdb::Connection> setup;
	std::unique_ptr<duckdb::Connection> user;
	qod_test::MintSpec handle;
};

} // namespace qod_qdrant_test
