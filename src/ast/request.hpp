#pragma once

#include "ast/json.hpp"
#include "catalog/config.hpp"
#include "lower/filter_expr.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace qod_qdrant {

//! POST /collections/{c}/points/scroll. `filter` is simplified and not FALSE (a FALSE filter is never sent);
//! a TRUE filter (no row policy, no filter_sql) is left out of the body.
struct ScrollRequest {
	FilterExpr filter;
	int64_t limit = 0;
	std::optional<JsonNode> offset;
	std::vector<std::string> include;
};

//! POST /collections/{c}/points/query: dense search on the named vector. A TRUE `filter` is left out.
struct QueryRequest {
	std::vector<float> vector;
	std::string using_vector;
	FilterExpr filter;
	int64_t limit = 0;
	std::vector<std::string> include;
};

//! POST /collections/{c}/points/query, keyword: BM25 of `text` on the sparse vector `using_vector`.
//! `filter` is F = P AND U, `corpus` is C = P (the IDF statistics); both simplified, neither FALSE. A TRUE F
//! is left out of the body; a TRUE C (no row policy) leaves `params` out: whole-collection statistics.
struct KeywordRequest {
	std::string text;
	std::string using_vector;
	FilterExpr filter;
	FilterExpr corpus;
	int64_t limit = 0;
	std::vector<std::string> include;
};

//! One prefetch of a hybrid request: a dense vector or a keyword text on `using_vector`.
struct HybridStage {
	bool keyword = false;
	std::vector<float> vector;
	std::string text;
	std::string using_vector;
};

//! POST /collections/{c}/points/query, hybrid: RRF over one prefetch per stage, each limited to
//! `prefetch_limit` and filtered by F; keyword stages carry the corpus C, the top level never does. A TRUE F or
//! C is left out as in KeywordRequest.
struct HybridRequest {
	std::vector<HybridStage> stages;
	FilterExpr filter;
	FilterExpr corpus;
	int64_t prefetch_limit = 0;
	int64_t limit = 0;
	std::vector<std::string> include;
};

std::string ScrollPath(const CollectionSpec &spec);
std::string QueryPath(const CollectionSpec &spec);

//! The payload paths of `fields`, in declared field order. Empty means `"with_payload":false`.
std::vector<std::string> IncludePaths(const CollectionSpec &spec, const std::set<size_t> &fields);

//! {["filter":F,]"limit":N[,"offset":ID],"with_payload":{"include":[...]}|false,"with_vector":false}
std::string ScrollRequestJson(const ScrollRequest &request, const CollectionSpec &spec);
//! {"query":[...],"using":V[,"filter":F],"limit":k,"with_payload":{"include":[...]}|false,"with_vector":false}
std::string QueryRequestJson(const QueryRequest &request, const CollectionSpec &spec);
//! {"query":{"text":T,"model":"qdrant/bm25"},"using":V[,"filter":F][,"params":{"idf":{"corpus":C}}],"limit":k,
//!  "with_payload":...,"with_vector":false}
std::string KeywordRequestJson(const KeywordRequest &request, const CollectionSpec &spec);
//! {"prefetch":[stage...],"query":{"rrf":{}}[,"filter":F],"limit":k,"with_payload":...,"with_vector":false}, a
//! dense stage {"query":[...],"using":V[,"filter":F],"limit":p}, a keyword stage
//! {"query":{"text":T,"model":"qdrant/bm25"},"using":V[,"filter":F][,"params":{"idf":{"corpus":C}}],"limit":p}.
std::string HybridRequestJson(const HybridRequest &request, const CollectionSpec &spec);

//! A response that is not the shape the global constraints pin. The message is privileged detail.
class ResponseError : public std::runtime_error {
public:
	explicit ResponseError(const std::string &what) : std::runtime_error(what) {
	}
};

//! A response that exceeds a parse budget (kResponseJsonLimits.max_nodes): a resource limit, not a shape error.
//! Deliberately not a ResponseError: callers catch it (before or beside ResponseError) and fail the statement
//! with search_budget_exceeded, as for search_max_response_bytes. The message is privileged detail.
class ResponseBudgetError : public std::runtime_error {
public:
	explicit ResponseBudgetError(const std::string &what) : std::runtime_error(what) {
	}
};

//! A point id as Qdrant writes one: a JSON unsigned integer that fits u64, or a UUID string (8-4-4-4-12 hex
//! digits, either case).
bool IsPointId(const JsonNode &id);

struct QdrantPoint {
	JsonNode id;
	std::optional<double> score;
	//! Null when the point carries no payload.
	JsonNode payload;
};

struct ScrollPage {
	std::vector<QdrantPoint> points;
	std::optional<JsonNode> next_offset;
};

//! Both parsers throw ResponseError for a body of another shape and ResponseBudgetError for one over budget.
//! A scroll result must carry next_page_offset (null on the last page).
ScrollPage ParseScrollResponse(const std::string &body);
//! Every point must carry a score.
std::vector<QdrantPoint> ParseQueryResponse(const std::string &body);

} // namespace qod_qdrant
