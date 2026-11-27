#include "lower/scroll_scan.hpp"

#include "ast/request.hpp"
#include "errors.hpp"
#include "lower/scan_data.hpp"
#include "settings.hpp"
#include "transport/remote.hpp"

#include <algorithm>

namespace qod_qdrant {
using namespace duckdb;

namespace {

constexpr int64_t kPageSize = 256;
constexpr const char *kScanOverBudget = "the scan read more than search_scan_budget_points points";

virtual_column_map_t VirtualColumnsOf(ClientContext &, optional_ptr<FunctionData>) {
	return VirtualColumns();
}

unique_ptr<GlobalTableFunctionState> ScrollInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = ScanDataOf(input.bind_data);
	// Once, up front: EmitRow's own refusal of an unknown column id stays a backstop.
	CheckScanColumns(input.column_ids, data.table.Spec());
	data.CheckAtExecution(context);
	auto state = make_uniq<ScanState>();
	state->column_ids.assign(input.column_ids.begin(), input.column_ids.end());
	state->decode_fields = DecodeFields(data, state->column_ids);
	state->done = data.remote_filter.kind == FilterExpr::Kind::False;
	return std::move(state);
}

void FetchPage(ClientContext &context, const QdrantScanData &data, ScanState &state, bool may_retry) {
	auto &spec = data.table.Spec();
	auto &source = data.table.Source();
	auto limits = ReadLimits(context);
	auto remaining = limits.scan_budget_points - state.read_points;
	if (remaining < 0) {
		// The budget was lowered (SET GLOBAL) below what this scan already read: never a limit below 1.
		Raise(SearchCode::BudgetExceeded, kScanOverBudget);
	}
	ScrollRequest request {data.remote_filter, std::min<int64_t>(kPageSize, remaining + 1), state.offset,
	                       IncludePaths(spec, state.decode_fields)};
	auto body = RemotePost(context, source.Target(), limits, ScrollPath(spec), ScrollRequestJson(request, spec), may_retry);
	ScrollPage page;
	try {
		page = ParseScrollResponse(body);
	} catch (ResponseBudgetError &e) {
		Fail(source.source_id, SearchCode::BudgetExceeded, kResponseOverBudget, e.what(), source.api_key);
	} catch (ResponseError &e) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch, e.what(), source.api_key);
	}
	state.read_points += int64_t(page.points.size());
	if (state.read_points > limits.scan_budget_points) {
		Raise(SearchCode::BudgetExceeded, kScanOverBudget);
	}
	if (page.points.empty() && page.next_offset) {
		Fail(source.source_id, SearchCode::SchemaMismatch, kSchemaMismatch, "empty page with a next offset", source.api_key);
	}
	state.rows.clear();
	state.next = 0;
	for (auto &point : page.points) {
		auto row = CheckedRow(point, data, state.decode_fields);
		row.score.reset();
		state.rows.push_back(std::move(row));
	}
	state.offset = page.next_offset;
	state.done = !state.offset;
}

void ScrollExecute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = ScanDataOf(input.bind_data);
	auto &state = input.global_state->Cast<ScanState>();
	idx_t out = 0;
	while (out < STANDARD_VECTOR_SIZE) {
		EmitBuffered(state, data.table.Spec(), output, out);
		if (out >= STANDARD_VECTOR_SIZE || state.done) {
			break;
		}
		FetchPage(context, data, state, !state.emitted_any && out == 0);
	}
	output.SetChildCardinality(out);
	if (out > 0) {
		state.emitted_any = true;
	}
}

} // namespace

TableFunction ScrollFunction() {
	TableFunction function(Identifier("qod_search_scan"), duckdb::vector<LogicalType>(), ScrollExecute, nullptr,
	                       ScrollInit);
	function.get_table_entry = TableEntryOf;
	function.get_virtual_columns = VirtualColumnsOf;
	function.projection_pushdown = true;
	function.filter_pushdown = false;
	function.filter_prune = false;
	return function;
}

unique_ptr<FunctionData> MakeScrollBindData(QdrantTableEntry &table, ClientContext &context) {
	auto data = make_uniq<QdrantScanData>(table, false);
	data->LowerWithoutPolicies(context);
	return std::move(data);
}

} // namespace qod_qdrant
