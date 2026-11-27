// qod_search(..., request := '<json>') through the real node hooks: exactly one of query and request, the text
// cap, the profile gate, the guarded body that reaches Qdrant (F, C, k, declared payload), the point id
// pre-check under the policy (a hidden id answers as a missing one), field uses of the request's own payload
// keys and vectors (column policies refuse them), and an ATTACH's REQUEST_FIELDS.
#include "check.hpp"
#include "engine.hpp"
#include "fake_transport.hpp"

using namespace qod_qdrant_test;

namespace {
std::string Raw(const std::string &alias, const std::string &request, const std::string &extra = ", k => 2",
                const std::string &select = "document_id") {
	std::string doubled;
	for (auto c : request) {
		doubled += c == '\'' ? std::string("''") : std::string(1, c);
	}
	return "SELECT " + select + " FROM qod_search('" + alias + ".main.contracts', request => '" + doubled + "'" + extra +
	       ") ORDER BY 1";
}

std::string Hits(const std::string &points) {
	return "{\"result\":{\"points\":[" + points + "]},\"status\":\"ok\",\"time\":0.001}";
}

std::string Scrolled(const std::string &points) {
	return "{\"result\":{\"points\":[" + points + "],\"next_page_offset\":null},\"status\":\"ok\",\"time\":0.001}";
}

const std::string kHit1 = "{\"id\":1,\"version\":1,\"score\":1.0,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d1\","
                          "\"status\":\"active\",\"owner_id\":7}}";

std::string Call(size_t i, bool path = false) {
	std::lock_guard<std::mutex> lock(Backend().mutex);
	if (i >= Backend().calls.size()) {
		return "no call";
	}
	return path ? Backend().calls[i].path : Backend().calls[i].body;
}

const std::string kDoc = "{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"limit\":999,"
                         "\"with_payload\":true,\"with_vector\":true}";
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	InstallFakeTransport();
	Engine e;
	auto config = ReadFile(fixtures + "sample2_canonical.json");
	Expect("attach", e.Attach("search_docs", config), "");
	Expect("attach exact_isolated",
	       e.Attach("search_exact", Patch(config, "\"guarantee\":\"filtered_retrieval\"", "\"guarantee\":\"exact_isolated\""),
	                ", READ_ONLY", "qdrant:fs-qdrant-2"),
	       "");
	Expect("attach with REQUEST_FIELDS",
	       e.Attach("search_enabled", config, ", REQUEST_FIELDS '[\"params.acorn\"]', READ_ONLY", "qdrant:fs-qdrant-3"), "");
	Contains("REQUEST_FIELDS must be valid",
	         e.Attach("search_bad", config, ", REQUEST_FIELDS '[\"Bad\"]', READ_ONLY", "qdrant:fs-qdrant-4"),
	         "REQUEST_FIELDS must be a JSON array of request path patterns");
	e.handle.row_policies = {{"p0", "search_docs", "main", "contracts", "tenant_id = 'acme'"},
	                         {"p1", "search_enabled", "main", "contracts", "tenant_id = 'acme'"},
	                         {"p2", "search_exact", "main", "contracts", "tenant_id = 'acme'"}};
	Expect("install the tenant policy", e.Install(), "true");

	// Exactly one of query and request; the text cap; the profile gate. All before any request.
	Backend().Reset();
	Contains("RequestAndQueryTogetherRefused",
	         RunSql(*e.user, Raw("search_docs", kDoc, ", query => qod_bm25('body_bm25', 'x'), k => 2")),
	         "search_query_unsupported: pass exactly one of query or request");
	Contains("neither query nor request keeps the query shape refusal",
	         RunSql(*e.user, "SELECT document_id FROM qod_search('search_docs.main.contracts', k => 2)"),
	         "search_query_unsupported: query must be qod_dense, qod_bm25 or qod_rrf");
	Expect("lower the request text cap", RunSql(*e.setup, "SET GLOBAL search_max_request_text_bytes = 16"), "");
	Contains("RequestOverTextLimitRefused", RunSql(*e.user, Raw("search_docs", kDoc)),
	         "search_query_unsupported: request: must be at most 16 bytes");
	Expect("restore the request text cap", RunSql(*e.setup, "SET GLOBAL search_max_request_text_bytes = 65536"), "");
	Contains("request not JSON", RunSql(*e.user, Raw("search_docs", "{\"query\":")), "search_query_unsupported: request: ");
	Contains("RequestKeywordUnderExactIsolatedRefused", RunSql(*e.user, Raw("search_exact", kDoc)),
	         "search_query_unsupported: keyword search is not enabled for this source");
	Contains("an unknown field is refused", RunSql(*e.user, Raw("search_docs", "{\"query\":{\"text\":\"m\",\"model\":"
	                                                                           "\"qdrant/bm25\"},\"using\":\"body_bm25\","
	                                                                           "\"params\":{\"acorn\":{}}}")),
	         "search_query_unsupported: params.acorn: not supported");
	Expect("no request for any refusal", std::to_string(Backend().CallCount()), "0");

