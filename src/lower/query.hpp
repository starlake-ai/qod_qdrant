#pragma once

#include "catalog/config.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <vector>

namespace qod_qdrant {

//! The tenant text of every malformed query value (cross-plan contract section 2, refusal 1).
extern const char *const kQueryShape;

//! STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], text VARCHAR): one qod_rrf branch.
duckdb::LogicalType QueryBranchType();

//! STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], text VARCHAR, prefetch_k BIGINT,
//! branches QueryBranchType()[]): what qod_dense, qod_bm25 and qod_rrf build and qod_search reads. Kinds
//! dense, bm25, rrf; the fields a kind does not use are NULL.
duckdb::LogicalType QueryType();

//! The query vector, checked against the registered vector: its datatype (float32), dimensions, every value
//! non-null and finite (a non-finite value never reaches the JSON writer). Raises search_query_unsupported.
std::vector<float> CheckedVector(const duckdb::Value &list, const VectorSpec &spec);

//! qod_dense(vector_name VARCHAR constant, vector FLOAT[]): kind dense. Query data only, no I/O.
duckdb::ScalarFunction DenseFunction();
//! qod_bm25(vector_name VARCHAR constant, text VARCHAR): kind bm25. Query data only, no I/O.
duckdb::ScalarFunction Bm25Function();
//! qod_rrf(branches QueryType()[], prefetch_k := BIGINT keyword-only, default NULL): kind rrf, each branch
//! the first four fields of an element, prefetch_k as given. Query data only, no I/O.
duckdb::ScalarFunction RrfFunction();

} // namespace qod_qdrant
