// Keyword (BM25) and hybrid (RRF) qod_search through the real node hooks and the fake transport: the
// guarantee gate and every query refusal at bind with zero requests, the request shapes of the spike record
// (C = P inside every keyword stage, never at the top level of a hybrid request, F on every stage; no corpus
// at all without a row policy), a tenant_id row policy in both F and C, sparse dependsOn field uses, exact
// lowering of C or refusal, the slice 2 settings, prepared statements, and the local recheck of every hit.
#include "ast/json.hpp"
#include "check.hpp"
#include "engine.hpp"
#include "fake_transport.hpp"

using namespace qod_qdrant_test;

namespace {
std::string Vector(size_t n, const std::string &value = "0.5") {
	std::string out = "[";
	for (size_t i = 0; i < n; ++i) {
		out += (i ? "," : "") + value;
	}
	return out + "]::FLOAT[]";
}

const std::string kDense = "qod_dense('text_dense', " + Vector(768) + ")";
const std::string kKeyword = "qod_bm25('body_bm25', 'merger agreement')";
const std::string kHybrid = "qod_rrf([" + kKeyword + ", " + kDense + "])";

std::string Search(const std::string &args, const std::string &select = "document_id, _qod_score",
                   const std::string &tail = "ORDER BY _qod_score DESC", const std::string &relation = "search_docs") {
	return "SELECT " + select + " FROM qod_search('" + relation + ".main.contracts', " + args + ") " + tail;
}

std::string Hits(const std::string &points) {
	return "{\"result\":{\"points\":[" + points + "]},\"status\":\"ok\",\"time\":0.001}";
}

const std::string kHit1 = "{\"id\":1,\"version\":1,\"score\":7.1,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d1\","
                          "\"status\":\"active\",\"owner_id\":7}}";
const std::string kHit2 = "{\"id\":2,\"version\":1,\"score\":5.6,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d2\","
                          "\"status\":\"active\",\"owner_id\":8}}";

std::string Body(size_t i) {
	std::lock_guard<std::mutex> lock(Backend().mutex);
	return i < Backend().calls.size() ? Backend().calls[i].body : "no call";
}

std::string Calls() {
	return std::to_string(Backend().CallCount());
}

//! The member `key` of `object` written back as JSON, or "absent".
std::string MemberJson(const qod_qdrant::JsonNode &object, const std::string &key) {
	auto member = object.Find(key);
	return member ? qod_qdrant::WriteJson(*member) : "absent";
}

//! The tenant part of a row policy on the ordinary field tenant_id, as lowered.
const std::string kT = "{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}}";
const std::string kU = "{\"key\":\"status\",\"match\":{\"value\":\"active\"}}";
const std::string kP = "{\"key\":\"owner_id\",\"match\":{\"any\":[7,8]}}";
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	InstallFakeTransport();
	Engine e;
	auto lexical = ReadFile(fixtures + "sample2_canonical.json");
	Expect("attach a filtered_retrieval source", e.Attach("search_docs", lexical), "");
	// The same collection staged under exact_isolated: sparse vectors declared, keyword search refused.
	auto staged = Patch(lexical, "\"guarantee\":\"filtered_retrieval\"", "\"guarantee\":\"exact_isolated\"");
	Expect("attach an exact_isolated source", e.Attach("staged_docs", staged, ", READ_ONLY", "qdrant:fs-qdrant-2"), "");
	Expect("install", e.Install(), "true");

	{
		Backend().Reset();
		ExpectTrue("prepare a keyword search", !e.user->Prepare(Search("query => " + kKeyword + ", k => 2"))->HasError());
		ExpectTrue("prepare a hybrid search", !e.user->Prepare(Search("query => " + kHybrid + ", k => 2"))->HasError());
		Contains("explain a hybrid search", RunSql(*e.user, "EXPLAIN " + Search("query => " + kHybrid + ", k => 2")), "QOD_SEARCH");
		Expect("no request before execution", Calls(), "0");
	}

	// Keyword without a row policy: F = U, C is TRUE so the request carries no params at all (statistics over
	// the whole collection), the text trimmed, k as the limit.
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Expect("keyword rows", RunSql(*e.user, Search("query => qod_bm25('body_bm25', '  merger agreement \n'), "
	                                              "filter_sql => 'status = ''active''', k => 2")),
	       "d1|7.1,d2|5.6");
	Expect("keyword request", Body(0),
	       "{\"query\":{\"text\":\"merger agreement\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"filter\":{\"must\":[" +
	           kU + "]},\"limit\":2,\"with_payload\":{\"include\":[\"document_id\",\"status\"]},\"with_vector\":false}");
	Contains("sparse lineage reported as a read", LastDescription(), "\"body\"");
	Expect("one request", Calls(), "1");

