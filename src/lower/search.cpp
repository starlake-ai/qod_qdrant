#include "lower/search.hpp"

#include "arrow/payload.hpp"
#include "ast/request.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "errors.hpp"
#include "linked_function_classes.hpp"
#include "lower/lowering.hpp"
#include "lower/request_guard.hpp"
#include "lower/scan_data.hpp"
#include "lower/validate.hpp"
#include "protected_scan.hpp"
#include "settings.hpp"
#include "transport/remote.hpp"

#include <algorithm>
#include <mutex>

namespace qod_qdrant {
using namespace duckdb;

namespace {

constexpr const char *kNotARelation = "relation must name a table of an attached search source";

struct SearchState : public ScanState {
	bool fetched = false;
};

const Value *Named(TableFunctionBindInput &input, const char *name) {
	for (auto &entry : input.named_parameters) {
		// Matched as DuckDB matches parameter names: a FILTER_SQL spelling must never drop U.
		if (StringUtil::CIEquals(entry.first.GetIdentifierName(), name)) {
			return &entry.second;
		}
	}
	return nullptr;
}

//! Whether the call names the keyword argument itself (the parsed call keeps the names after binding), as
//! opposed to DuckDB filling in its default.
bool Passed(TableFunctionBindInput &input, const char *name) {
	if (!input.ref.function || input.ref.function->GetExpressionType() != ExpressionType::FUNCTION) {
		return false;
	}
	for (auto &argument : input.ref.function->Cast<FunctionExpression>().GetArguments()) {
		if (StringUtil::CIEquals(argument.GetName().GetIdentifierName(), name)) {
			return true;
		}
	}
	return false;
}

//! The request a raw search reads (contract: lower/request_guard.hpp): a constant JSON text within
//! search_max_request_text_bytes, checked by the guard against the source's enabled request fields, and refused
//! under exact_isolated when any level is a keyword stage, as qod_bm25 is. All before any I/O.
CheckedRequest BindRequest(TableFunctionBindInput &input, QdrantTableEntry &table, const SearchLimits &limits) {
	constexpr JsonLimits kRequestJsonLimits {32, 64, 100000};
	auto value = Named(input, "request");
	if (!value || value->IsNull()) {
		Raise(SearchCode::QueryUnsupported, "request: must be a JSON text");
	}
	auto text = value->ToString();
	if (int64_t(text.size()) > limits.max_request_text_bytes) {
		Raise(SearchCode::QueryUnsupported,
		      "request: must be at most " + std::to_string(limits.max_request_text_bytes) + " bytes");
	}
	JsonNode parsed;
	try {
		parsed = ParseJson(text, kRequestJsonLimits);
	} catch (std::exception &e) {
		// "<what> at offset <n>", never a value.
		Raise(SearchCode::QueryUnsupported, std::string("request: ") + e.what());
	}
	auto checked = CheckRequest(parsed, table.Spec(), table.Source().request_fields, limits);
	if (checked.keyword && !table.Source().config.AllowsLexical()) {
		Raise(SearchCode::QueryUnsupported, "keyword search is not enabled for this source");
	}
	return checked;
}

//! The point ids a raw request names, checked under the sealed filter before the search: one scroll for
//! has_id AND F. A hidden id and a missing one answer the same refusal, so an id query cannot probe the rows a
//! policy hides.
void PrecheckIds(ClientContext &context, const QdrantScanData &data) {
	auto &spec = data.table.Spec();
	auto &source = data.table.Source();
	auto &ids = data.raw->point_ids;
	std::vector<JsonNode> must;
	if (data.remote_filter.kind != FilterExpr::Kind::True) {
		must.push_back(FilterJson(data.remote_filter, spec));
	}
	must.push_back(JsonNode::Object({{"has_id", JsonNode::Array(ids)}}));
	auto body = WriteJson(JsonNode::Object({{"filter", JsonNode::Object({{"must", JsonNode::Array(std::move(must))}})},
	                                        {"limit", JsonNode::Int(int64_t(ids.size()))},
	                                        {"with_payload", JsonNode::Bool(false)},
	                                        {"with_vector", JsonNode::Bool(false)}}));
	auto reply = RemotePost(context, source.Target(), ReadLimits(context), ScrollPath(spec), body, true);
	ScrollPage page;
	try {
		page = ParseScrollResponse(reply);
	} catch (ResponseBudgetError &e) {
		Fail(source.source_id, SearchCode::BudgetExceeded, kResponseOverBudget, e.what(), source.api_key);
	} catch (ResponseError &e) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch, e.what(), source.api_key);
	}
	if (page.points.size() < ids.size()) {
		Raise(SearchCode::QueryUnsupported, "query: point not found");
	}
}

