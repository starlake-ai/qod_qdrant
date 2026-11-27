#pragma once

#include "catalog/config.hpp"
#include "duckdb.hpp"
#include "settings.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qod_qdrant {

//! One validated stage: a dense vector or a keyword text on a registered vector of the relation.
struct QueryBranch {
	enum class Kind { Dense, Bm25 };
	Kind kind = Kind::Dense;
	std::string vector_name;
	//! Dense: the checked vector.
	std::vector<float> vector;
	//! Bm25: the text with ASCII whitespace trimmed, as sent.
	std::string text;

	bool operator==(const QueryBranch &other) const;
};

//! A query value qod_search accepted: Dense and Keyword hold one branch, Hybrid 2..search_max_fusion_branches.
struct SearchQuery {
	enum class Mode { Dense, Keyword, Hybrid };
	Mode mode = Mode::Dense;
	std::vector<QueryBranch> branches;
	//! Hybrid only: what qod_rrf was given (nullopt for NULL) until ResolvePrefetchK replaces it.
	std::optional<int64_t> prefetch_k;

	bool operator==(const SearchQuery &other) const;
	//! Whether any branch is a keyword stage (so the request carries the statistics corpus).
	bool HasKeyword() const;
};

//! `text` without leading and trailing ASCII whitespace (space, \t, \n, \v, \f, \r).
std::string TrimAsciiWhitespace(const std::string &text);

//! qod_search's check of its query value, whatever built it, in the contract's order (section 2, refusals 1
//! to 5; every one search_query_unsupported): 1 the shape (NULL, wrong type, unknown kind, a used field NULL
//! or an unused one set), 2 any bm25 while `allows_lexical` is false, 3 the qod_rrf branch count, 4 every
//! qod_rrf branch dense or bm25, 5 each branch against the relation (vector kind and name, text bytes,
//! vector checks). No I/O.
SearchQuery ValidateQuery(const duckdb::Value &query, const CollectionSpec &spec, bool allows_lexical,
                          const SearchLimits &limits);

//! Refusal 6: a hybrid query's prefetch_k once k is resolved. NULL resolves to max(k, 100) capped at
//! search_max_prefetch_k; the result must be in k..search_max_prefetch_k.
int64_t ResolvePrefetchK(const std::optional<int64_t> &requested, int64_t k, const SearchLimits &limits);

} // namespace qod_qdrant
