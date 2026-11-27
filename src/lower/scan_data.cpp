#include "lower/scan_data.hpp"

#include "host_flags.hpp"

#include "crypto.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "errors.hpp"
#include "lower/lowering.hpp"
#include "settings.hpp"

namespace qod_qdrant {
using namespace duckdb;

const char *const kSchemaMismatch = "the search source returned data that does not match the registered schema";
const char *const kResponseOverBudget = "the search response holds more values than the node parses";

namespace {

std::string FilterKey(const FilterExpr &expr, const CollectionSpec &spec) {
	if (expr.kind == FilterExpr::Kind::True) {
		return "TRUE";
	}
	if (expr.kind == FilterExpr::Kind::False) {
		return "FALSE";
	}
	return WriteJson(FilterJson(expr, spec));
}

//! The query's stages as the seal covers them: mode, then per stage its kind, vector name and text, then
//! prefetch_k. The dense vector stays out, as in slice 1.
std::string QueryKey(const SearchQuery &query) {
	std::string key = std::to_string(int(query.mode));
	for (auto &b : query.branches) {
		key += '\x1e' + std::to_string(int(b.kind)) + '\x1d' + b.vector_name + '\x1d' + b.text;
	}
	return key + '\x1e' + (query.prefetch_k ? std::to_string(*query.prefetch_k) : std::string("-"));
}

} // namespace

unique_ptr<FunctionData> QdrantScanData::Copy() const {
	auto copy = make_uniq<QdrantScanData>(table, bounded);
	copy->user_filter = user_filter;
	copy->user_fields = user_fields;
	copy->query = query;
	copy->k = k;
	copy->raw = raw;
	copy->raw_key = raw_key;
	copy->accepted = accepted;
	copy->remote_filter = remote_filter;
	copy->corpus_filter = corpus_filter;
	copy->policy_fields = policy_fields;
	copy->accepted_digest = accepted_digest;
	copy->sealed_config_digest = sealed_config_digest;
	copy->sealed_guarantee = sealed_guarantee;
	copy->sealed_generation = sealed_generation;
	copy->sealed_schema_version = sealed_schema_version;
	copy->seal = seal;
	copy->CopySealFrom(*this);
	return std::move(copy);
}

bool QdrantScanData::Equals(const FunctionData &other_p) const {
	auto other_data = dynamic_cast<const QdrantScanData *>(&other_p);
	if (!other_data) {
		return false;
	}
	auto &other = *other_data;
	return &table == &other.table && bounded == other.bounded && query == other.query && k == other.k &&
	       raw_key == other.raw_key &&
	       accepted == other.accepted && seal == other.seal &&
	       SealedDigest() == other.SealedDigest() &&
	       FilterKey(user_filter, table.Spec()) == FilterKey(other.user_filter, table.Spec());
}

qod::LoweringResult QdrantScanData::AcceptRowPolicies(ClientContext &context, const LogicalGet &get,
                                                      const std::vector<qod::RowPolicy> &policies,
                                                      const std::vector<unique_ptr<Expression>> &predicates) {
	auto p = predicates.empty() ? std::optional<FilterExpr>(FilterExpr::True())
	                            : LowerPolicyPredicates(context, get, predicates, table.Spec());
	return LowerFilters(context, std::move(p), policies);
}

void QdrantScanData::LowerWithoutPolicies(ClientContext &context) {
	if (HostEnforcesPolicies()) {
		return;
	}
	LowerFilters(context, FilterExpr::True(), {});
	HostSealUnprotected(*this);
}

qod::LoweringResult QdrantScanData::LowerFilters(ClientContext &context, std::optional<FilterExpr> p,
                                                 const std::vector<qod::RowPolicy> &policies) {
	auto &spec = table.Spec();
	auto limits = ReadLimits(context);
	accepted = false;
	policy_fields.clear();
	qod::LoweringResult result {true, ""};
	FilterExpr combined;
	FilterExpr corpus = FilterExpr::True();
	if (p) {
		combined = Simplify(FilterExpr::And({*p, user_filter}));
		if (int64_t(NodeCount(combined)) > limits.max_filter_nodes) {
			if (bounded) {
				return {false, "search_budget_exceeded"};
			}
			p.reset();
		}
	}
	if (p && bounded) {
		// C = P: the same exact lowering without U, TRUE when no row policy applies (whole-collection
		// statistics). The node cap applies to it too, when it is sent.
		corpus = Simplify(*p);
		if (query.HasKeyword() && int64_t(NodeCount(corpus)) > limits.max_filter_nodes) {
			return {false, "search_budget_exceeded"};
		}
	}
	if (!p && bounded) {
		return {false, "search_policy_unsupported"};
	}
	if (!p) {
		// Unbounded scan: U remotely, P only in core's local filter above the scan.
		combined = Simplify(user_filter);
		result = {false, "search_policy_unsupported"};
	} else {
		CollectFields(*p, policy_fields);
	}
	remote_filter = std::move(combined);
	corpus_filter = std::move(corpus);
	accepted = true;
	accepted_digest = qod::PolicyDigest(policies);
	sealed_config_digest = table.Source().config_digest;
	sealed_guarantee = table.Source().config.guarantee;
	sealed_generation = spec.generation;
	sealed_schema_version = spec.schema_version;
	seal = ComputeSeal();
	return result;
}

std::vector<qod::FieldUse> QdrantScanData::FieldUses() const {
	auto &spec = table.Spec();
	std::vector<qod::FieldUse> uses;
	std::set<size_t> filter_fields = user_fields;
	filter_fields.insert(policy_fields.begin(), policy_fields.end());
	if (raw) {
		// The raw request's own filter, formula and order_by keys: a column policy on any refuses it.
		filter_fields.insert(raw->filter_fields.begin(), raw->filter_fields.end());
	}
	for (auto f : filter_fields) {
		uses.push_back({spec.fields.at(f).name, qod::FieldUse::Kind::Filter});
	}
	// The dependsOn of every stage's vector, dense or sparse, each field once.
	std::set<std::string> lineage;
	auto lineage_of = [&](const std::vector<std::string> &depends_on) {
		for (auto &name : depends_on) {
			if (lineage.insert(name).second) {
				uses.push_back({name, qod::FieldUse::Kind::VectorLineage});
			}
		}
	};
	if (raw) {
		for (auto &name : raw->vectors) {
			if (auto v = spec.FindVector(name)) {
				lineage_of(v->depends_on);
			} else if (auto s = spec.FindSparseVector(name)) {
				lineage_of(s->depends_on);
			}
		}
	}
	for (auto &b : query.branches) {
		const std::vector<std::string> *depends_on = nullptr;
		if (b.kind == QueryBranch::Kind::Dense) {
			if (auto v = spec.FindVector(b.vector_name)) {
				depends_on = &v->depends_on;
			}
		} else if (auto v = spec.FindSparseVector(b.vector_name)) {
			depends_on = &v->depends_on;
		}
		if (!depends_on) {
			continue;
		}
		for (auto &name : *depends_on) {
			if (lineage.insert(name).second) {
				uses.push_back({name, qod::FieldUse::Kind::VectorLineage});
			}
		}
	}
	return uses;
}

std::string QdrantScanData::ComputeSeal() const {
	auto &spec = table.Spec();
	std::string canonical = accepted_digest + '\x1f' + sealed_config_digest + '\x1f' + sealed_guarantee + '\x1f' +
	                        std::to_string(sealed_generation) + '\x1f' + std::to_string(sealed_schema_version) + '\x1f' +
	                        FilterKey(remote_filter, spec) + '\x1f' + FilterKey(corpus_filter, spec) + '\x1f' +
	                        QueryKey(query) + '\x1f' + std::to_string(k) + '\x1f' + raw_key;
	return qod::Hex(qod::Sha256(canonical));
}

void QdrantScanData::CheckAtExecution(ClientContext &context) const {
	RequireSealed();
	constexpr const char *kChanged = "the authorization of this search changed; run the statement again";
	if (!accepted || SealedDigest() != accepted_digest || ComputeSeal() != seal) {
		Raise(SearchCode::PolicyChanged, kChanged);
	}
	auto &spec = table.Spec();
	QualifiedName name(table.ParentCatalog().GetName(), Identifier(spec.schema), Identifier(spec.table));
	auto current = Catalog::GetEntry<TableCatalogEntry>(context, name, OnEntryNotFound::RETURN_NULL);
	auto entry = current ? dynamic_cast<QdrantTableEntry *>(current.get()) : nullptr;
	if (!entry || entry->Source().config_digest != sealed_config_digest ||
	    entry->Source().config.guarantee != sealed_guarantee || entry->Spec().generation != sealed_generation ||
	    entry->Spec().schema_version != sealed_schema_version) {
		Raise(SearchCode::PolicyChanged, kChanged);
	}
}

std::set<size_t> QdrantScanData::FilterFields() const {
	std::set<size_t> fields;
	CollectFields(remote_filter, fields);
	return fields;
}

const QdrantScanData &ScanDataOf(optional_ptr<const FunctionData> bind_data) {
	auto data = bind_data ? dynamic_cast<const QdrantScanData *>(bind_data.get()) : nullptr;
	if (!data) {
		Raise(SearchCode::QueryUnsupported, "the scan does not carry search bind data");
	}
	return *data;
}

optional_ptr<TableCatalogEntry> TableEntryOf(optional_ptr<const FunctionData> bind_data) {
	auto data = bind_data ? dynamic_cast<const QdrantScanData *>(bind_data.get()) : nullptr;
	return data ? &data->table : nullptr;
}

std::set<size_t> DecodeFields(const QdrantScanData &data, const std::vector<column_t> &column_ids) {
	auto fields = data.FilterFields();
	for (auto id : column_ids) {
		if (id < data.table.Spec().fields.size()) {
			fields.insert(size_t(id));
		}
	}
	return fields;
}

DecodedRow CheckedRow(const QdrantPoint &point, const QdrantScanData &data, const std::set<size_t> &decode_fields) {
	auto &source = data.table.Source();
	DecodedRow row;
	try {
		row = DecodePoint(point, data.table.Spec(), decode_fields);
	} catch (ResponseError &e) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch, e.what(), source.api_key);
	}
	if (!Evaluate(data.remote_filter, row.values)) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch,
		     "point " + row.id + " does not satisfy the lowered filter", source.api_key);
	}
	return row;
}

void EmitBuffered(ScanState &state, const CollectionSpec &spec, DataChunk &output, idx_t &out) {
	while (out < STANDARD_VECTOR_SIZE && state.next < state.rows.size()) {
		EmitRow(state.rows[state.next++], state.column_ids, spec, output, out++);
	}
}

} // namespace qod_qdrant
