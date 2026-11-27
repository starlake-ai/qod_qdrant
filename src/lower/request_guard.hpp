#pragma once

#include "ast/json.hpp"
#include "catalog/config.hpp"
#include "lower/filter_expr.hpp"
#include "settings.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace qod_qdrant {

//! A raw Qdrant points/query body (qod_search(request := ...)) checked at bind, before core hands the scan its
//! row policies: every rule that needs no filter. `filter_fields` are the declared fields the request's own
//! filter, formula and order_by read (reported to core as FieldUse::Kind::Filter, so a column policy on any of
//! them refuses the statement), `point_ids` every point id it names (checked under the policy before the
//! search), `keyword` whether any level is a keyword (BM25) stage.
struct CheckedRequest {
	JsonNode request;
	std::vector<JsonNode> point_ids;
	std::set<size_t> filter_fields;
	bool keyword = false;
	//! Every declared vector a level names in `using`: their dependsOn are reported as vector lineage.
	std::set<std::string> vectors;
};

//! Refuses (search_query_unsupported: <json path>: <rule>, never a value) whatever the guard does not know
//! unless `enabled` (the tenant's REQUEST_FIELDS) names its path, and an undeclared payload key
//! (search_field_denied). Paths drop prefetch levels: `params.acorn` names that key at every level.
CheckedRequest CheckRequest(const JsonNode &request, const CollectionSpec &spec,
                            const std::vector<std::string> &enabled, const SearchLimits &limits);

//! The body qod-search sends, from a checked request and the scan's sealed filters: F (row policy AND
//! filter_sql) ANDed into the filter of every level, C (row policy) as the IDF corpus of every keyword level,
//! a Document's declared options written, and at the top level limit k, the selected declared payload paths
//! and no vectors. `digest` is the body's SHA-256, for the seal. F must not be FALSE.
struct GuardedRequest {
	std::string body;
	std::string digest;
};

GuardedRequest WriteRequest(const CheckedRequest &checked, const CollectionSpec &spec, const FilterExpr &filter,
                            const FilterExpr &corpus, const std::set<size_t> &selected, int64_t k);

//! A request path pattern: [a-z_]+(\.[a-z_]+){0,4} (TenantSettings.scala).
bool IsRequestFieldPattern(const std::string &pattern);

//! The REQUEST_FIELDS of an ATTACH: a JSON array of patterns; search_config_invalid otherwise.
std::vector<std::string> ParseRequestFields(const std::string &text);

} // namespace qod_qdrant