	// Hybrid: one prefetch per branch with F, the keyword one without params (no row policy: C is TRUE),
	// prefetch_k defaulting to max(k, 100).
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Expect("hybrid rows", RunSql(*e.user, Search("query => " + kHybrid + ", filter_sql => 'status = ''active''', k => 2")),
	       "d1|7.1,d2|5.6");
	auto f = "{\"must\":[" + kU + "]}";
	auto head = "{\"prefetch\":[{\"query\":{\"text\":\"merger agreement\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\","
	            "\"filter\":" + f + ",\"limit\":100},{\"query\":[0.5,";
	Expect("hybrid request head", Body(0).substr(0, head.size()), head);
	Contains("hybrid request tail", Body(0),
	         "],\"using\":\"text_dense\",\"filter\":" + f + ",\"limit\":100}],\"query\":{\"rrf\":{}},\"filter\":" + f +
	             ",\"limit\":2,\"with_payload\":{\"include\":[\"document_id\",\"status\"]},\"with_vector\":false}");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("prefetch_k as given", RunSql(*e.user, Search("query => qod_rrf([" + kKeyword + ", " + kDense + "], prefetch_k => 50), k => 10",
	                                                     "document_id", "")),
	       "d1");
	Contains("prefetch limit 50", Body(0), "\"limit\":50}");
	Contains("top-level limit k, no filter without a row policy or filter_sql", Body(0), "\"query\":{\"rrf\":{}},\"limit\":10,");
	Expect("hybrid request, no policy and no filter_sql: no filter and no params on any stage",
	       Body(0).substr(0, Body(0).find("{\"query\":[0.5,")),
	       "{\"prefetch\":[{\"query\":{\"text\":\"merger agreement\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\","
	       "\"limit\":50},");
	Contains("no stage carries a filter or a corpus", Body(0), "\"using\":\"text_dense\",\"limit\":50}]", "\"params\"");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("keyword, no policy and no filter_sql",
	       RunSql(*e.user, Search("query => " + kKeyword + ", k => 2", "document_id", "")), "d1");
	Expect("keyword request: whole-collection statistics, no filter", Body(0),
	       "{\"query\":{\"text\":\"merger agreement\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"limit\":2,"
	       "\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("hybrid of two keyword branches, no policy",
	       RunSql(*e.user, Search("query => qod_rrf([" + kKeyword + ", qod_bm25('title_bm25', 'deal')]), "
	                              "filter_sql => 'status = ''active''', k => 2",
	                              "document_id", "")),
	       "d1");
	Expect("each keyword prefetch without params", Body(0),
	       "{\"prefetch\":[{\"query\":{\"text\":\"merger agreement\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\","
	       "\"filter\":" + f + ",\"limit\":100},{\"query\":{\"text\":\"deal\",\"model\":\"qdrant/bm25\"},\"using\":\"title_bm25\","
	       "\"filter\":" + f + ",\"limit\":100}],\"query\":{\"rrf\":{}},\"filter\":" + f +
	           ",\"limit\":2,\"with_payload\":{\"include\":[\"document_id\",\"status\"]},\"with_vector\":false}");

