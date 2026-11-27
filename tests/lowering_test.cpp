// Exact lowering (spec 5.3): each supported form to its Qdrant filter JSON, every other form refused
// without echoing a literal, BIGINT without precision loss, policy predicates as core binds them.
#include "catalog/config.hpp"
#include "check.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "lower/filter_expr.hpp"
#include "lower/lowering.hpp"

using namespace qod_qdrant;
using namespace qod_qdrant_test;
using namespace duckdb;

namespace {
CollectionSpec spec;

std::string Lowered(Connection &con, const std::string &sql) {
	std::string out;
	con.context->RunFunctionInTransaction([&] {
		auto expr = BindFilterSql(*con.context, sql, spec);
		out = expr.kind == FilterExpr::Kind::True    ? "TRUE"
		      : expr.kind == FilterExpr::Kind::False ? "FALSE"
		                                             : WriteJson(FilterJson(expr, spec));
	});
	return out;
}

std::string Refusal(Connection &con, const std::string &sql) {
	return ErrorOf([&] { Lowered(con, sql); });
}

unique_ptr<ParsedExpression> Policy(const std::string &sql) {
	auto parser = Parser::GetBuiltinParser();
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_TRY,
	                                     make_uniq<CastExpression>(LogicalType::BOOLEAN, parser.ParseSingleExpression(sql)));
}

std::string PolicyLowered(Connection &con, unique_ptr<ParsedExpression> parsed) {
	std::string out;
	con.context->RunFunctionInTransaction([&] {
		auto bound = BindColumns(*con.context, std::move(parsed), spec);
		auto lowered = LowerPolicyPredicate(*con.context, *bound.expr, spec, bound.resolver);
		if (!lowered) {
			out = "not lowered";
		} else {
			auto simple = Simplify(*lowered);
			out = simple.kind == FilterExpr::Kind::False ? "FALSE" : WriteJson(FilterJson(simple, spec));
		}
	});
	return out;
}