unique_ptr<FunctionData> SearchBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &types,
                                    vector<Identifier> &names) {
	auto &relation = input.inputs[0];
	if (relation.IsNull()) {
		Raise(SearchCode::QueryUnsupported, kNotARelation);
	}
	QdrantTableEntry *table = nullptr;
	try {
		auto entry = Catalog::GetEntry<TableCatalogEntry>(context, QualifiedName::Parse(relation.ToString()),
		                                                  OnEntryNotFound::RETURN_NULL);
		table = entry ? dynamic_cast<QdrantTableEntry *>(entry.get()) : nullptr;
	} catch (CatalogException &) {
		// An unknown catalog or schema: the same refusal as an unknown or foreign table.
		table = nullptr;
	} catch (BinderException &) {
		table = nullptr;
	} catch (ParserException &) {
		// A name QualifiedName::Parse cannot read (unbalanced quotes).
		table = nullptr;
	}
	if (!table) {
		Raise(SearchCode::QueryUnsupported, kNotARelation);
	}
	// What a base table reference records: a prepared statement whose catalog is detached (and maybe attached
	// again) is then rebound instead of running with a bind data that holds the old, freed table entry.
	if (input.binder) {
		input.binder->GetStatementProperties().RegisterDBRead(table->ParentCatalog(), context);
	}
	auto &spec = table->Spec();
	for (auto &f : spec.fields) {
		types.push_back(SqlType(f.type));
		names.emplace_back(f.name);
	}
	auto data = make_uniq<QdrantScanData>(*table, true);
	auto limits = ReadLimits(context);

	if (Passed(input, "query") && Passed(input, "request")) {
		Raise(SearchCode::QueryUnsupported, "pass exactly one of query or request");
	}
	if (Passed(input, "request")) {
		data->raw = BindRequest(input, *table, limits);
		std::string fields;
		for (auto &f : table->Source().request_fields) {
			fields += f + ",";
		}
		data->raw_key = WriteJson(data->raw->request) + '\x1d' + fields;
	} else {
		auto query = Named(input, "query");
		if (!query) {
			Raise(SearchCode::QueryUnsupported, kQueryShape);
		}
		// The same checks whatever built the value (a hand-spelled struct included), the profile gate among them:
		// all before any I/O (contract section 2, refusals 1 to 5).
		data->query = ValidateQuery(*query, spec, table->Source().config.AllowsLexical(), limits);
	}

	auto k = Named(input, "k");
	if (k && k->IsNull() && Passed(input, "k")) {
		// An explicit NULL is a client bug, not a request for the default.
		Raise(SearchCode::QueryUnsupported, "k must not be NULL");
	}
	// The trusted init may lower search_max_k below search_default_k: the default then follows the cap.
	data->k = (!k || k->IsNull()) ? std::min(limits.default_k, limits.max_k) : k->GetValue<int64_t>();
	if (data->k < 1 || data->k > limits.max_k) {
		Raise(SearchCode::QueryUnsupported, "k must be in 1.." + std::to_string(limits.max_k));
	}
	if (!data->raw && data->query.mode == SearchQuery::Mode::Hybrid) {
		// Refusal 6, once k is known.
		data->query.prefetch_k = ResolvePrefetchK(data->query.prefetch_k, data->k, limits);
	}

	auto filter = Named(input, "filter_sql");
	// The default is the blank string: NULL is only ever an explicit filter_sql => NULL.
	if (filter && filter->IsNull()) {
		Raise(SearchCode::QueryUnsupported, "filter_sql must not be NULL");
	}
	auto sql = filter ? filter->ToString() : std::string();
	data->user_filter = BindFilterSql(context, sql, spec);
	// U alone over the cap is the call's own fault, refused here in the connector's words; P AND U
	// over it is refused when core offers the row policies (AcceptRowPolicies).
	if (int64_t(NodeCount(data->user_filter)) > limits.max_filter_nodes) {
		Raise(SearchCode::BudgetExceeded, "filter_sql has more filter nodes than search_max_filter_nodes");
	}
	CollectFields(data->user_filter, data->user_fields);
	data->LowerWithoutPolicies(context);
	return std::move(data);
}

virtual_column_map_t VirtualColumnsOf(ClientContext &, optional_ptr<FunctionData>) {
	return VirtualColumns();
}

unique_ptr<GlobalTableFunctionState> SearchInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = ScanDataOf(input.bind_data);
	// Once, up front, then the seal: both before any I/O.
	CheckScanColumns(input.column_ids, data.table.Spec());
	data.CheckAtExecution(context);
	auto state = make_uniq<SearchState>();
	state->column_ids.assign(input.column_ids.begin(), input.column_ids.end());
	state->decode_fields = DecodeFields(data, state->column_ids);
	return std::move(state);
}

