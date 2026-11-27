#pragma once

#include "duckdb.hpp"

#include <cstdint>

namespace qod_qdrant {

//! The node limits of spec section 6 (contract section 3 names and defaults).
struct SearchLimits {
	int64_t default_k = 10;
	int64_t max_k = 1000;
	int64_t max_request_bytes = 262144;
	int64_t max_filter_nodes = 1024;
	int64_t deadline_ms = 10000;
	int64_t max_response_bytes = 16777216;
	int64_t scan_budget_points = 1000000;
	int64_t max_concurrent_requests = 16;
	//! Slice 2 (cross-plan contract section 2): qod_bm25 text, qod_rrf prefetch_k and branch count.
	int64_t max_query_text_bytes = 4096;
	int64_t max_prefetch_k = 1000;
	int64_t max_fusion_branches = 4;
	//! Raw requests (qod_search(request := ...), lower/request_guard.hpp).
	int64_t max_request_text_bytes = 65536;
	int64_t max_prefetch_depth = 2;
	int64_t max_hnsw_ef = 512;
	int64_t max_request_ids = 64;
	int64_t max_oversampling = 8;
};

//! Registers the search_* settings: BIGINT, GLOBAL only, refused (search_config_invalid) on a connection
//! that carries a quack session, so only the node's trusted init can change them.
void RegisterSettings(duckdb::DBConfig &config);

//! The current values (defaults when a setting cannot be read).
SearchLimits ReadLimits(duckdb::ClientContext &context);

//! Whether `context` serves a tenant: quack opened it and installed a session (core's kQuackSessionKey).
bool IsTenantConnection(duckdb::ClientContext &context);

} // namespace qod_qdrant
