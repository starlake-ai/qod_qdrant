// The query constructors through the real node hooks, on a tenant connection: qod_dense, qod_bm25 and
// qod_rrf build one query type, unused fields NULL; prefetch_k is keyword-only with a NULL default; vector
// names are constants; nothing is sent; all three are classed scalar allowed.
#include "check.hpp"
#include "engine.hpp"
#include "fake_transport.hpp"
#include "linked_function_classes.hpp"
#include "lower/query.hpp"

using namespace qod_qdrant_test;

namespace {
const std::string kType = "STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], \"text\" VARCHAR, prefetch_k BIGINT, "
                          "branches STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], \"text\" VARCHAR)[])";
const std::string kTwo = "[qod_bm25('b', 'x'), qod_dense('v', [1.5]::FLOAT[])]";
const std::string kTwoBranches = "[{'kind': bm25, 'vector_name': b, 'vector': NULL, 'text': x}, "
                                 "{'kind': dense, 'vector_name': v, 'vector': [1.5], 'text': NULL}]";

std::string Rrf(const std::string &prefetch_k, const std::string &branches = kTwoBranches) {
	return "{'kind': rrf, 'vector_name': NULL, 'vector': NULL, 'text': NULL, 'prefetch_k': " + prefetch_k +
	       ", 'branches': " + branches + "}";
}
} // namespace

int main() {
	InstallFakeTransport();
	Engine e;
	Expect("install", e.Install(), "true");
	Backend().Reset();
	auto &user = *e.user;

	Expect("QueryType()", qod_qdrant::QueryType().ToString(), kType);
	Expect("qod_dense type", RunSql(user, "SELECT typeof(qod_dense('v', [1.5]::FLOAT[]))"), kType);
	Expect("qod_bm25 type", RunSql(user, "SELECT typeof(qod_bm25('body_bm25', 'merger'))"), kType);
	Expect("qod_rrf type", RunSql(user, "SELECT typeof(qod_rrf(" + kTwo + "))"), kType);
	Expect("qod_bm25 value", RunSql(user, "SELECT qod_bm25('body_bm25', 'merger agreement')"),
	       "{'kind': bm25, 'vector_name': body_bm25, 'vector': NULL, 'text': merger agreement, 'prefetch_k': NULL, "
	       "'branches': NULL}");
	Expect("qod_bm25 keeps the text as given (qod_search trims)", RunSql(user, "SELECT qod_bm25('b', '  x ').text"), "  x ");
	Expect("qod_rrf value, prefetch_k omitted", RunSql(user, "SELECT qod_rrf(" + kTwo + ")"), Rrf("NULL"));
	Expect("prefetch_k :=", RunSql(user, "SELECT qod_rrf(" + kTwo + ", prefetch_k := 50)"), Rrf("50"));
	Expect("prefetch_k =>", RunSql(user, "SELECT qod_rrf(" + kTwo + ", prefetch_k => 50)"), Rrf("50"));
	Expect("PREFETCH_K, as DuckDB matches names", RunSql(user, "SELECT qod_rrf(" + kTwo + ", PREFETCH_K := 50)"), Rrf("50"));
	Expect("prefetch_k := NULL", RunSql(user, "SELECT qod_rrf(" + kTwo + ", prefetch_k := NULL)"), Rrf("NULL"));
	Contains("prefetch_k is keyword-only", RunSql(user, "SELECT qod_rrf(" + kTwo + ", 50)"), "Binder Error: No function matches");
	Contains("no other keyword", RunSql(user, "SELECT qod_rrf(" + kTwo + ", k := 50)"), "Binder Error: No function matches");
	Expect("a nested qod_rrf keeps its kind as a branch",
	       RunSql(user, "SELECT qod_rrf([qod_rrf(" + kTwo + "), qod_dense('v', [1.5]::FLOAT[])]).branches"),
	       "[{'kind': rrf, 'vector_name': NULL, 'vector': NULL, 'text': NULL}, "
	       "{'kind': dense, 'vector_name': v, 'vector': [1.5], 'text': NULL}]");
	Expect("a NULL element is a NULL branch", RunSql(user, "SELECT qod_rrf([NULL, qod_bm25('b', 'x')]).branches"),
	       "[NULL, {'kind': bm25, 'vector_name': b, 'vector': NULL, 'text': x}]");
	Expect("no branches", RunSql(user, "SELECT qod_rrf([]).branches"), "[]");
	Expect("NULL branches list", RunSql(user, "SELECT qod_rrf(NULL)"), "NULL");
	Expect("NULL text", RunSql(user, "SELECT qod_bm25('b', NULL)"), "NULL");
	Expect("NULL vector name", RunSql(user, "SELECT qod_bm25(NULL, 'x')"), "NULL");
	Contains("qod_bm25 needs a constant vector name", RunSql(user, "SELECT qod_bm25(s, 'x') FROM (VALUES ('b')) t(s)"),
	         "search_query_unsupported: qod_bm25 needs a constant vector name");
	Contains("qod_dense needs a constant vector name",
	         RunSql(user, "SELECT qod_dense(s, [1.5]::FLOAT[]) FROM (VALUES ('v')) t(s)"),
	         "search_query_unsupported: qod_dense needs a constant vector name");
	Expect("prepare the text as a parameter", RunSql(user, "PREPARE t AS SELECT qod_bm25('body_bm25', ?).text"), "");
	Expect("execute binds the text", RunSql(user, "EXECUTE t('merger')"), "merger");
	Expect("prepare prefetch_k as a parameter",
	       RunSql(user, "PREPARE p AS SELECT qod_rrf(" + kTwo + ", prefetch_k := ?).prefetch_k"), "");
	Expect("execute binds prefetch_k", RunSql(user, "EXECUTE p(7)"), "7");
	ExpectTrue("qod_bm25 classed allowed (scalar)",
	           qod::LinkedFunctionClassOf("qod_bm25", "scalar") == qod::FunctionClass::Allowed);
	ExpectTrue("qod_rrf classed allowed (scalar)", qod::LinkedFunctionClassOf("qod_rrf", "scalar") == qod::FunctionClass::Allowed);
	ExpectTrue("qod_dense still classed allowed (scalar)",
	           qod::LinkedFunctionClassOf("qod_dense", "scalar") == qod::FunctionClass::Allowed);
	Expect("building query values sends nothing", std::to_string(Backend().CallCount()), "0");

	return Finish();
}