//! The request body of the sealed query. Every stage is rechecked against the attached relation first: a
//! bind data that does not match it never reaches the wire.
std::string RequestBody(const QdrantScanData &data, const std::vector<std::string> &include) {
	auto &spec = data.table.Spec();
	auto &query = data.query;
	for (auto &b : query.branches) {
		bool known = b.kind == QueryBranch::Kind::Dense
		                 ? spec.FindVector(b.vector_name) &&
		                       int64_t(b.vector.size()) == spec.FindVector(b.vector_name)->dimensions
		                 : spec.FindSparseVector(b.vector_name) && data.table.Source().config.AllowsLexical();
		if (!known) {
			Raise(SearchCode::QueryUnsupported, kQueryShape);
		}
	}
	if (query.branches.empty() || (query.mode == SearchQuery::Mode::Hybrid && !query.prefetch_k)) {
		Raise(SearchCode::QueryUnsupported, kQueryShape);
	}
	// The mode must agree with its branches: Dense is one dense branch, Keyword one bm25 branch, Hybrid two or
	// more of either.
	size_t dense = 0;
	for (auto &b : query.branches) {
		dense += b.kind == QueryBranch::Kind::Dense ? 1 : 0;
	}
	auto bm25 = query.branches.size() - dense;
	bool agrees = query.mode == SearchQuery::Mode::Dense
	                  ? dense == 1 && bm25 == 0
	                  : query.mode == SearchQuery::Mode::Keyword ? bm25 == 1 && dense == 0 : query.branches.size() >= 2;
	if (!agrees) {
		Raise(SearchCode::QueryUnsupported, kQueryShape);
	}
	auto &first = query.branches[0];
	switch (query.mode) {
	case SearchQuery::Mode::Dense:
		return QueryRequestJson({first.vector, first.vector_name, data.remote_filter, data.k, include}, spec);
	case SearchQuery::Mode::Keyword:
		return KeywordRequestJson({first.text, first.vector_name, data.remote_filter, data.corpus_filter, data.k, include},
		                          spec);
	case SearchQuery::Mode::Hybrid:
		break;
	}
	HybridRequest request {{}, data.remote_filter, data.corpus_filter, *query.prefetch_k, data.k, include};
	for (auto &b : query.branches) {
		request.stages.push_back({b.kind == QueryBranch::Kind::Bm25, b.vector, b.text, b.vector_name});
	}
	return HybridRequestJson(request, spec);
}

void Fetch(ClientContext &context, const QdrantScanData &data, SearchState &state) {
	state.fetched = true;
	state.done = true;
	if (data.remote_filter.kind == FilterExpr::Kind::False) {
		return;
	}
	auto &spec = data.table.Spec();
	auto &source = data.table.Source();
	std::string request;
	if (data.raw) {
		if (!data.raw->point_ids.empty()) {
			PrecheckIds(context, data);
		}
		request = WriteRequest(*data.raw, spec, data.remote_filter, data.corpus_filter, state.decode_fields, data.k).body;
	} else {
		request = RequestBody(data, IncludePaths(spec, state.decode_fields));
	}
	auto body = RemotePost(context, source.Target(), ReadLimits(context), QueryPath(spec), request, true);
	std::vector<QdrantPoint> hits;
	try {
		hits = ParseQueryResponse(body);
	} catch (ResponseBudgetError &e) {
		Fail(source.source_id, SearchCode::BudgetExceeded, kResponseOverBudget, e.what(), source.api_key);
	} catch (ResponseError &e) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch, e.what(), source.api_key);
	}
	if (int64_t(hits.size()) > data.k) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch, "more hits than k", source.api_key);
	}
	for (auto &hit : hits) {
		state.rows.push_back(CheckedRow(hit, data, state.decode_fields));
	}
}

void SearchExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = ScanDataOf(input.bind_data);
	auto &state = input.global_state->Cast<SearchState>();
	if (!state.fetched) {
		Fetch(context, data, state);
	}
	idx_t out = 0;
	EmitBuffered(state, data.table.Spec(), output, out);
	output.SetChildCardinality(out);
}

} // namespace

TableFunction SearchFunction() {
	FunctionSignature signature;
	signature.AddParameter(LogicalType::VARCHAR);
	// Each keyword-only parameter needs a default, or a call that omits it fails to bind.
	signature.AddKeywordOnly(Identifier("query"), QueryType(), Value(QueryType()));
	signature.AddKeywordOnly(Identifier("filter_sql"), LogicalType::VARCHAR, Value(""));
	signature.AddKeywordOnly(Identifier("request"), LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
	signature.AddKeywordOnly(Identifier("k"), LogicalType::BIGINT, Value(LogicalType::BIGINT));
	TableFunction function(Identifier("qod_search"), std::move(signature), SearchExecute, SearchBind, SearchInit);
	function.get_table_entry = TableEntryOf;
	function.get_virtual_columns = VirtualColumnsOf;
	function.projection_pushdown = true;
	function.filter_pushdown = false;
	function.filter_prune = false;
	return function;
}

void RegisterSearchFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(DenseFunction());
	loader.RegisterFunction(Bm25Function());
	loader.RegisterFunction(RrfFunction());
	loader.RegisterFunction(SearchFunction());
	qod::RegisterBoundedFunction("qod_search");
	// Core freezes linked classes before serving; a later database open in the same process must not
	// register again (it would throw).
	static std::once_flag classes;
	std::call_once(classes, [] {
		qod::RegisterLinkedFunctionClass("qod_search", "table", qod::FunctionClass::Allowed);
		qod::RegisterLinkedFunctionClass("qod_dense", "scalar", qod::FunctionClass::Allowed);
		qod::RegisterLinkedFunctionClass("qod_bm25", "scalar", qod::FunctionClass::Allowed);
		qod::RegisterLinkedFunctionClass("qod_rrf", "scalar", qod::FunctionClass::Allowed);
	});
}

} // namespace qod_qdrant
