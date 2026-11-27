#pragma once

#include "catalog/catalog.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace qod_qdrant {

//! The scan function of every qdrant table (not in the catalog: only GetScanFunction returns it).
duckdb::TableFunction ScrollFunction();
//! Its bind data: an unbounded QdrantScanData (no I/O).
duckdb::unique_ptr<duckdb::FunctionData> MakeScrollBindData(QdrantTableEntry &table, duckdb::ClientContext &context);

} // namespace qod_qdrant
