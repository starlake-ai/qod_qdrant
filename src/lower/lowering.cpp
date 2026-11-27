#include "lower/lowering.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "errors.hpp"
#include "function_classes.hpp"

namespace qod_qdrant {
using namespace duckdb;

namespace {

constexpr const char *kUnsupportedFilter =
    "filter_sql may use only column = literal, column IN (literals), AND and OR on the relation's columns";

std::optional<size_t> Column(const Expression &e, const CollectionSpec &spec, const ColumnResolver &resolve) {
	if (e.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return std::nullopt;
	}
	auto &ref = e.Cast<BoundColumnRefExpression>();
	if (ref.Depth() != 0) {
		return std::nullopt;
	}
	auto field = resolve(ref);
	if (!field || *field >= spec.fields.size() || e.GetReturnType() != SqlType(spec.fields[*field].type)) {
		return std::nullopt;
	}
	return field;
}

//! The literal side folded by DuckDB itself. A cast the binder left on the literal is part of what DuckDB
//! compares, so its folded value is exact: `owner_id = '1.5'` binds as owner_id = CAST('1.5' AS BIGINT),
//! which DuckDB rounds to 2, and lowers to 2. A DECIMAL literal instead casts the column side and is refused
//! (interpretation 14(10)).
std::optional<FieldValue> FoldLiteral(ClientContext &context, const Expression &e, FieldType type) {
	if (!e.IsFoldable() || e.GetReturnType() != SqlType(type)) {
		return std::nullopt;
	}
	Value value;
	if (!ExpressionExecutor::TryEvaluateScalar(context, e, value) || value.IsNull()) {
		return std::nullopt;
	}
	FieldValue v;
	v.type = type;
	switch (type) {
	case FieldType::Varchar:
		v.text = StringValue::Get(value);
		return v;
	case FieldType::Bigint:
		v.integer = value.GetValue<int64_t>();
		return v;
	case FieldType::Boolean:
		v.boolean = value.GetValue<bool>();
		return v;
	case FieldType::Double:
		break;
	}
	return std::nullopt;
}

//! TRY(CAST(x AS BOOLEAN)) to x; nullptr when the cast is not the identity on BOOLEAN.
const Expression *Unwrap(const Expression &term) {
	const Expression *t = &term;
	if (t->GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    t->GetExpressionType() == ExpressionType::OPERATOR_TRY) {
		auto &children = t->Cast<BoundOperatorExpression>().GetChildren();
		if (children.size() != 1) {
			return nullptr;
		}
		t = children[0].get();
	}
	if (BoundCastExpression::IsCast(*t)) {
		auto &cast = t->Cast<BoundFunctionExpression>();
		if (BoundCastExpression::TargetType(cast) != LogicalType::BOOLEAN ||
		    BoundCastExpression::SourceType(cast) != LogicalType::BOOLEAN) {
			return nullptr;
		}
		t = &BoundCastExpression::Child(cast);
	}
	return t;
}

void Screen(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::SUBQUERY:
	case ExpressionClass::PARAMETER:
	case ExpressionClass::LAMBDA:
	case ExpressionClass::WINDOW:
	case ExpressionClass::STAR:
		Raise(SearchCode::QueryUnsupported, kUnsupportedFilter);
	case ExpressionClass::COLUMN_REF:
		if (expr.Cast<ColumnRefExpression>().ColumnNames().size() != 1) {
			Raise(SearchCode::QueryUnsupported, kUnsupportedFilter);
		}
		break;
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateChildren(expr, [](const ParsedExpression &child) { Screen(child); });
}

} // namespace

LogicalType SqlType(FieldType type) {
	switch (type) {
	case FieldType::Varchar:
		return LogicalType::VARCHAR;
	case FieldType::Bigint:
		return LogicalType::BIGINT;
	case FieldType::Boolean:
		return LogicalType::BOOLEAN;
	case FieldType::Double:
		return LogicalType::DOUBLE;
	}
	return LogicalType::VARCHAR;
}

std::optional<FilterExpr> LowerExpression(ClientContext &context, const Expression &e, const CollectionSpec &spec,
                                          const ColumnResolver &resolve) {
	if (e.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    (e.GetExpressionType() == ExpressionType::CONJUNCTION_AND ||
	     e.GetExpressionType() == ExpressionType::CONJUNCTION_OR)) {
		std::vector<FilterExpr> children;
		for (auto &child : e.Cast<BoundConjunctionExpression>().GetChildren()) {
			auto lowered = LowerExpression(context, *child, spec, resolve);
			if (!lowered) {
				return std::nullopt;
			}
			children.push_back(std::move(*lowered));
		}
		return e.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? FilterExpr::And(std::move(children))
		                                                                : FilterExpr::Or(std::move(children));
	}
	if (e.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && BoundComparisonExpression::IsComparison(e) &&
	    e.GetExpressionType() == ExpressionType::COMPARE_EQUAL) {
		auto &cmp = e.Cast<BoundFunctionExpression>();
		auto *column_side = &BoundComparisonExpression::Left(cmp);
		auto *literal_side = &BoundComparisonExpression::Right(cmp);
		auto field = Column(*column_side, spec, resolve);
		if (!field) {
			std::swap(column_side, literal_side);
			field = Column(*column_side, spec, resolve);
		}
		if (!field) {
			return std::nullopt;
		}
		auto type = spec.fields[*field].type;
		if (type == FieldType::Double) {
			return std::nullopt;
		}
		auto value = FoldLiteral(context, *literal_side, type);
		if (!value) {
			return std::nullopt;
		}
		return FilterExpr::Match(*field, std::move(*value));
	}
	// DuckDB 2.0 binds `x IN ()` as contains(row(), x): an empty IN, FALSE.
	if (e.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION && !BoundComparisonExpression::IsComparison(e) &&
	    !BoundCastExpression::IsCast(e)) {
		auto &call = e.Cast<BoundFunctionExpression>();
		auto name = StringUtil::Lower(Identifier(call.Function().GetName()).GetIdentifierName());
		auto &args = call.GetChildren();
		if (name != "contains" || args.size() != 2 || !args[0]->IsFoldable()) {
			return std::nullopt;
		}
		auto field = Column(*args[1], spec, resolve);
		if (!field || (spec.fields[*field].type != FieldType::Varchar && spec.fields[*field].type != FieldType::Bigint)) {
			return std::nullopt;
		}
		Value container;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *args[0], container) || container.IsNull()) {
			return std::nullopt;
		}
		// `x IN ()` binds its row() as an empty TUPLE constant (an unnamed STRUCT).
		auto id = container.type().id();
		bool empty = (StructType::IsStruct(id) && StructValue::GetChildren(container).empty()) ||
		             (id == LogicalTypeId::LIST && ListValue::GetChildren(container).empty());
		if (!empty) {
			return std::nullopt;
		}
		return FilterExpr::In(*field, {});
	}
	if (e.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR && e.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &children = e.Cast<BoundOperatorExpression>().GetChildren();
		if (children.size() < 2) {
			return std::nullopt;
		}
		auto field = Column(*children[0], spec, resolve);
		if (!field) {
			return std::nullopt;
		}
		auto type = spec.fields[*field].type;
		if (type != FieldType::Varchar && type != FieldType::Bigint) {
			return std::nullopt;
		}
		std::vector<FieldValue> values;
		for (size_t i = 1; i < children.size(); ++i) {
			auto value = FoldLiteral(context, *children[i], type);
			if (!value) {
				return std::nullopt;
			}
			values.push_back(std::move(*value));
		}
		return FilterExpr::In(*field, std::move(values));
	}
	return std::nullopt;
}