FieldValue Text(const std::string &s) {
	FieldValue v;
	v.type = FieldType::Varchar;
	v.text = s;
	return v;
}
FieldValue Int(int64_t n) {
	FieldValue v;
	v.type = FieldType::Bigint;
	v.integer = n;
	return v;
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	auto rich = Patch(ReadFile(fixtures + "sample.json"),
	                  "{ \"name\": \"title\", \"path\": \"title\", \"type\": \"VARCHAR\", \"required\": false }",
	                  "{ \"name\": \"title\", \"path\": \"meta.title\", \"type\": \"DOUBLE\", \"required\": false }, "
	                  "{ \"name\": \"published\", \"path\": \"meta.published\", \"type\": \"BOOLEAN\", \"required\": false }");
	auto parsed = ParseConfig(rich, {"https://qdrant.internal:6333"});
	if (!parsed.config) {
		std::cerr << "fixture refused: " << parsed.errors.at(0) << std::endl;
		return 1;
	}
	// tenant_id 0, document_id 1, title 2 (DOUBLE), published 3, status 4, owner_id 5, body 6
	spec = parsed.config->collections.at(0);
	DuckDB db(nullptr);
	Connection con(db);

	Expect("VARCHAR =", Lowered(con, "status = 'active'"), "{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]}");
	Expect("literal first", Lowered(con, "'active' = status"), "{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]}");
	Expect("quote in literal", Lowered(con, "status = 'it''s'"), "{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"it's\"}}]}");
	Expect("BIGINT =", Lowered(con, "owner_id = 42"), "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":42}}]}");
	Expect("BIGINT max, no precision loss", Lowered(con, "owner_id = 9223372036854775807"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":9223372036854775807}}]}");
	Expect("BOOLEAN = on a nested path", Lowered(con, "published = true"),
	       "{\"must\":[{\"key\":\"meta.published\",\"match\":{\"value\":true}}]}");
	Expect("VARCHAR IN", Lowered(con, "status IN ('a', 'b')"), "{\"must\":[{\"key\":\"status\",\"match\":{\"any\":[\"a\",\"b\"]}}]}");
	Expect("BIGINT IN", Lowered(con, "owner_id IN (-1, 9223372036854775807)"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"any\":[-1,9223372036854775807]}}]}");
	Expect("OR inside AND", Lowered(con, "(status = 'a' OR status = 'b') AND owner_id = 1"),
	       "{\"must\":[{\"should\":[{\"key\":\"status\",\"match\":{\"value\":\"a\"}},{\"key\":\"status\",\"match\":{\"value\":\"b\"}}]},"
	       "{\"key\":\"owner_id\",\"match\":{\"value\":1}}]}");
	Expect("AND inside OR", Lowered(con, "status = 'a' OR (owner_id = 1 AND published = false)"),
	       "{\"should\":[{\"key\":\"status\",\"match\":{\"value\":\"a\"}},{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":1}},"
	       "{\"key\":\"meta.published\",\"match\":{\"value\":false}}]}]}");
	Expect("blank filter", Lowered(con, "  "), "TRUE");
	Expect("empty IN on VARCHAR (contains(row(), x))", Lowered(con, "status IN ()"), "FALSE");
	Expect("empty IN on BIGINT", Lowered(con, "owner_id IN ()"), "FALSE");
	Expect("empty IN inside OR", Lowered(con, "status IN () OR owner_id = 1"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":1}}]}");
	Expect("INTEGER literal -1 cast to BIGINT", Lowered(con, "owner_id = -1"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":-1}}]}");
	Expect("int64 min (a BIGINT literal)", Lowered(con, "owner_id = -9223372036854775808"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":-9223372036854775808}}]}");
	Expect("beyond 2^53 stays exact", Lowered(con, "owner_id = 9007199254740993"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":9007199254740993}}]}");

	for (auto sql : {"status != 'zzsecret'", "NOT (status = 'zzsecret')", "status IS NULL", "owner_id > 1",
	                 "status LIKE 'zzsecret%'", "CAST(owner_id AS VARCHAR) = 'zzsecret'", "owner_id = 1.5", "status = NULL",
	                 "status IN ('zzsecret', NULL)", "lower(status) = 'zzsecret'", "status = (SELECT 'zzsecret')", "status = ?",
	                 "body", "true", "title = 1.5", "published IN (true)", "contracts.status = 'zzsecret'",
	                 "nope = 'zzsecret'", "status = 'zzsecret'; SELECT 1", "status = 'a', owner_id = 1",
	                 "status = 'zzsecret' COLLATE nocase", "status NOT IN ('zzsecret')"}) {
		Contains(std::string("refused: ") + sql, Refusal(con, sql), "search_query_unsupported: ", "zzsecret");
	}

	Expect("policies as core binds them",
	       PolicyLowered(con, make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, Policy("status = 'a'"),
	                                                           Policy("owner_id IN (1, 2)"))),
	       "{\"should\":[{\"key\":\"status\",\"match\":{\"value\":\"a\"}},{\"key\":\"owner_id\",\"match\":{\"any\":[1,2]}}]}");
	Expect("one policy", PolicyLowered(con, Policy("tenant_id = 'acme' AND owner_id = 7")),
	       "{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}},{\"key\":\"owner_id\",\"match\":{\"value\":7}}]}");
	Expect("a non-boolean cast is not lowered", PolicyLowered(con, Policy("status")), "not lowered");
	Expect("a range is not lowered", PolicyLowered(con, Policy("owner_id > 3")), "not lowered");
	Expect("an empty-IN policy grants nothing", PolicyLowered(con, Policy("owner_id IN ()")), "FALSE");
	Contains("a non-empty contains() is not an IN", Refusal(con, "contains(['zzsecret'], status)"),
	         "search_query_unsupported: ", "zzsecret");
	// A VARCHAR literal on a BIGINT column folds through DuckDB's own VARCHAR to BIGINT cast ('1.5' rounds to
	// 2): the lowered value is exactly what DuckDB compares (interpretation 14(10) refuses DECIMAL literals,
	// which cast the column side instead).
	Expect("VARCHAR literal cast to BIGINT", Lowered(con, "owner_id = '1'"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":1}}]}");
	Expect("VARCHAR literal rounded by DuckDB's cast", Lowered(con, "owner_id = '1.5'"),
	       "{\"must\":[{\"key\":\"owner_id\",\"match\":{\"value\":2}}]}");
	Expect("contains() on an empty list", Lowered(con, "contains([], status)"), "FALSE");
	Contains("string contains() is not an IN", Refusal(con, "contains('', status)"), "search_query_unsupported: ");
	con.Query("SET default_collation = 'nocase'");
	Contains("a default collation wraps the column", Refusal(con, "status = 'zzsecret'"), "search_query_unsupported: ",
	         "zzsecret");
	con.Query("RESET default_collation");
	Expect("collation reset", Lowered(con, "status = 'a'"), "{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"a\"}}]}");

	auto f = FilterExpr::And({FilterExpr::Match(4, Text("a")), FilterExpr::In(5, {})});
	Expect("empty IN is FALSE", Simplify(f).kind == FilterExpr::Kind::False ? "false" : "other", "false");
	auto g = Simplify(FilterExpr::Or({FilterExpr::False(), FilterExpr::Match(4, Text("a"))}));
	Expect("OR drops FALSE", g.kind == FilterExpr::Kind::Match ? "match" : "other", "match");
	Expect("OR with TRUE", Simplify(FilterExpr::Or({FilterExpr::Match(4, Text("a")), FilterExpr::True()})).kind ==
	                               FilterExpr::Kind::True
	                           ? "true"
	                           : "other",
	       "true");
	auto h = Simplify(FilterExpr::And({FilterExpr::True(), FilterExpr::And({FilterExpr::Match(4, Text("a")),
	                                                                        FilterExpr::Match(5, Int(1))})}));
	Expect("AND flattens", std::to_string(h.children.size()) + " nodes " + std::to_string(NodeCount(h)), "2 nodes 3");
	std::set<size_t> fields;
	CollectFields(h, fields);
	Expect("fields used", std::to_string(fields.size()) + ":" + std::to_string(*fields.begin()), "2:4");
	// tenant_id is an ordinary field: a tenant row policy lowers like any other match.
	auto tenant_policy = FilterExpr::Match(0, Text("acme"));
	Expect("tenant_id match", WriteJson(FilterJson(tenant_policy, spec)),
	       "{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}}]}");

	std::vector<std::optional<FieldValue>> row(spec.fields.size());
	ExpectTrue("missing value never matches", !Evaluate(tenant_policy, row));
	row[0] = Text("acme");
	ExpectTrue("matching value", Evaluate(tenant_policy, row));
	row[0] = Text("bob");
	ExpectTrue("other value", !Evaluate(tenant_policy, row));
	row[5] = Int(INT64_MAX);
	ExpectTrue("BIGINT IN evaluates exactly", Evaluate(FilterExpr::In(5, {Int(INT64_MAX - 1), Int(INT64_MAX)}), row));
	ExpectTrue("BIGINT neighbour", !Evaluate(FilterExpr::Match(5, Int(INT64_MAX - 1)), row));
	ExpectTrue("empty IN evaluates false", !Evaluate(FilterExpr::In(5, {}), row));
	row[4] = Text("open");
	// row: tenant_id 'bob', status 'open', owner_id INT64_MAX.
	auto nested = [](const std::string &status) {
		return FilterExpr::Or({FilterExpr::And({FilterExpr::Match(0, Text("acme")), FilterExpr::Match(4, Text("open"))}),
		                       FilterExpr::And({FilterExpr::Match(4, Text(status)), FilterExpr::In(5, {Int(INT64_MAX)})})});
	};
	ExpectTrue("nested OR of ANDs, second branch true", Evaluate(nested("open"), row));
	ExpectTrue("nested OR of ANDs, both branches false", !Evaluate(nested("closed"), row));

	auto json_error = [](const FilterExpr &expr) {
		try {
			FilterJson(expr, spec);
		} catch (std::logic_error &) {
			return std::string("logic_error");
		}
		return std::string("no error");
	};
	Expect("FilterJson refuses an empty IN", json_error(FilterExpr::In(5, {})), "logic_error");
	Expect("FilterJson refuses an empty AND", json_error(FilterExpr::And({})), "logic_error");
	Expect("FilterJson refuses an empty OR", json_error(FilterExpr::Or({})), "logic_error");
	Expect("FilterJson refuses a nested empty IN",
	       json_error(FilterExpr::And({FilterExpr::Match(4, Text("a")), FilterExpr::In(5, {})})), "logic_error");
	Expect("FilterJson refuses a TRUE root", json_error(FilterExpr::True()), "logic_error");
	return Finish();
}
