#pragma once

#include "ast/request.hpp"
#include "catalog/config.hpp"
#include "duckdb.hpp"
#include "duckdb/common/table_column.hpp"
#include "lower/filter_expr.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace qod_qdrant {

//! The virtual column ids of every qdrant scan: _qod_id (VARCHAR) and _qod_score (DOUBLE, NULL on scroll).
duckdb::column_t IdColumn();
duckdb::column_t ScoreColumn();
duckdb::virtual_column_map_t VirtualColumns();

//! A point id as _qod_id: decimal for an unsigned integer, lowercase hyphenated text for a UUID.
//! Throws ResponseError for anything else.
std::string FormatPointId(const JsonNode &id);

struct DecodedRow {
	std::string id;
	std::optional<double> score;
	//! One slot per CollectionSpec field; only the decoded fields are set, nullopt also for NULL.
	std::vector<std::optional<FieldValue>> values;
};

//! Decodes `fields` of the point's payload with the registry's types: a missing or null required field,
//! a value of another JSON type, or an array refuses (ResponseError naming the field; privileged).
DecodedRow DecodePoint(const QdrantPoint &point, const CollectionSpec &spec, const std::set<size_t> &fields);

duckdb::Value ToDuckValue(const std::optional<FieldValue> &value, FieldType type);

//! Checks once, before any request, that every scan column id is a field index or a virtual column; raises
//! search_query_unsupported otherwise.
void CheckScanColumns(const std::vector<duckdb::column_t> &column_ids, const CollectionSpec &spec);

//! Writes `row` into `output` at `row_index`, one value per scan column id (field, _qod_id, _qod_score).
void EmitRow(const DecodedRow &row, const std::vector<duckdb::column_t> &column_ids, const CollectionSpec &spec,
             duckdb::DataChunk &output, duckdb::idx_t row_index);

} // namespace qod_qdrant