std::optional<FilterExpr> LowerPolicyPredicate(ClientContext &context, const Expression &predicate,
                                               const CollectionSpec &spec, const ColumnResolver &resolve) {
	std::vector<const Expression *> terms;
	if (predicate.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    predicate.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		for (auto &child : predicate.Cast<BoundConjunctionExpression>().GetChildren()) {
			terms.push_back(child.get());
		}
	} else {
		terms.push_back(&predicate);
	}
	std::vector<FilterExpr> lowered;
	for (auto term : terms) {
		auto inner = Unwrap(*term);
		if (!inner) {
			return std::nullopt;
		}
		auto one = LowerExpression(context, *inner, spec, resolve);
		if (!one) {
			return std::nullopt;
		}
		lowered.push_back(std::move(*one));
	}
	return FilterExpr::Or(std::move(lowered));
}

std::optional<FilterExpr> LowerPolicyPredicates(ClientContext &context, const LogicalGet &get,
                                                const std::vector<unique_ptr<Expression>> &predicates,
                                                const CollectionSpec &spec) {
	auto table_index = get.table_index;
	auto &column_ids = get.GetColumnIds();
	ColumnResolver resolve = [&](const BoundColumnRefExpression &ref) -> std::optional<size_t> {
		if (ref.Binding().table_index != table_index) {
			return std::nullopt;
		}
		auto slot = ref.Binding().column_index.GetIndex();
		if (slot >= column_ids.size() || column_ids[slot].IsVirtualColumn()) {
			return std::nullopt;
		}
		return size_t(column_ids[slot].GetPrimaryIndex());
	};
	std::vector<FilterExpr> all;
	for (auto &predicate : predicates) {
		auto one = LowerPolicyPredicate(context, *predicate, spec, resolve);
		if (!one) {
			return std::nullopt;
		}
		all.push_back(std::move(*one));
	}
	return FilterExpr::And(std::move(all));
}

