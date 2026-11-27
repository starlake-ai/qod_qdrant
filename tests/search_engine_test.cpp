// qod_search through the real node hooks: bind without I/O, exact lowering of P AND U before the remote
// top-k (a tenant_id row policy included), refusal of an inexact policy before any request, field uses, k
// and vector validation, local recheck of every hit, outer WHERE applied after k, prepared statements.
#include "catalog/catalog.hpp"
#include "check.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table_function.hpp"
#include "engine.hpp"
#include "fake_transport.hpp"
#include "linked_function_classes.hpp"
#include "lower/scan_data.hpp"
#include "lower/search.hpp"
#include "protected_scan.hpp"

using namespace qod_qdrant_test;

namespace {
std::string Vector(size_t n, const std::string &value = "0.5") {
	std::string out = "[";
	for (size_t i = 0; i < n; ++i) {
		out += (i ? "," : "") + value;
	}
	return out + "]::FLOAT[]";
}

std::string Search(const std::string &args, const std::string &select = "document_id, _qod_score",
                   const std::string &tail = "ORDER BY _qod_score DESC") {
	return "SELECT " + select + " FROM qod_search('search_docs.main.contracts', " + args + ") " + tail;
}

//! A hand-spelled query value of the slice 2 shape: kind, vector name and vector given, the other fields NULL.
std::string Spelled(const std::string &kind, const std::string &name, const std::string &vector) {
	return "{'kind': " + kind + ", 'vector_name': " + name + ", 'vector': " + vector +
	       ", 'text': NULL, 'prefetch_k': NULL, 'branches': NULL}";
}

std::string Hits(const std::string &points) {
	return "{\"result\":{\"points\":[" + points + "]},\"status\":\"ok\",\"time\":0.001}";
}

const std::string kHit1 = "{\"id\":1,\"version\":1,\"score\":1.0,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d1\","
                          "\"status\":\"active\",\"owner_id\":7}}";
const std::string kHit2 = "{\"id\":2,\"version\":1,\"score\":0.5,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d2\","
                          "\"status\":\"active\",\"owner_id\":8}}";

std::string Body(size_t i) {
	std::lock_guard<std::mutex> lock(Backend().mutex);
	return i < Backend().calls.size() ? Backend().calls[i].body : "no call";
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	InstallFakeTransport();
	Engine e;
	auto config = ReadFile(fixtures + "sample_canonical.json");
	Expect("attach", e.Attach("search_docs", config), "");
	ExpectTrue("qod_search classed allowed (table)",
	           qod::LinkedFunctionClassOf("qod_search", "table") == qod::FunctionClass::Allowed);
	ExpectTrue("qod_dense classed allowed (scalar)",
	           qod::LinkedFunctionClassOf("qod_dense", "scalar") == qod::FunctionClass::Allowed);
	ExpectTrue("qod_search registered bounded", qod::IsBoundedFunction("qod_search"));
	Expect("install", e.Install(), "true");

	auto dense = "query => qod_dense('text_dense', " + Vector(768) + ")";
	{
		// Bind, post_bind and pre_optimize do no remote I/O: a prepared or explained search sends nothing.
		Backend().Reset();
		auto prepared = e.user->Prepare(Search(dense + ", k => 2"));
		ExpectTrue("prepare a search", !prepared->HasError());
		Contains("explain a search", RunSql(*e.user, "EXPLAIN " + Search(dense + ", k => 2")), "QOD_SEARCH");
		Expect("no request before execution", std::to_string(Backend().CallCount()), "0");
	}
	Expect("qod_dense type", RunSql(*e.user, "SELECT typeof(qod_dense('v', [1.5]::FLOAT[]))"),
	       "STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], \"text\" VARCHAR, prefetch_k BIGINT, "
	       "branches STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], \"text\" VARCHAR)[])");
	Expect("qod_dense value", RunSql(*e.user, "SELECT qod_dense('v', [1.5]::FLOAT[])"),
	       "{'kind': dense, 'vector_name': v, 'vector': [1.5], 'text': NULL, 'prefetch_k': NULL, 'branches': NULL}");
	Contains("qod_dense needs a constant vector name",
	         RunSql(*e.user, "SELECT qod_dense(s, [1.5]::FLOAT[]) FROM (VALUES ('v')) t(s)"), "search_query_unsupported: ");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Expect("search rows", RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''active''', k => 2")), "d1|1.0,d2|0.5");
	Contains("U lowered alone without a row policy, k as the limit, projected and filter fields only", Body(0),
	         "\"using\":\"text_dense\",\"filter\":{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]},"
	         "\"limit\":2,\"with_payload\":{\"include\":[\"document_id\",\"status\"]},\"with_vector\":false}");
	Contains("vector serialized", Body(0), "{\"query\":[0.5,0.5,");
	Contains("vector lineage reported as a read", LastDescription(), "\"body\"");

	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("default k", RunSql(*e.user, Search(dense, "document_id", "")), "d1");
	Contains("default k is search_default_k", Body(0), "\"limit\":10,");
	Contains("no row policy and no filter_sql: no filter at all", Body(0), "\"using\":\"text_dense\",\"limit\":10,",
	         "\"filter\"");
	// A search_max_k below search_default_k caps the default too: the call that gives no k still runs.
	Expect("raise the default k", RunSql(*e.setup, "SET GLOBAL search_default_k = 50"), "");
	Expect("lower the max k", RunSql(*e.setup, "SET GLOBAL search_max_k = 20"), "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("default k above max k", RunSql(*e.user, Search(dense, "document_id", "")), "d1");
	Contains("default k capped by search_max_k", Body(0), "\"limit\":20,");
	Contains("an explicit k above search_max_k still refuses", RunSql(*e.user, Search(dense + ", k => 21")),
	         "search_query_unsupported: k must be in 1..20");
	Expect("reset the default k", RunSql(*e.setup, "RESET search_default_k"), "");
	Expect("reset the max k", RunSql(*e.setup, "RESET search_max_k"), "");

	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Expect("outer WHERE filters after k", RunSql(*e.user, Search(dense + ", k => 2", "document_id", "WHERE owner_id = 8")), "d2");
	Contains("outer WHERE never pushed below k", Body(0), "\"limit\":2,", "owner_id\",\"match");

	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Expect("virtual columns: _qod_id and the point's score",
	       RunSql(*e.user, Search(dense + ", k => 2", "_qod_id, document_id, _qod_score", "ORDER BY 1")), "1|d1|1.0,2|d2|0.5");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("parameter names match as DuckDB matches them",
	       RunSql(*e.user, Search(dense + ", FILTER_SQL => 'status = ''active''', K => 3", "document_id", "")), "d1");
	Contains("an upper-case filter_sql is still lowered", Body(0),
	         "{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]},\"limit\":3,");
	auto literal = Spelled("'dense'", "'text_dense'", Vector(768));
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("a hand-built query struct goes through the same checks", RunSql(*e.user, Search("query => " + literal, "document_id", "")),
	       "d1");
	Contains("and builds the same request", Body(0), "\"using\":\"text_dense\"");
	Backend().Reset();
	Contains("a hand-spelled struct of the slice 1 shape no longer binds",
	         RunSql(*e.user, Search("query => {'kind': 'dense', 'vector_name': 'text_dense', 'vector': " + Vector(768) + "}")),
	         "Binder Error: No function matches");
	Expect("and sends nothing", std::to_string(Backend().CallCount()), "0");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits("{\"id\":3,\"score\":0.7,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d3\","
	                                     "\"status\":\"draft\"}}"))};
	Contains("a hit outside U fails the statement",
	         RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''active''', k => 2", "document_id", "")),
	         "search_schema_mismatch: ", "draft");
	Backend().Reset();
	{
		// Over the parse budget (kResponseJsonLimits.max_nodes): search_budget_exceeded, detail only logged.
		std::string many = "{\"result\":{\"points\":[0";
		for (int i = 0; i < 1048576; ++i) {
			many += ",0";
		}
		many += "]},\"status\":\"ok\"}";
		Backend().replies = {Reply(200, many)};
	}
	auto over = RunSql(*e.user, Search(dense + ", k => 2"));
	Contains("query response over the parse budget", over, "search_budget_exceeded: ", "search_schema_mismatch");
	Contains("tenant text of the budget refusal", over, "Invalid Input Error: search_budget_exceeded: the search response", "{");

	// Column mapping (Task 4 invariant): a policy on document_id and a filter on status, both VARCHAR like
	// tenant_id, lower onto their own payload keys; the include list is projected plus filter fields.
	e.handle.row_policies = {{"p0", "search_docs", "main", "contracts", "document_id = 'd1'"}};
	Expect("install same-type policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("same-type policy search",
	       RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''active''', k => 2", "owner_id, _qod_score", "")), "7|1.0");
	Contains("each field on its own key, include in declared order", Body(0),
	         "\"filter\":{\"must\":[{\"key\":\"document_id\",\"match\":{\"value\":\"d1\"}},{\"key\":\"status\","
	         "\"match\":{\"value\":\"active\"}}]},\"limit\":2,"
	         "\"with_payload\":{\"include\":[\"document_id\",\"status\",\"owner_id\"]},\"with_vector\":false}");

	// More filter nodes than search_max_filter_nodes after combining P and U: refused before any request.
	e.handle.row_policies = {{"pn", "search_docs", "main", "contracts", "owner_id = 7"}};
	Expect("install a one-node policy", e.Install(), "true");
	Expect("lower the node budget", RunSql(*e.setup, "SET GLOBAL search_max_filter_nodes = 2"), "");
	Backend().Reset();
	Contains("P AND U over the node budget", RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''active''', k => 2")),
	         "search_budget_exceeded: ");
	Expect("no request over the node budget", std::to_string(Backend().CallCount()), "0");
	e.handle.row_policies = {};
	Expect("install without policy", e.Install(), "true");
	// filter_sql alone over the cap: refused at bind, in the connector's words (core's row policy text is
	// for P AND U, above).
	auto two_nodes = dense + ", filter_sql => 'status = ''active'' AND document_id = ''d1''', k => 2";
	Contains("filter_sql alone over the node budget", RunSql(*e.user, Search(two_nodes)),
	         "search_budget_exceeded: filter_sql has more filter nodes than search_max_filter_nodes", "row policy");
	Contains("refused at bind: a prepare fails too", e.user->Prepare(Search(two_nodes))->GetError(),
	         "search_budget_exceeded: filter_sql has more filter nodes");
	Expect("no request for filter_sql over the node budget", std::to_string(Backend().CallCount()), "0");
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("U alone fits the node budget",
	       RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''active''', k => 2", "document_id", "")), "d1");
	Expect("reset the node budget", RunSql(*e.setup, "RESET search_max_filter_nodes"), "");

	e.handle.row_policies = {{"p1", "search_docs", "main", "contracts", "owner_id IN (7, 8)"}};
	Expect("install exact policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Expect("exact policy search", RunSql(*e.user, Search(dense + ", k => 2", "document_id", "ORDER BY 1")), "d1,d2");
	Contains("P lowered", Body(0), "\"filter\":{\"must\":[{\"key\":\"owner_id\",\"match\":{\"any\":[7,8]}}]},");
	Contains("policy fields decoded for the recheck", Body(0), "\"include\":[\"document_id\",\"owner_id\"]");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits("{\"id\":3,\"score\":0.7,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d3\","
	                                     "\"owner_id\":9}}"))};
	Contains("a hit outside P fails the statement", RunSql(*e.user, Search(dense + ", k => 2", "document_id", "")),
	         "search_schema_mismatch: ", "d3");

	e.handle.row_policies = {{"p2", "search_docs", "main", "contracts", "owner_id > 5"}};
	Expect("install inexact policy", e.Install(), "true");
	Backend().Reset();
	Contains("inexact policy refused", RunSql(*e.user, Search(dense + ", k => 2")),
	         "QoD: search_policy_unsupported: a row policy cannot be enforced before the connector's candidate limit");
	Expect("zero requests before authorization", std::to_string(Backend().CallCount()), "0");

	// A row policy that itself lowers to FALSE (an empty IN on tenant_id) grants nothing: F is FALSE, the
	// search answers no rows and sends no request, never a request without a filter.
	e.handle.row_policies = {{"pf", "search_docs", "main", "contracts", "tenant_id IN ()"}};
	Expect("install a FALSE policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("FALSE policy: no rows", RunSql(*e.user, Search(dense + ", k => 2", "document_id", "")), "");
	Expect("FALSE policy: no request", std::to_string(Backend().CallCount()), "0");

	e.handle.row_policies = {};
	Expect("install no policy", e.Install(), "true");
	Backend().Reset();
	for (auto bad : {dense + ", k => 0", dense + ", k => 1001", "query => qod_dense('text_dense', " + Vector(3) + ")",
	                 "query => qod_dense('no_such_vector', " + Vector(768) + ")", std::string("k => 2"),
	                 dense + ", filter_sql => 'status LIKE ''a%'''", dense + ", filter_sql => 'secret_col = 1'",
	                 "query => qod_dense('text_dense', " + Vector(768, "NULL") + ")"}) {
		Contains("refused at bind: " + bad.substr(0, 60), RunSql(*e.user, Search(bad)), "search_query_unsupported: ");
	}
	Contains("not a qdrant relation", RunSql(*e.user, "SELECT * FROM qod_search('memory.main.nope', " + dense + ")"),
	         "search_query_unsupported: ");
	Expect("a plain table", RunSql(*e.setup, "CREATE TABLE plain (x INTEGER)"), "");
	Contains("a table of another catalog", RunSql(*e.user, "SELECT * FROM qod_search('memory.main.plain', " + dense + ")"),
	         "search_query_unsupported: ");
	Contains("a NULL relation", RunSql(*e.user, "SELECT * FROM qod_search(NULL, " + dense + ")"), "search_query_unsupported: ");
	for (auto bad : {"query => qod_dense('text_dense', " + Vector(767) + " || ['nan'::FLOAT])",
	                 "query => qod_dense('text_dense', " + Vector(767) + " || ['inf'::FLOAT])",
	                 "query => qod_dense('text_dense', " + Vector(767) + " || ['-inf'::FLOAT])",
	                 "query => qod_dense(NULL, " + Vector(768) + ")", std::string("query => NULL"),
	                 "query => " + Spelled("'sparse'", "'text_dense'", Vector(768)),
	                 "query => " + Spelled("NULL", "'text_dense'", Vector(768)),
	                 "query => " + Spelled("'dense'", "'no_such_vector'", Vector(768)),
	                 dense + ", filter_sql => 'status = ?'", dense + ", filter_sql => 'status = $1'"}) {
		auto refusal = RunSql(*e.user, Search(bad));
		Contains("refused at bind: " + bad.substr(0, 60), refusal, "search_query_unsupported: ");
		Contains("refusal names no collection: " + bad.substr(0, 40), refusal, "search_query_unsupported: ", "contracts_v3");
		Contains("refusal names no backend: " + bad.substr(0, 40), refusal, "search_query_unsupported: ", "qdrant");
	}
	for (auto bad : {std::string("query => qod_dense('text_dense', NULL::FLOAT[])"), std::string("query => qod_dense('text_dense', []::FLOAT[])"),
	                 "query => " + Spelled("'dense'", "'text_dense'", "NULL::FLOAT[]"),
	                 dense + ", k => NULL", dense + ", k => NULL::BIGINT", dense + ", filter_sql => NULL"}) {
		Contains("explicit NULL or empty refused: " + bad.substr(0, 60), RunSql(*e.user, Search(bad)), "search_query_unsupported: ");
	}
	Contains("a name with an unbalanced quote", RunSql(*e.user, "SELECT * FROM qod_search('\"search_docs', " + dense + ")"),
	         "search_query_unsupported: ");
	Expect("no request for any refusal", std::to_string(Backend().CallCount()), "0");
	Expect("prepare with k as a parameter", RunSql(*e.user, "PREPARE kp AS " + Search(dense + ", k := ?", "document_id", "")), "");
	Contains("k bound to NULL at execution", RunSql(*e.user, "EXECUTE kp(NULL)"), "search_query_unsupported: ");
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("k bound at execution", RunSql(*e.user, "EXECUTE kp(1)"), "d1");
	Contains("the bound k is the limit", Body(0), "\"limit\":1,");
	Backend().Reset();

	Expect("prepare with the vector as a parameter (:= as the contract writes it)",
	       RunSql(*e.user, "PREPARE v AS SELECT document_id FROM qod_search('search_docs.main.contracts', "
	                       "query := qod_dense('text_dense', ?::FLOAT[]), k := 1)"),
	       "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute binds the vector", RunSql(*e.user, "EXECUTE v(" + Vector(768) + ")"), "d1");
	Contains("the bound vector reached the request", Body(0), "{\"query\":[0.5,0.5,");
	Backend().Reset();
	Contains("a vector of the wrong size is refused at execution", RunSql(*e.user, "EXECUTE v(" + Vector(3) + ")"),
	         "search_query_unsupported: ");
	Expect("and sends nothing", std::to_string(Backend().CallCount()), "0");

	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Contains("more hits than k", RunSql(*e.user, Search(dense + ", k => 1")), "search_schema_mismatch: ");
	// Tenants are separated by a row policy on the ordinary field tenant_id: lowered into the remote filter
	// with U, and a hit of another tenant fails the statement.
	e.handle.row_policies = {{"t1", "search_docs", "main", "contracts", "tenant_id = 'acme'"}};
	Expect("install tenant policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("tenant policy search",
	       RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''active''', k => 2", "document_id", "")), "d1");
	Contains("tenant policy AND U lowered", Body(0),
	         "\"filter\":{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}},{\"key\":\"status\","
	         "\"match\":{\"value\":\"active\"}}]},\"limit\":2,\"with_payload\":{\"include\":[\"tenant_id\",\"document_id\","
	         "\"status\"]}");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits("{\"id\":9,\"score\":0.9,\"payload\":{\"tenant_id\":\"bob\",\"document_id\":\"x\"}}"))};
	Contains("hit outside the tenant policy", RunSql(*e.user, Search(dense + ", k => 2")), "search_schema_mismatch: ", "bob");
	e.handle.row_policies = {{"t2", "search_docs", "main", "contracts", "tenant_id = 'acme' AND owner_id > 5"}};
	Expect("install inexact tenant policy", e.Install(), "true");
	Backend().Reset();
	Contains("inexact tenant policy refused", RunSql(*e.user, Search(dense + ", k => 2")), "search_policy_unsupported: ");
	Expect("zero requests before authorization (tenant policy)", std::to_string(Backend().CallCount()), "0");
	e.handle.row_policies = {};
	Expect("install no policy after the tenant policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Broken(qod_qdrant::Failure::Cancelled)};
	Contains("cancellation interrupts", RunSql(*e.user, Search(dense + ", k => 2")), "Interrupted");

	// A prepared search without parameters, then a policy change before the next execution: the new
	// policy is lowered (the statement is rebound and the connector reseals) or the search is refused.
	Expect("prepare without parameters", RunSql(*e.user, "PREPARE w AS " + Search(dense + ", k => 2", "document_id", "")), "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute without parameters", RunSql(*e.user, "EXECUTE w"), "d1");
	Contains("no filter before the change", Body(0), "\"using\":\"text_dense\",\"limit\":2,", "\"filter\"");
	e.handle.row_policies = {{"p6", "search_docs", "main", "contracts", "owner_id = 7"}};
	Expect("install a policy after prepare", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute after the change", RunSql(*e.user, "EXECUTE w"), "d1");
	Contains("the new policy reached the request", Body(0), "{\"key\":\"owner_id\",\"match\":{\"value\":7}}");
	e.handle.row_policies = {};
	Expect("install no policy again", e.Install(), "true");

	Expect("prepare", RunSql(*e.user, "PREPARE s AS " + Search(dense + ", filter_sql => ?, k => 2", "document_id", "ORDER BY 1")), "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute", RunSql(*e.user, "EXECUTE s('status = ''active''')"), "d1");
	Contains("filter revalidated each execution", RunSql(*e.user, "EXECUTE s('status LIKE ''a%''')"), "search_query_unsupported: ");
	e.handle.row_policies = {{"p3", "search_docs", "main", "contracts", "status = 'active'"}};
	Expect("install policy while prepared", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("prepared search reseals", RunSql(*e.user, "EXECUTE s('owner_id = 7')"), "d1");
	Contains("new policy lowered", Body(0), "{\"key\":\"status\",\"match\":{\"value\":\"active\"}}");

	e.handle.row_policies = {};
	e.handle.column_policies = {{"c1", "search_docs", "main", "contracts", "body", "deny", ""}};
	Expect("install denied body", e.Install(), "true");
	Backend().Reset();
	Contains("denied field via vector lineage", RunSql(*e.user, Search(dense + ", k => 2", "document_id", "")), "field_denied");
	e.handle.column_policies = {{"c2", "search_docs", "main", "contracts", "status", "mask", "'***'"}};
	Expect("install masked status", e.Install(), "true");
	Contains("masked field in filter_sql", RunSql(*e.user, Search(dense + ", filter_sql => 'status = ''x''', k => 2", "document_id", "")),
	         "field_denied");
	Expect("no request for a denied use", std::to_string(Backend().CallCount()), "0");
	e.handle.column_policies = {};
	Expect("install no column policy", e.Install(), "true");
	// A prepared statement whose catalog is detached, and maybe attached again, is rebound (the search
	// records its catalog read as a base table reference does) instead of reading the freed table entry.
	Expect("prepare a search before DETACH", RunSql(*e.user, "PREPARE d AS " + Search(dense + ", k => 2", "document_id", "")), "");
	Expect("prepare a plain scan before DETACH", RunSql(*e.user, "PREPARE p AS SELECT document_id FROM search_docs.main.contracts"),
	       "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute before DETACH", RunSql(*e.user, "EXECUTE d"), "d1");
	Expect("detach", RunSql(*e.setup, "DETACH search_docs"), "");
	Expect("attach again", e.Attach("search_docs", config), "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute after DETACH and ATTACH rebinds", RunSql(*e.user, "EXECUTE d"), "d1");
	Expect("one request after the rebind", std::to_string(Backend().CallCount()), "1");
	Backend().Reset();
	Backend().replies = {Reply(200, "{\"result\":{\"points\":[" + kHit1 + "],\"next_page_offset\":null},\"status\":\"ok\"}")};
	Expect("a prepared plain scan rebinds too", RunSql(*e.user, "EXECUTE p"), "d1");
	Expect("detach again", RunSql(*e.setup, "DETACH search_docs"), "");
	Backend().Reset();
	Contains("execute after DETACH refuses", RunSql(*e.user, "EXECUTE d"), "search_query_unsupported: ");
	Expect("and sends nothing", std::to_string(Backend().CallCount()), "0");
	Expect("attach for the rest", e.Attach("search_docs", config), "");

	{
		// The datatype branch of CheckedVector: the config parser admits only float32, so a registry entry
		// with another datatype is built directly.
		qod_qdrant::VectorSpec spec;
		spec.name = "v";
		spec.dimensions = 2;
		spec.datatype = "float32";
		auto two = duckdb::Value::LIST(duckdb::LogicalType::FLOAT, {duckdb::Value::FLOAT(1), duckdb::Value::FLOAT(2)});
		Expect("float32 vector accepted", ErrorOf([&] { qod_qdrant::CheckedVector(two, spec); }), "no error");
		spec.datatype = "float16";
		Contains("another datatype refused", ErrorOf([&] { qod_qdrant::CheckedVector(two, spec); }), "search_query_unsupported: ");
	}
	{
		// The search's global init checks the column ids once, then the seal, before any I/O; a bind data
		// that is not ours fails closed with a coded error.
		e.setup->BeginTransaction();
		auto &context = *e.setup->context;
		auto entry = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(
		    context, duckdb::QualifiedName(duckdb::Identifier("search_docs"), duckdb::Identifier("main"),
		                                   duckdb::Identifier("contracts")),
		    duckdb::OnEntryNotFound::RETURN_NULL);
		auto table = entry ? dynamic_cast<qod_qdrant::QdrantTableEntry *>(entry.get()) : nullptr;
		ExpectTrue("the table entry is a qdrant table", table != nullptr);
		if (table) {
			qod_qdrant::QdrantScanData bind(*table, true);
			ExpectTrue("search bind data is bounded", bind.IsBounded());
			auto function = qod_qdrant::SearchFunction();
			duckdb::vector<duckdb::idx_t> no_projection;
			duckdb::TableFunctionInitInput bad(&bind, duckdb::vector<duckdb::column_t> {1, 6}, no_projection, nullptr);
			Contains("unknown column id refused at init", ErrorOf([&] { function.init_global(context, bad); }),
			         "search_query_unsupported: ");
			duckdb::TableFunctionInitInput good(&bind, duckdb::vector<duckdb::column_t> {1, qod_qdrant::ScoreColumn()},
			                                    no_projection, nullptr);
			Contains("known column ids reach the seal check", ErrorOf([&] { function.init_global(context, good); }),
			         "QoD: connector scan ran without a sealed policy obligation");
			duckdb::TableFunctionData foreign_data;
			duckdb::TableFunctionInitInput foreign_input(&foreign_data, duckdb::vector<duckdb::column_t> {1}, no_projection,
			                                             nullptr);
			Contains("foreign bind data refused at init", ErrorOf([&] { function.init_global(context, foreign_input); }),
			         "search_query_unsupported: ");
		}
		e.setup->Rollback();
	}
	Backend().Reset();
	Contains("internal connection never sealed", RunSql(*e.setup, Search(dense + ", k => 2")),
	         "QoD: connector scan ran without a sealed policy obligation");
	Expect("no request from any init refusal", std::to_string(Backend().CallCount()), "0");
	return Finish();
}
