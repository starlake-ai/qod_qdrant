#pragma once

#include "catalog/config.hpp"
#include "duckdb.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "lower/filter_expr.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace qod_qdrant {

//! The DuckDB type of a registered field.
duckdb::LogicalType SqlType(FieldType type);

//! Maps a bound column reference to its field index, or nullopt for anything else.
using ColumnResolver = std::function<std::optional<size_t>(const duckdb::BoundColumnRefExpression &)>;

//! The exact lowering of a bound boolean expression, or nullopt when any part of it is outside spec 5.3:
//! `col = literal` (VARCHAR, BIGINT, BOOLEAN), `col IN (literals)` (VARCHAR, BIGINT), AND, OR. The column
//! side must be the bare column; the literal side must fold to a non-null value of exactly its type.
std::optional<FilterExpr> LowerExpression(duckdb::ClientContext &context, const duckdb::Expression &expr,
                                          const CollectionSpec &spec, const ColumnResolver &resolve);

//! One RowPolicyBoundary predicate: the OR of TRY(CAST(<policy> AS BOOLEAN)) terms (a lone term as is).
//! TRY and an identity BOOLEAN cast are stripped; any other cast is not lowered.
std::optional<FilterExpr> LowerPolicyPredicate(duckdb::ClientContext &context, const duckdb::Expression &predicate,
                                               const CollectionSpec &spec, const ColumnResolver &resolve);

//! The AND of the predicates core hands AcceptRowPolicies, column references resolved through
//! get.GetColumnIds(). TRUE for an empty list.
std::optional<FilterExpr> LowerPolicyPredicates(duckdb::ClientContext &context, const duckdb::LogicalGet &get,
                                                const std::vector<duckdb::unique_ptr<duckdb::Expression>> &predicates,
                                                const CollectionSpec &spec);

struct BoundColumns {
	duckdb::unique_ptr<duckdb::Expression> expr;
	ColumnResolver resolver;
};

//! Binds `parsed` against the collection's fields only, as core binds a row policy: no subquery,
//! parameter, lambda, window or qualified column; functions only from the system catalog and classed
//! allowed. Raises search_query_unsupported on anything else or on a binding failure.
BoundColumns BindColumns(duckdb::ClientContext &context, duckdb::unique_ptr<duckdb::ParsedExpression> parsed,
                         const CollectionSpec &spec);

//! filter_sql (U): parsed as one expression, bound by BindColumns, lowered exactly, simplified. TRUE for a
//! blank string. Raises search_query_unsupported for anything it cannot lower exactly.
FilterExpr BindFilterSql(duckdb::ClientContext &context, const std::string &sql, const CollectionSpec &spec);

} // namespace qod_qdrant
