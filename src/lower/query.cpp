#include "lower/query.hpp"

#include "errors.hpp"

#include <cmath>

namespace qod_qdrant {
using namespace duckdb;

const char *const kQueryShape = "query must be qod_dense, qod_bm25 or qod_rrf";

namespace {

//! A query value of kind `kind`: the given fields, every other one NULL.
Value MakeQuery(const std::string &kind, const Value &vector_name, const Value &vector, const Value &text,
                const Value &prefetch_k, const Value &branches) {
	child_list_t<Value> children;
	children.emplace_back(Identifier("kind"), Value(kind));
	children.emplace_back(Identifier("vector_name"), vector_name);
	children.emplace_back(Identifier("vector"), vector);
	children.emplace_back(Identifier("text"), text);
	children.emplace_back(Identifier("prefetch_k"), prefetch_k);
	children.emplace_back(Identifier("branches"), branches);
	return Value::STRUCT(std::move(children));
}

Value NullVector() {
	return Value(LogicalType::LIST(LogicalType::FLOAT));
}

Value NullBranches() {
	return Value(LogicalType::LIST(QueryBranchType()));
}

void DenseExecute(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t i = 0; i < args.size(); ++i) {
		auto name = args.GetValue(0, i);
		auto vector = args.GetValue(1, i);
		if (name.IsNull() || vector.IsNull()) {
			result.SetValue(i, Value(QueryType()));
			continue;
		}
		result.SetValue(i, MakeQuery("dense", name, vector, Value(LogicalType::VARCHAR), Value(LogicalType::BIGINT),
		                             NullBranches()));
	}
}

void Bm25Execute(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t i = 0; i < args.size(); ++i) {
		auto name = args.GetValue(0, i);
		auto text = args.GetValue(1, i);
		if (name.IsNull() || text.IsNull()) {
			result.SetValue(i, Value(QueryType()));
			continue;
		}
		result.SetValue(i, MakeQuery("bm25", name, NullVector(), text, Value(LogicalType::BIGINT), NullBranches()));
	}
}

void RrfExecute(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t i = 0; i < args.size(); ++i) {
		auto list = args.GetValue(0, i);
		auto prefetch_k = args.GetValue(1, i);
		if (list.IsNull()) {
			result.SetValue(i, Value(QueryType()));
			continue;
		}
		vector<Value> branches;
		for (auto &element : ListValue::GetChildren(list)) {
			if (element.IsNull()) {
				branches.push_back(Value(QueryBranchType()));
				continue;
			}
			// The first four fields: a nested qod_rrf keeps its kind, so qod_search refuses it by name.
			auto &fields = StructValue::GetChildren(element);
			child_list_t<Value> branch;
			branch.emplace_back(Identifier("kind"), fields[0]);
			branch.emplace_back(Identifier("vector_name"), fields[1]);
			branch.emplace_back(Identifier("vector"), fields[2]);
			branch.emplace_back(Identifier("text"), fields[3]);
			branches.push_back(Value::STRUCT(std::move(branch)));
		}
		result.SetValue(i, MakeQuery("rrf", Value(LogicalType::VARCHAR), NullVector(), Value(LogicalType::VARCHAR),
		                             prefetch_k, Value::LIST(QueryBranchType(), std::move(branches))));
	}
}

void RequireConstantName(BindScalarFunctionInput &input, const char *function) {
	auto &arguments = input.GetArguments();
	if (arguments.empty() || !arguments[0]->IsFoldable()) {
		Raise(SearchCode::QueryUnsupported, std::string(function) + " needs a constant vector name");
	}
}

unique_ptr<FunctionData> DenseBind(BindScalarFunctionInput &input) {
	RequireConstantName(input, "qod_dense");
	return nullptr;
}

unique_ptr<FunctionData> Bm25Bind(BindScalarFunctionInput &input) {
	RequireConstantName(input, "qod_bm25");
	return nullptr;
}

} // namespace

LogicalType QueryBranchType() {
	child_list_t<LogicalType> children;
	children.emplace_back(Identifier("kind"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("vector_name"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("vector"), LogicalType::LIST(LogicalType::FLOAT));
	children.emplace_back(Identifier("text"), LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(children));
}

LogicalType QueryType() {
	child_list_t<LogicalType> children;
	children.emplace_back(Identifier("kind"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("vector_name"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("vector"), LogicalType::LIST(LogicalType::FLOAT));
	children.emplace_back(Identifier("text"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("prefetch_k"), LogicalType::BIGINT);
	children.emplace_back(Identifier("branches"), LogicalType::LIST(QueryBranchType()));
	return LogicalType::STRUCT(std::move(children));
}

std::vector<float> CheckedVector(const Value &list, const VectorSpec &spec) {
	if (list.IsNull() || spec.datatype != "float32") {
		Raise(SearchCode::QueryUnsupported, kQueryShape);
	}
	auto &items = ListValue::GetChildren(list);
	auto wrong = "the query vector must hold " + std::to_string(spec.dimensions) + " finite FLOAT values";
	if (int64_t(items.size()) != spec.dimensions) {
		Raise(SearchCode::QueryUnsupported, wrong);
	}
	std::vector<float> out;
	out.reserve(items.size());
	for (auto &item : items) {
		if (item.IsNull()) {
			Raise(SearchCode::QueryUnsupported, wrong);
		}
		auto f = item.GetValue<float>();
		if (!std::isfinite(f)) {
			Raise(SearchCode::QueryUnsupported, wrong);
		}
		out.push_back(f);
	}
	return out;
}

ScalarFunction DenseFunction() {
	return ScalarFunction(Identifier("qod_dense"),
	                      duckdb::vector<LogicalType> {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::FLOAT)},
	                      QueryType(), DenseExecute, DenseBind);
}

ScalarFunction Bm25Function() {
	return ScalarFunction(Identifier("qod_bm25"), duckdb::vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                      QueryType(), Bm25Execute, Bm25Bind);
}

ScalarFunction RrfFunction() {
	FunctionSignature signature;
	signature.AddParameter(LogicalType::LIST(QueryType()));
	signature.AddKeywordOnly(Identifier("prefetch_k"), LogicalType::BIGINT, Value(LogicalType::BIGINT));
	signature.SetReturnType(QueryType());
	ScalarFunction function(Identifier("qod_rrf"), std::move(signature), RrfExecute);
	// DuckDB folds a call with a constant NULL argument to NULL under the default null handling, and the
	// omitted prefetch_k is exactly that: the function handles NULLs itself.
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

} // namespace qod_qdrant
