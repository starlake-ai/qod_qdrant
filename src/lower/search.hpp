#pragma once

#include "catalog/config.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "lower/query.hpp"

namespace qod_qdrant {

//! qod_search(relation VARCHAR, query := <QueryType()>, filter_sql := VARCHAR, k := BIGINT): bounded.
duckdb::TableFunction SearchFunction();

//! Registers qod_dense, qod_bm25, qod_rrf and qod_search, qod_search as a bounded function, and (once per
//! process, before core freezes linked classes) their classes: qod_search table allowed, the three query
//! constructors scalar allowed.
void RegisterSearchFunctions(duckdb::ExtensionLoader &loader);

} // namespace qod_qdrant
