#include "lower/validate.hpp"

#include "errors.hpp"
#include "lower/query.hpp"

#include <algorithm>

namespace qod_qdrant {
using namespace duckdb;

namespace {

enum class RawKind { Dense, Bm25, Rrf };

std::optional<RawKind> KindOf(const Value &kind) {
	if (kind.IsNull()) {
		return std::nullopt;
	}
	auto &text = StringValue::Get(kind);
	if (text == "dense") {
		return RawKind::Dense;
	}
	if (text == "bm25") {
		return RawKind::Bm25;
	}
	if (text == "rrf") {
		return RawKind::Rrf;
	}
	return std::nullopt;
}

//! Refusal 1 for the four fields every value and branch carries: what the kind uses is set, the rest NULL.
bool WellFormed(RawKind kind, const Value &vector_name, const Value &vector, const Value &text) {
	switch (kind) {
	case RawKind::Dense:
		return !vector_name.IsNull() && !vector.IsNull() && text.IsNull();
	case RawKind::Bm25:
		return !vector_name.IsNull() && vector.IsNull() && !text.IsNull();
	case RawKind::Rrf:
		return vector_name.IsNull() && vector.IsNull() && text.IsNull();
	}
	return false;
}

struct RawBranch {
	RawKind kind;
	const Value *vector_name;
	const Value *vector;
	const Value *text;
};

[[noreturn]] void Refuse(const std::string &text) {
	Raise(SearchCode::QueryUnsupported, text);
}

QueryBranch CheckedBranch(const RawBranch &raw, const CollectionSpec &spec, const SearchLimits &limits) {
	QueryBranch branch;
	branch.vector_name = StringValue::Get(*raw.vector_name);
	if (raw.kind == RawKind::Dense) {
		auto vector_spec = spec.FindVector(branch.vector_name);
		if (!vector_spec) {
			Refuse("qod_dense names a vector the relation does not declare");
		}
		branch.kind = QueryBranch::Kind::Dense;
		branch.vector = CheckedVector(*raw.vector, *vector_spec);
		return branch;
	}
	if (!spec.FindSparseVector(branch.vector_name)) {
		Refuse("qod_bm25 names a vector the relation does not declare as sparse");
	}
	branch.kind = QueryBranch::Kind::Bm25;
	branch.text = TrimAsciiWhitespace(StringValue::Get(*raw.text));
	if (branch.text.empty() || int64_t(branch.text.size()) > limits.max_query_text_bytes) {
		Refuse("the query text must be 1.." + std::to_string(limits.max_query_text_bytes) + " bytes");
	}
	return branch;
}

} // namespace

bool QueryBranch::operator==(const QueryBranch &other) const {
	return kind == other.kind && vector_name == other.vector_name && vector == other.vector && text == other.text;
}

bool SearchQuery::operator==(const SearchQuery &other) const {
	return mode == other.mode && branches == other.branches && prefetch_k == other.prefetch_k;
}

bool SearchQuery::HasKeyword() const {
	return std::any_of(branches.begin(), branches.end(),
	                   [](const QueryBranch &b) { return b.kind == QueryBranch::Kind::Bm25; });
}

std::string TrimAsciiWhitespace(const std::string &text) {
	auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; };
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && space(text[begin])) {
		++begin;
	}
	while (end > begin && space(text[end - 1])) {
		--end;
	}
	return text.substr(begin, end - begin);
}

SearchQuery ValidateQuery(const Value &query, const CollectionSpec &spec, bool allows_lexical, const SearchLimits &limits) {
	// 1. The shape, over the whole value, before anything else.
	if (query.IsNull() || query.type() != QueryType()) {
		Refuse(kQueryShape);
	}
	auto &parts = StructValue::GetChildren(query);
	auto kind = KindOf(parts[0]);
	if (!kind || !WellFormed(*kind, parts[1], parts[2], parts[3])) {
		Refuse(kQueryShape);
	}
	std::vector<RawBranch> raw;
	if (*kind == RawKind::Rrf) {
		if (parts[5].IsNull()) {
			Refuse(kQueryShape);
		}
		for (auto &element : ListValue::GetChildren(parts[5])) {
			if (element.IsNull()) {
				Refuse(kQueryShape);
			}
			auto &fields = StructValue::GetChildren(element);
			auto branch_kind = KindOf(fields[0]);
			if (!branch_kind || !WellFormed(*branch_kind, fields[1], fields[2], fields[3])) {
				Refuse(kQueryShape);
			}
			raw.push_back({*branch_kind, &fields[1], &fields[2], &fields[3]});
		}
	} else {
		if (!parts[4].IsNull() || !parts[5].IsNull()) {
			Refuse(kQueryShape);
		}
		raw.push_back({*kind, &parts[1], &parts[2], &parts[3]});
	}
	// 2. The profile gate, before any other per-branch check.
	if (!allows_lexical &&
	    std::any_of(raw.begin(), raw.end(), [](const RawBranch &b) { return b.kind == RawKind::Bm25; })) {
		Refuse("keyword search is not enabled for this source");
	}
	SearchQuery result;
	if (*kind == RawKind::Rrf) {
		// 3. The branch count, 4. the branch kinds (no RRF inside RRF).
		if (raw.size() < 2 || int64_t(raw.size()) > limits.max_fusion_branches) {
			Refuse("qod_rrf takes 2.." + std::to_string(limits.max_fusion_branches) + " branches");
		}
		for (auto &b : raw) {
			if (b.kind == RawKind::Rrf) {
				Refuse("qod_rrf branches must be qod_dense or qod_bm25");
			}
		}
		result.mode = SearchQuery::Mode::Hybrid;
		if (!parts[4].IsNull()) {
			result.prefetch_k = parts[4].GetValue<int64_t>();
		}
	} else {
		result.mode = *kind == RawKind::Dense ? SearchQuery::Mode::Dense : SearchQuery::Mode::Keyword;
	}
	// 5. Each branch, in order, against the relation.
	for (auto &b : raw) {
		result.branches.push_back(CheckedBranch(b, spec, limits));
	}
	return result;
}

int64_t ResolvePrefetchK(const std::optional<int64_t> &requested, int64_t k, const SearchLimits &limits) {
	auto value = requested ? *requested : std::min(std::max<int64_t>(k, 100), limits.max_prefetch_k);
	if (value < k || value > limits.max_prefetch_k) {
		Raise(SearchCode::QueryUnsupported,
		      "prefetch_k must be in " + std::to_string(k) + ".." + std::to_string(limits.max_prefetch_k));
	}
	return value;
}

} // namespace qod_qdrant