	// An exact row policy that separates tenants on the ordinary field tenant_id: F = P AND U, C = P, read back
	// from the JSON that left the node. The tenant restriction is in both the filter and the corpus.
	e.handle.row_policies = {{"p1", "search_docs", "main", "contracts", "tenant_id = 'acme' AND owner_id IN (7, 8)"}};
	Expect("install an exact policy", e.Install(), "true");
	auto full_f = "{\"must\":[" + kT + "," + kP + "," + kU + "]}";
	auto c = "{\"must\":[" + kT + "," + kP + "]}";
	for (auto query : {kKeyword, kHybrid, "qod_rrf([" + kDense + ", qod_bm25('title_bm25', 'deal'), " + kKeyword + "])"}) {
		Backend().Reset();
		Backend().replies = {Reply(200, Hits(kHit1))};
		auto label = query.substr(0, 12) + "/" + std::to_string(query.size());
		Expect("policy search " + label,
		       RunSql(*e.user, Search("query => " + query + ", filter_sql => 'status = ''active''', k => 2", "document_id", "")),
		       "d1");
		auto sent = qod_qdrant::ParseJson(Body(0), qod_qdrant::kResponseJsonLimits);
		Expect(label + ": top-level filter is F", MemberJson(sent, "filter"), full_f);
		auto prefetch = sent.Find("prefetch");
		if (!prefetch) {
			Expect(label + ": keyword corpus is C", MemberJson(*sent.Find("params")->Find("idf"), "corpus"), c);
			continue;
		}
		Expect(label + ": never a top-level params on a hybrid request", MemberJson(sent, "params"), "absent");
		for (size_t i = 0; i < prefetch->items.size(); ++i) {
			auto &stage = prefetch->items[i];
			auto where = label + " prefetch " + std::to_string(i);
			Expect(where + ": filter is F", MemberJson(stage, "filter"), full_f);
			if (stage.Find("query")->Is(qod_qdrant::JsonNode::Kind::Object)) {
				Expect(where + ": keyword corpus is C", MemberJson(*stage.Find("params")->Find("idf"), "corpus"), c);
			} else {
				Expect(where + ": dense stage has no params", MemberJson(stage, "params"), "absent");
			}
		}
	}
	Backend().Reset();
	Backend().replies = {Reply(200, Hits("{\"id\":3,\"score\":4.0,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d3\","
	                                     "\"status\":\"active\",\"owner_id\":9}}"))};
	Contains("a keyword hit outside P fails the statement", RunSql(*e.user, Search("query => " + kKeyword + ", k => 2", "document_id", "")),
	         "search_schema_mismatch: ", "d3");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits("{\"id\":5,\"score\":4.0,\"payload\":{\"tenant_id\":\"bob\",\"document_id\":\"d5\","
	                                     "\"status\":\"active\",\"owner_id\":7}}"))};
	Contains("a keyword hit of another tenant fails the statement",
	         RunSql(*e.user, Search("query => " + kKeyword + ", k => 2", "document_id", "")), "search_schema_mismatch: ", "bob");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits("{\"id\":4,\"score\":0.5,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d4\","
	                                     "\"status\":\"draft\",\"owner_id\":7}}"))};
	Contains("a fused hit outside U fails the statement",
	         RunSql(*e.user, Search("query => " + kHybrid + ", filter_sql => 'status = ''active''', k => 2", "document_id", "")),
	         "search_schema_mismatch: ", "draft");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1 + "," + kHit2))};
	Contains("more fused hits than k", RunSql(*e.user, Search("query => " + kHybrid + ", k => 1")), "search_schema_mismatch: ");

	// The node cap applies to C as well: with U FALSE, F is empty and C = P alone is over the cap.
	Expect("lower the node budget", RunSql(*e.setup, "SET GLOBAL search_max_filter_nodes = 2"), "");
	Backend().Reset();
	Contains("C over the node budget", RunSql(*e.user, Search("query => " + kKeyword + ", filter_sql => 'status IN ()', k => 2")),
	         "search_budget_exceeded: ");
	Expect("no request for C over the budget", Calls(), "0");
	Expect("a dense search sends no C: FALSE filter, no rows, no request",
	       RunSql(*e.user, Search("query => " + kDense + ", filter_sql => 'status IN ()', k => 2")), "");
	Expect("and no request", Calls(), "0");
	Expect("reset the node budget", RunSql(*e.setup, "RESET search_max_filter_nodes"), "");

	// A row policy that itself lowers to FALSE (an empty IN on tenant_id) grants nothing: F and C are FALSE,
	// keyword and hybrid searches answer no rows and send no request, never one without a filter or corpus.
	e.handle.row_policies = {{"pf", "search_docs", "main", "contracts", "tenant_id IN ()"}};
	Expect("install a FALSE policy", e.Install(), "true");
	for (auto query : {kKeyword, kHybrid}) {
		Backend().Reset();
		Backend().replies = {Reply(200, Hits(kHit1))};
		auto label = query.substr(0, 8);
		Expect("FALSE policy, no rows: " + label, RunSql(*e.user, Search("query => " + query + ", k => 2", "document_id", "")), "");
		Expect("FALSE policy, no request: " + label, Calls(), "0");
	}

	// C is lowered exactly or the search refused, as F: an inexact policy refuses keyword and hybrid alike.
	e.handle.row_policies = {{"p2", "search_docs", "main", "contracts", "owner_id > 5"}};
	Expect("install an inexact policy", e.Install(), "true");
	Backend().Reset();
	for (auto query : {kKeyword, kHybrid}) {
		Contains("inexact policy refused: " + query.substr(0, 8), RunSql(*e.user, Search("query => " + query + ", k => 2")),
		         "QoD: search_policy_unsupported: a row policy cannot be enforced before the connector's candidate limit");
	}
	Expect("zero requests for an inexact policy", Calls(), "0");
	e.handle.row_policies = {};
	Expect("install no policy", e.Install(), "true");

	// The guarantee gate: before any other per-branch check and any I/O, prepare included.
	const std::string gate = "search_query_unsupported: keyword search is not enabled for this source";
	Backend().Reset();
	for (auto query : {kKeyword, kHybrid, std::string("qod_bm25('no_such', '')"), std::string("qod_rrf([") + kKeyword + "])"}) {
		auto refusal = RunSql(*e.user, Search("query => " + query + ", k => 2", "document_id", "", "staged_docs"));
		Contains("gate: " + query.substr(0, 20), refusal, gate);
		Contains("gate names no backend: " + query.substr(0, 20), refusal, gate, "qdrant");
	}
	Contains("gate at prepare", e.user->Prepare(Search("query => " + kKeyword, "document_id", "", "staged_docs"))->GetError(), gate);
	Expect("zero requests behind the gate", Calls(), "0");
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("dense search on the exact_isolated source", RunSql(*e.user, Search("query => " + kDense + ", k => 2", "document_id", "",
	                                                                          "staged_docs")),
	       "d1");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("dense-only fusion on the exact_isolated source",
	       RunSql(*e.user, Search("query => qod_rrf([" + kDense + ", " + kDense + "]), k => 2", "document_id", "", "staged_docs")), "d1");
	Contains("dense-only fusion carries no corpus", Body(0), "\"query\":{\"rrf\":{}}", "\"params\"");

	// Every other refusal of contract section 2, in its wording, at bind, with zero requests.
	Backend().Reset();
	std::vector<std::pair<std::string, std::string>> refusals = {
	    {"NULL", "query must be qod_dense, qod_bm25 or qod_rrf"},
	    {"qod_bm25('body_bm25', NULL)", "query must be qod_dense, qod_bm25 or qod_rrf"},
	    {"qod_rrf([" + kKeyword + "])", "qod_rrf takes 2..4 branches"},
	    {"qod_rrf([qod_rrf([" + kKeyword + ", " + kDense + "]), " + kDense + "])", "qod_rrf branches must be qod_dense or qod_bm25"},
	    {"qod_bm25('text_dense', 'merger')", "qod_bm25 names a vector the relation does not declare as sparse"},
	    {"qod_dense('body_bm25', " + Vector(768) + ")", "qod_dense names a vector the relation does not declare"},
	    {"qod_bm25('body_bm25', ' \t ')", "the query text must be 1..4096 bytes"},
	    {"qod_bm25('body_bm25', repeat('a', 4097))", "the query text must be 1..4096 bytes"},
	    {"qod_rrf([" + kKeyword + ", " + kDense + "], prefetch_k => 1), k => 10", "prefetch_k must be in 10..1000"},
	    {"qod_rrf([" + kKeyword + ", " + kDense + "], prefetch_k => 1001)", "prefetch_k must be in 10..1000"},
	};
	for (auto &r : refusals) {
		Contains("refused: " + r.first.substr(0, 40), RunSql(*e.user, Search("query => " + r.first)),
		         "search_query_unsupported: " + r.second);
	}
	Contains("a hand-spelled slice 1 struct does not bind",
	         RunSql(*e.user, Search("query => {'kind': 'bm25', 'vector_name': 'body_bm25', 'vector': NULL::FLOAT[]}")),
	         "Binder Error: No function matches");
	Expect("zero requests for every refusal", Calls(), "0");
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("a hand-spelled keyword value goes through the same checks",
	       RunSql(*e.user, Search("query => {'kind': 'bm25', 'vector_name': 'body_bm25', 'vector': NULL, 'text': 'merger', "
	                              "'prefetch_k': NULL, 'branches': NULL}, k => 2",
	                              "document_id", "")),
	       "d1");
	Contains("and builds the keyword request", Body(0), "{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\"");

	// The slice 2 settings, set by the trusted init.
	Expect("two branches at most", RunSql(*e.setup, "SET GLOBAL search_max_fusion_branches = 2"), "");
	Expect("five bytes of text at most", RunSql(*e.setup, "SET GLOBAL search_max_query_text_bytes = 5"), "");
	Expect("prefetch_k 50 at most", RunSql(*e.setup, "SET GLOBAL search_max_prefetch_k = 50"), "");
	Backend().Reset();
	Contains("search_max_fusion_branches", RunSql(*e.user, Search("query => qod_rrf([" + kDense + "," + kDense + "," + kDense + "])")),
	         "search_query_unsupported: qod_rrf takes 2..2 branches");
	Contains("search_max_query_text_bytes", RunSql(*e.user, Search("query => qod_bm25('body_bm25', 'merger')")),
	         "search_query_unsupported: the query text must be 1..5 bytes");
	Expect("zero requests over the slice 2 limits", Calls(), "0");
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("default prefetch_k capped by search_max_prefetch_k",
	       RunSql(*e.user, Search("query => qod_rrf([qod_bm25('body_bm25', 'deal'), " + kDense + "]), k => 10", "document_id", "")), "d1");
	Contains("prefetch limit 50", Body(0), "\"limit\":50}");
	for (auto name : {"search_max_fusion_branches", "search_max_query_text_bytes", "search_max_prefetch_k"}) {
		Expect(std::string("reset ") + name, RunSql(*e.setup, std::string("RESET ") + name), "");
	}
	Expect("the request budget covers the whole body", RunSql(*e.setup, "SET GLOBAL search_max_request_bytes = 1024"), "");
	Backend().Reset();
	Contains("a keyword request over search_max_request_bytes",
	         RunSql(*e.user, Search("query => qod_bm25('body_bm25', repeat('a', 1500))")), "search_budget_exceeded: ");
	Expect("is never sent", Calls(), "0");
	Expect("reset the request budget", RunSql(*e.setup, "RESET search_max_request_bytes"), "");

	// The text as a prepared parameter: checked at every execution.
	Expect("prepare with the text as a parameter",
	       RunSql(*e.user, "PREPARE kw AS " + Search("query => qod_bm25('body_bm25', ?), k => 2", "document_id", "")), "");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("execute binds the text", RunSql(*e.user, "EXECUTE kw('merger')"), "d1");
	Contains("the bound text reached the request", Body(0), "{\"query\":{\"text\":\"merger\",");
	Backend().Reset();
	Contains("a blank text refused at execution", RunSql(*e.user, "EXECUTE kw('   ')"),
	         "search_query_unsupported: the query text must be 1..4096 bytes");
	Contains("a NULL text refused at execution", RunSql(*e.user, "EXECUTE kw(NULL)"),
	         "search_query_unsupported: query must be qod_dense, qod_bm25 or qod_rrf");
	Expect("and sends nothing", Calls(), "0");

	// Field uses: the dependsOn of every branch's vector, dense or sparse.
	e.handle.column_policies = {{"c1", "search_docs", "main", "contracts", "body", "deny", ""}};
	Expect("install denied body", e.Install(), "true");
	Backend().Reset();
	Contains("denied body via body_bm25", RunSql(*e.user, Search("query => " + kKeyword + ", k => 2", "document_id", "")), "field_denied");
	Contains("denied body via the dense branch of a fusion",
	         RunSql(*e.user, Search("query => qod_rrf([qod_bm25('title_bm25', 'deal'), " + kDense + "]), k => 2", "document_id", "")),
	         "field_denied");
	Expect("no request for a denied use", Calls(), "0");
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("title_bm25 does not read body",
	       RunSql(*e.user, Search("query => qod_bm25('title_bm25', 'deal'), k => 2", "document_id", "")), "d1");
	e.handle.column_policies = {{"c2", "search_docs", "main", "contracts", "title", "mask", "'***'"}};
	Expect("install masked title", e.Install(), "true");
	Backend().Reset();
	Contains("masked title via title_bm25", RunSql(*e.user, Search("query => qod_bm25('title_bm25', 'deal'), k => 2", "document_id", "")),
	         "field_denied");
	Contains("masked title via a fusion of two keyword branches",
	         RunSql(*e.user, Search("query => qod_rrf([" + kKeyword + ", qod_bm25('title_bm25', 'deal')]), k => 2", "document_id", "")),
	         "field_denied");
	Expect("no request for a masked use", Calls(), "0");
	e.handle.column_policies = {};
	Expect("install no column policy", e.Install(), "true");

	// The guarantee is re-checked at execution: a statement prepared on a filtered_retrieval source is refused
	// once the same alias is re-attached as exact_isolated, with no request.
	Expect("prepare a keyword search to outlive its source",
	       RunSql(*e.user, "PREPARE regate AS " + Search("query => " + kKeyword + ", k => 2", "document_id", "")), "");
	Expect("detach the source", RunSql(*e.setup, "DETACH search_docs"), "");
	Expect("re-attach the alias as exact_isolated", e.Attach("search_docs", staged), "");
	Expect("install after the re-attach", e.Install(), "true");
	Backend().Reset();
	Contains("the prepared keyword search is refused at execution", RunSql(*e.user, "EXECUTE regate"), gate);
	Expect("and sends nothing", Calls(), "0");
	return Finish();
}