	// The guarded body: F, C on the keyword level, k, declared payload only, no vectors.
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("RequestSendsGuardedBody: rows", RunSql(*e.user, Raw("search_docs", kDoc)), "d1");
	auto body = Call(0);
	Contains("guarded body: the policy filter", body,
	         "\"filter\":{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}}]}");
	Contains("guarded body: the corpus", body,
	         "\"params\":{\"idf\":{\"corpus\":{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}}]}}}");
	Contains("guarded body: limit k", body, "\"limit\":2,");
	Contains("guarded body: declared payload only, no vectors", body, "\"with_vector\":false", "\"with_payload\":true");
	Expect("one request", std::to_string(Backend().CallCount()), "1");

	// An enabled field is copied; the same field on a source that did not enable it is refused.
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	Expect("enabled field accepted",
	       RunSql(*e.user, Raw("search_enabled", "{\"query\":{\"text\":\"m\",\"model\":\"qdrant/bm25\"},\"using\":"
	                                             "\"body_bm25\",\"params\":{\"acorn\":{\"enable\":true}}}")),
	       "d1");
	Contains("enabled field sent verbatim", Call(0), "\"acorn\":{\"enable\":true}");

	// Point ids: checked under the policy first; a hidden id and a missing one answer alike.
	Backend().Reset();
	Backend().handler = [](const FakeCall &c) {
		return c.path.find("/points/scroll") != std::string::npos ? Reply(200, Scrolled("")) : Reply(200, Hits(kHit1));
	};
	auto hidden = RunSql(*e.user, Raw("search_docs", "{\"query\":57,\"using\":\"body_bm25\"}"));
	auto missing = RunSql(*e.user, Raw("search_docs", "{\"query\":999,\"using\":\"body_bm25\"}"));
	Contains("HiddenAndMissingIdAnswerAlike: hidden", hidden, "search_query_unsupported: query: point not found");
	Expect("HiddenAndMissingIdAnswerAlike: same text", missing, hidden);
	Expect("only the pre-checks ran", std::to_string(Backend().CallCount()), "2");
	Contains("the pre-check is a scroll", Call(0, true), "/points/scroll");
	Contains("the pre-check carries the policy and the ids", Call(0),
	         "{\"filter\":{\"must\":[{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}}]},{\"has_id\":[57]}]},\"limit\":1,"
	         "\"with_payload\":false,\"with_vector\":false}");
	Backend().Reset();
	Backend().handler = [](const FakeCall &c) {
		return c.path.find("/points/scroll") != std::string::npos ? Reply(200, Scrolled("{\"id\":57,\"version\":1}"))
		                                                          : Reply(200, Hits(kHit1));
	};
	Expect("a visible id searches", RunSql(*e.user, Raw("search_docs", "{\"query\":57,\"using\":\"body_bm25\"}")), "d1");
	Expect("pre-check then search", std::to_string(Backend().CallCount()), "2");
	Backend().Reset();
	Backend().replies = {Reply(200, Hits(kHit1))};
	RunSql(*e.user, Raw("search_docs", kDoc));
	Expect("PrecheckSkippedWithoutIds", std::to_string(Backend().CallCount()), "1");

	// Field uses: the request's own payload keys and vectors meet core's column policies.
	e.handle.column_policies = {{"c1", "search_docs", "main", "contracts", "owner_id", "deny", ""}};
	Expect("install a column policy on owner_id", e.Install(), "true");
	Backend().Reset();
	Contains("FieldUsesIncludeFormulaKeys",
	         RunSql(*e.user, Raw("search_docs", "{\"prefetch\":{\"query\":{\"text\":\"m\",\"model\":\"qdrant/bm25\"},"
	                                            "\"using\":\"body_bm25\",\"limit\":10},\"query\":{\"formula\":{\"sum\":"
	                                            "[{\"mult\":[0,\"$score\"]},\"owner_id\"]}}}")),
	         "field_denied");
	Contains("order_by on a denied column", RunSql(*e.user, Raw("search_docs", "{\"query\":{\"order_by\":\"owner_id\"}}")),
	         "field_denied");
	e.handle.column_policies = {{"c2", "search_docs", "main", "contracts", "body", "mask", "'x'"}};
	Expect("install a mask on body", e.Install(), "true");
	Contains("a vector's lineage meets the mask", RunSql(*e.user, Raw("search_docs", kDoc)), "field_denied");
	Expect("no request for a denied field", std::to_string(Backend().CallCount()), "0");
	return Finish();
}