BoundColumns BindColumns(ClientContext &context, unique_ptr<ParsedExpression> parsed, const CollectionSpec &spec) {
	Screen(*parsed);
	vector<Identifier> names;
	vector<LogicalType> types;
	for (auto &f : spec.fields) {
		names.emplace_back(f.name);
		types.push_back(SqlType(f.type));
	}
	BoundColumns result;
	bool failed = false;
	try {
		auto binder = Binder::CreateBinder(context);
		auto table = binder->GenerateTableIndex();
		binder->bind_context.AddGenericBinding(table, Identifier(spec.table), names, types);
		binder->SetCatalogLookupCallback([](CatalogEntry &entry) {
			if (entry.type == CatalogType::TYPE_ENTRY && entry.Cast<TypeCatalogEntry>().internal) {
				return;
			}
			auto name = StringUtil::Lower(entry.name.GetIdentifierName());
			if (entry.ParentCatalog().GetName() != Identifier("system")) {
				throw PermissionException("not allowed");
			}
			if (entry.type == CatalogType::SCALAR_FUNCTION_ENTRY || entry.type == CatalogType::MACRO_ENTRY) {
				auto cls = qod::FunctionClassOf(name, entry.type == CatalogType::MACRO_ENTRY ? "macro" : "scalar");
				if (!cls || *cls != qod::FunctionClass::Allowed || name == "getvariable" || name == "current_setting") {
					throw PermissionException("not allowed");
				}
			} else if (entry.type != CatalogType::TYPE_ENTRY && entry.type != CatalogType::COLLATION_ENTRY) {
				throw PermissionException("not allowed");
			}
		});
		WhereBinder where_binder(*binder, context);
		result.expr = where_binder.Bind(parsed);
		result.resolver = [table](const BoundColumnRefExpression &ref) -> std::optional<size_t> {
			if (ref.Binding().table_index != table) {
				return std::nullopt;
			}
			return size_t(ref.Binding().column_index.GetIndex());
		};
	} catch (InterruptException &) {
		// A cancelled statement stays cancelled, never a refusal of its filter_sql.
		throw;
	} catch (std::exception &) {
		failed = true;
	}
	if (failed || !result.expr) {
		Raise(SearchCode::QueryUnsupported, "filter_sql does not bind to the relation's columns");
	}
	return result;
}

FilterExpr BindFilterSql(ClientContext &context, const std::string &sql, const CollectionSpec &spec) {
	if (sql.find_first_not_of(" \t\r\n") == std::string::npos) {
		return FilterExpr::True();
	}
	unique_ptr<ParsedExpression> parsed;
	try {
		auto parser = Parser::GetBuiltinParser();
		parsed = parser.ParseSingleExpression(sql);
	} catch (InterruptException &) {
		throw;
	} catch (std::exception &) {
		parsed = nullptr;
	}
	if (!parsed) {
		Raise(SearchCode::QueryUnsupported, "filter_sql must be one SQL expression");
	}
	auto bound = BindColumns(context, std::move(parsed), spec);
	auto lowered = LowerExpression(context, *bound.expr, spec, bound.resolver);
	if (!lowered) {
		Raise(SearchCode::QueryUnsupported, kUnsupportedFilter);
	}
	return Simplify(*lowered);
}

} // namespace qod_qdrant
