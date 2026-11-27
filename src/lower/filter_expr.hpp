#pragma once

#include "ast/json.hpp"
#include "catalog/config.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace qod_qdrant {

//! A typed payload value: what a lowered literal holds and what a decoded payload field holds.
struct FieldValue {
	FieldType type = FieldType::Varchar;
	std::string text;
	int64_t integer = 0;
	bool boolean = false;
	//! Double fields only (projection, never lowered).
	double real = 0;

	bool operator==(const FieldValue &other) const;
};

//! The filters slice 1 lowers (spec 5.3). `field` indexes CollectionSpec::fields. Match is `field = value`
//! (VARCHAR, BIGINT, BOOLEAN), In is `field IN (values)` (VARCHAR, BIGINT); a missing or null field never
//! matches either.
struct FilterExpr {
	enum class Kind { True, False, Match, In, And, Or };
	Kind kind = Kind::True;
	size_t field = 0;
	std::vector<FieldValue> values;
	std::vector<FilterExpr> children;

	static FilterExpr True();
	static FilterExpr False();
	static FilterExpr Match(size_t field, FieldValue value);
	static FilterExpr In(size_t field, std::vector<FieldValue> values);
	static FilterExpr And(std::vector<FilterExpr> children);
	static FilterExpr Or(std::vector<FilterExpr> children);
};

//! Folds TRUE and FALSE away and flattens nested AND/OR: an empty IN is FALSE, an AND with a FALSE child
//! is FALSE, an OR with a TRUE child is TRUE, a one-child AND/OR is that child. The root of the result is
//! TRUE or FALSE only when the whole filter is.
FilterExpr Simplify(const FilterExpr &expr);

//! Match and In count 1, AND/OR 1 plus their children, TRUE/FALSE 0.
size_t NodeCount(const FilterExpr &expr);

void CollectFields(const FilterExpr &expr, std::set<size_t> &fields);

//! A literal as Qdrant JSON: a string, an integer (exact int64 text) or a boolean.
JsonNode ValueJson(const FieldValue &value);

//! The Qdrant filter object of a simplified filter: AND is {"must":[...]}, OR is {"should":[...]}, a lone
//! condition is {"must":[condition]}. Throws std::logic_error for a TRUE or FALSE root, or for an IN without
//! values or an AND/OR without children anywhere in the tree (none survives Simplify).
JsonNode FilterJson(const FilterExpr &expr, const CollectionSpec &spec);

//! The filter's truth on decoded values (nullopt: missing or null), with Qdrant's matching semantics.
bool Evaluate(const FilterExpr &expr, const std::vector<std::optional<FieldValue>> &row);

} // namespace qod_qdrant
