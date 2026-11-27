// The guard of raw Qdrant requests (qod_search(request := ...)): what it refuses at bind, what it reports to core
// (filter fields, point ids, keyword), and the body it writes once the row policy is known: F on every level,
// C as the corpus of keyword levels only, a Document's declared options, limit k, declared payload only.
#include "ast/json.hpp"
#include "catalog/config.hpp"
#include "check.hpp"
#include "duckdb.hpp"
#include "lower/filter_expr.hpp"
#include "lower/request_guard.hpp"

using namespace qod_qdrant;
using namespace qod_qdrant_test;

namespace {
CollectionSpec spec;
SearchLimits limits;

FieldValue Text(const std::string &s) {
	FieldValue v;
	v.type = FieldType::Varchar;
	v.text = s;
	return v;
}

JsonNode J(const std::string &text) {
	return ParseJson(text, kResponseJsonLimits);
}

CheckedRequest Check(const std::string &request, const std::vector<std::string> &enabled = {},
                     const CollectionSpec &s = spec) {
	return CheckRequest(J(request), s, enabled, limits);
}

std::string Refusal(const std::string &request, const std::vector<std::string> &enabled = {}) {
	return ErrorOf([&] { Check(request, enabled); });
}

// tenant_id 0, document_id 1, title 2, status 3, owner_id 4, body 5 (sample2.json); text_dense has 2 dimensions.
const FilterExpr kF = FilterExpr::Match(0, Text("acme"));
const FilterExpr kC = FilterExpr::Match(0, Text("acme"));

JsonNode Written(const std::string &request, const CollectionSpec &s = spec, const std::vector<std::string> &enabled = {},
                 const std::set<size_t> &selected = {1}, int64_t k = 5) {
	auto checked = Check(request, enabled, s);
	return J(WriteRequest(checked, s, kF, kC, selected, k).body);
}

std::string At(const JsonNode &v, const std::vector<std::string> &path) {
	const JsonNode *n = &v;
	for (auto &p : path) {
		if (n->Is(JsonNode::Kind::Array)) {
			n = &n->items.at(std::stoul(p));
		} else {
			n = n->Find(p);
		}
		if (!n) {
			return "absent";
		}
	}
	return WriteJson(*n);
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	auto config = Patch(ReadFile(fixtures + "sample2.json"), "\"dimensions\": 768", "\"dimensions\": 2");
	spec = ParseConfig(config, {"https://qdrant.internal:6333"}).config->collections.at(0);
	auto kFJson = WriteJson(FilterJson(kF, spec));
	auto kCJson = WriteJson(FilterJson(kC, spec));

	// F on every level, the user's filter ANDed under it.
	auto deep = Written("{\"prefetch\":[{\"prefetch\":{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"limit\":5},"
	                    "\"query\":{\"fusion\":\"rrf\"},\"limit\":5}],\"query\":{\"fusion\":\"rrf\"}}");
	Expect("FilterIsWrappedOnEveryLevel: top", At(deep, {"filter"}), kFJson);
	Expect("FilterIsWrappedOnEveryLevel: level 1", At(deep, {"prefetch", "0", "filter"}), kFJson);
	Expect("FilterIsWrappedOnEveryLevel: level 2", At(deep, {"prefetch", "0", "prefetch", "filter"}), kFJson);
	auto anded = Written("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"key\":\"status\","
	                     "\"match\":{\"value\":\"active\"}}]}}");
	Expect("UserFilterIsAnded", At(anded, {"filter"}),
	       "{\"must\":[" + kFJson + ",{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]}]}");
	auto status_use = Check("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"key\":\"status\","
	                        "\"match\":{\"value\":\"active\"}}]}}");
	ExpectTrue("a user filter key is a filter field use", status_use.filter_fields == std::set<size_t> {3});
	Contains("UndeclaredFilterKeyIsDenied",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"key\":\"salary\","
	                 "\"match\":{\"value\":1}}]}}"),
	         "search_field_denied: filter.must[0].key: names no declared field", "salary\"");
	Contains("nested filter refused",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"nested\":{\"key\":\"x\","
	                 "\"filter\":{}}}]}}"),
	         "filter.must[0].nested: not supported");

	// Owned and refused fields.
	Contains("IdfFromUserIsRefused", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"idf\":{}}}"),
	         "params.idf: set by qod-search");
	Contains("LookupFromRefused",
	         Refusal("{\"query\":9,\"lookup_from\":{\"collection\":\"hr_private\"}}"), "lookup_from: not supported");
	Contains("ShardKeyRefused", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"shard_key\":\"a\"}"),
	         "shard_key: not supported");
	Contains("OffsetRefused", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"offset\":10}"),
	         "offset: not supported");
	Contains("ExactRefused", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"exact\":true}}"),
	         "params.exact: must be false");
	Contains("hnsw_ef capped", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"hnsw_ef\":513}}"),
	         "params.hnsw_ef: must be an integer in 1..512");
	auto owned = Written("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"limit\":999,\"with_payload\":true,"
	                     "\"with_vector\":true}");
	Expect("LimitIsK", At(owned, {"limit"}), "5");
	Expect("PayloadAndVectorOverwritten: payload", At(owned, {"with_payload"}), "{\"include\":[\"document_id\"]}");
	Expect("PayloadAndVectorOverwritten: vector", At(owned, {"with_vector"}), "false");
	Expect("no payload selected", At(Written("{\"query\":[0.1,0.2],\"using\":\"text_dense\"}", spec, {}, {}), {"with_payload"}),
	       "false");

	// Keyword stages: C as their corpus, never elsewhere.
	auto hybrid = Written("{\"prefetch\":[{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\","
	                      "\"limit\":20},{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"limit\":20}],\"query\":{\"rrf\":{\"k\":60,"
	                      "\"weights\":[2,1]}}}");
	Expect("CorpusOnKeywordStagesOnly: keyword stage", At(hybrid, {"prefetch", "0", "params"}),
	       "{\"idf\":{\"corpus\":" + kCJson + "}}");
	Expect("CorpusOnKeywordStagesOnly: dense stage", At(hybrid, {"prefetch", "1", "params"}), "absent");
	Expect("CorpusOnKeywordStagesOnly: top", At(hybrid, {"params"}), "absent");
	Expect("rrf k and weights pass", At(hybrid, {"query"}), "{\"rrf\":{\"k\":60,\"weights\":[2,1]}}");
	ExpectTrue("Bm25DocumentMarksKeyword",
	           Check("{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\"}").keyword);
	ExpectTrue("a dense request is not keyword", !Check("{\"query\":[0.1,0.2],\"using\":\"text_dense\"}").keyword);
	Contains("OtherModelRefused",
	         Refusal("{\"query\":{\"text\":\"x\",\"model\":\"sentence-transformers/all-minilm-l6-v2\"},\"using\":\"body_bm25\"}"),
	         "query.model: must be qdrant/bm25");
	Contains("a Document on a dense vector refused",
	         Refusal("{\"query\":{\"text\":\"x\",\"model\":\"qdrant/bm25\"},\"using\":\"text_dense\"}"),
	         "using: must name a declared sparse vector");

	// Declared options of the using vector always win.
	auto french = spec;
	for (auto &v : french.sparse_vectors) {
		if (v.name == "body_bm25") {
			v.options = J("{\"language\":\"french\"}");
		}
	}
	auto doc = "{\"query\":{\"text\":\"cartes\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\"}";
	Expect("Bm25DocumentGetsDeclaredOptions", At(Written(doc, french), {"query", "options"}), "{\"language\":\"french\"}");
	Contains("Bm25DocumentDifferentDeclaredKeyRefused",
	         ErrorOf([&] {
		         Check("{\"query\":{\"text\":\"c\",\"model\":\"qdrant/bm25\",\"options\":{\"language\":\"english\"}},"
		               "\"using\":\"body_bm25\"}",
		               {}, french);
	         }),
	         "query.options.language: differs from the index's declared options");
	Expect("Bm25DocumentExtraKeyKept",
	       At(Written("{\"query\":{\"text\":\"c\",\"model\":\"qdrant/bm25\",\"options\":{\"k\":1.5}},\"using\":\"body_bm25\"}",
	                  french),
	          {"query", "options"}),
	       "{\"k\":1.5,\"language\":\"french\"}");

	// Point ids are collected for the pre-check, whatever query kind names them.
	auto ids = [](const std::string &request) {
		std::string out;
		for (auto &id : Check(request).point_ids) {
			out += WriteJson(id) + " ";
		}
		return out;
	};
	Expect("PointIdsCollected: query", ids("{\"query\":57,\"using\":\"text_dense\"}"), "57 ");
	Expect("PointIdsCollected: nearest", ids("{\"query\":{\"nearest\":58},\"using\":\"text_dense\"}"), "58 ");
	Expect("PointIdsCollected: recommend",
	       ids("{\"query\":{\"recommend\":{\"positive\":[1,\"0b4f1d5e-1c2a-4f6b-9a7d-2e3c4b5a6d7e\"],\"negative\":[[0.1,0.2]]}},"
	           "\"using\":\"text_dense\"}"),
	       "1 \"0b4f1d5e-1c2a-4f6b-9a7d-2e3c4b5a6d7e\" ");
	Expect("PointIdsCollected: discover",
	       ids("{\"query\":{\"discover\":{\"target\":7,\"context\":[{\"positive\":8,\"negative\":9}]}},\"using\":\"text_dense\"}"), "7 8 9 ");
	Expect("PointIdsCollected: context", ids("{\"query\":{\"context\":[{\"positive\":10,\"negative\":11}]},\"using\":\"text_dense\"}"), "10 11 ");
	Expect("PointIdsCollected: none for a vector", ids("{\"query\":[0.1,0.2],\"using\":\"text_dense\"}"), "");
	Contains("too many ids", Refusal("{\"using\":\"text_dense\",\"query\":{\"recommend\":{\"positive\":[" + [] {
		         std::string s;
		         for (int i = 1; i <= 65; i++) {
			         s += (i > 1 ? "," : "") + std::to_string(i);
		         }
		         return s;
	         }() + "]}}}"),
	         "query: names more than 64 point ids");

	// Payload keys read by order_by and formula are filter uses; undeclared ones are denied.
	ExpectTrue("OrderByAndFormulaKeysReported: order_by",
	           Check("{\"query\":{\"order_by\":\"owner_id\"}}").filter_fields == std::set<size_t> {4});
	ExpectTrue("OrderByAndFormulaKeysReported: order_by object",
	           Check("{\"query\":{\"order_by\":{\"key\":\"owner_id\",\"direction\":\"desc\"}}}").filter_fields ==
	               std::set<size_t> {4});
	auto formula = "{\"prefetch\":{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"limit\":10},\"query\":{\"formula\":"
	               "{\"sum\":[{\"mult\":[0,\"$score\"]},\"owner_id\"]}}}";
	ExpectTrue("OrderByAndFormulaKeysReported: formula variable", Check(formula).filter_fields == std::set<size_t> {4});
	Contains("a formula variable that is no declared field is denied",
	         Refusal("{\"query\":{\"formula\":{\"sum\":[\"$score\",\"salary\"]}}}"),
	         "search_field_denied: query.formula.sum[1]: names no declared field");
	Contains("formula defaults name declared fields only",
	         Refusal("{\"query\":{\"formula\":\"$score\",\"defaults\":{\"salary\":0}}}"),
	         "search_field_denied: query.defaults.salary: names no declared field");
	ExpectTrue("formula datetime_key and decay",
	           Check("{\"query\":{\"formula\":{\"exp_decay\":{\"x\":{\"datetime_key\":\"title\"},\"target\":{\"datetime\":"
	                 "\"2026-11-27T00:00:00Z\"},\"scale\":86400}}}}")
	                   .filter_fields == std::set<size_t> {2});
	ExpectTrue("formula condition key",
	           Check("{\"query\":{\"formula\":{\"mult\":[\"$score\",{\"key\":\"status\",\"match\":{\"value\":\"x\"}}]}}}")
	                   .filter_fields == std::set<size_t> {3});
	Contains("an unknown formula operator is refused", Refusal("{\"query\":{\"formula\":{\"newop\":[1]}}}"),
	         "query.formula.newop: not supported");

	// Unknown fields: refused unless the tenant enabled their path; then copied verbatim.
	Contains("UnknownKeyRefused: params",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"acorn\":{\"enable\":true}}}"),
	         "params.acorn: not supported");
	Contains("UnknownKeyRefused: top", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"zzz\":1}"),
	         "zzz: not supported");
	Contains("UnknownKeyRefused: query kind", Refusal("{\"query\":{\"newkind\":1}}"), "query.newkind: not supported");
	auto enabled = Written("{\"prefetch\":{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"acorn\":{\"enable\":true}}},"
	                       "\"query\":{\"fusion\":\"rrf\"}}",
	                       spec, {"params.acorn"});
	Expect("EnabledKeyCopied, in a prefetch level too", At(enabled, {"prefetch", "params", "acorn"}), "{\"enable\":true}");
	Expect("an enabled query kind is copied", At(Written("{\"query\":{\"newkind\":1}}", spec, {"query.newkind"}), {"query"}),
	       "{\"newkind\":1}");

	// Vectors and caps.
	Contains("using an undeclared vector", Refusal("{\"query\":[0.1,0.2],\"using\":\"nope\"}"),
	         "using: must name a declared vector");
	Contains("dimensions checked", Refusal("{\"query\":[0.1,0.2,0.3],\"using\":\"text_dense\"}"),
	         "query: must hold 2 finite numbers");
	Contains("depth capped",
	         Refusal("{\"prefetch\":{\"prefetch\":{\"prefetch\":{\"query\":[0.1,0.2],\"using\":\"text_dense\"}}},"
	                 "\"query\":{\"fusion\":\"rrf\"}}"),
	         "prefetch: at most 2 levels");
	Contains("branches capped",
	         Refusal("{\"prefetch\":[{\"query\":[0.1,0.2],\"using\":\"text_dense\"},{\"query\":[0.1,0.2],\"using\":\"text_dense\"},"
	                 "{\"query\":[0.1,0.2],\"using\":\"text_dense\"},{\"query\":[0.1,0.2],\"using\":\"text_dense\"},"
	                 "{\"query\":[0.1,0.2],\"using\":\"text_dense\"}],\"query\":{\"fusion\":\"rrf\"}}"),
	         "prefetch: at most 4 branches");
	Contains("prefetch limit capped",
	         Refusal("{\"prefetch\":{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"limit\":100000},\"query\":{\"fusion\":\"rrf\"}}"),
	         "prefetch.limit: must be an integer in 1..1000");
	Contains("not an object", Refusal("[1]"), "request: must be a JSON object");

	// Review fix 1: a condition is a closed shape, so no operator can hide beside it (Qdrant 1.20.2 then reads the
	// object as the operator and the score carries the payload value).
	auto formula_of = [](const std::string &expr) {
		return "{\"prefetch\":{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"limit\":10},\"query\":{\"formula\":"
		       "{\"sum\":[{\"mult\":[0,\"$score\"]}," + expr + "]}}}";
	};
	Contains("a field condition with an operator beside it is refused",
	         Refusal(formula_of("{\"key\":\"status\",\"match\":0,\"mult\":[1,\"owner_id\"]}")),
	         "search_query_unsupported: query.formula.sum[1]");
	Contains("a field condition needs an object match",
	         Refusal(formula_of("{\"key\":\"status\",\"match\":0}")), "query.formula.sum[1].match: must be an object");
	Contains("has_id with an operator beside it is refused",
	         Refusal(formula_of("{\"has_id\":\"x\",\"mult\":[1,\"owner_id\"]}")), "query.formula.sum[1]");
	Contains("has_vector with an operator beside it is refused",
	         Refusal(formula_of("{\"has_vector\":5,\"mult\":[1,\"owner_id\"]}")), "query.formula.sum[1]");
	Contains("a filter field condition with an extra member is refused",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"key\":\"status\","
	                 "\"match\":{\"value\":\"a\"},\"mult\":[1]}]}}"),
	         "filter.must[0].mult: not supported");
	Contains("has_id holds point ids", Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":"
	                                           "[{\"has_id\":[\"x\"]}]}}"),
	         "filter.must[0].has_id: must be an array of point ids");
	Contains("has_vector names a declared vector",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"has_vector\":\"nope\"}]}}"),
	         "filter.must[0].has_vector: must name a declared vector");
	ExpectTrue("has_vector reports the vector for its lineage",
	           Check("{\"query\":{\"order_by\":\"owner_id\"},\"filter\":{\"must\":[{\"has_vector\":\"body_bm25\"}]}}")
	                   .vectors.count("body_bm25") == 1);
	ExpectTrue("a well-formed field condition still passes",
	           Check(formula_of("{\"key\":\"status\",\"match\":{\"value\":\"x\"}}")).filter_fields == std::set<size_t> {3});

	// Review fix 2: a query that uses a vector space names a declared vector, never Qdrant's unnamed default.
	Contains("an id query needs using", Refusal("{\"query\":1}"), "using: must name a declared vector for this query");
	Contains("recommend needs using", Refusal("{\"query\":{\"recommend\":{\"positive\":[1]}}}"),
	         "using: must name a declared vector for this query");
	Contains("lookup_from is refused before anything else", Refusal("{\"query\":9,\"lookup_from\":{\"collection\":\"x\"}}"),
	         "lookup_from: not supported");
	ExpectTrue("formula, fusion and order_by need no using",
	           !Check("{\"query\":{\"order_by\":\"owner_id\"}}").keyword &&
	               Check("{\"query\":{\"formula\":\"$score\"}}").point_ids.empty());

	// Review fix 3: quantization is closed and capped.
	Contains("oversampling capped",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"quantization\":{\"rescore\":true,"
	                 "\"oversampling\":100000}}}"),
	         "params.quantization.oversampling: must be a number in 1..8");
	Contains("quantization members closed",
	         Refusal("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"quantization\":{\"zzz\":1}}}"),
	         "params.quantization.zzz: not supported");
	ExpectTrue("a reasonable quantization passes",
	           !Check("{\"query\":[0.1,0.2],\"using\":\"text_dense\",\"params\":{\"quantization\":{\"ignore\":false,"
	                  "\"rescore\":true,\"oversampling\":2.5}}}")
	                .keyword);

	// Review fix 5: the declared options reach a document at every input position.
	Expect("declared options in recommend.positive",
	       At(Written("{\"query\":{\"recommend\":{\"positive\":[{\"text\":\"c\",\"model\":\"qdrant/bm25\"}]}},"
	                  "\"using\":\"body_bm25\"}",
	                  french),
	          {"query", "recommend", "positive", "0", "options"}),
	       "{\"language\":\"french\"}");
	Expect("declared options in discover.context",
	       At(Written("{\"query\":{\"discover\":{\"target\":{\"text\":\"a\",\"model\":\"qdrant/bm25\"},\"context\":"
	                  "[{\"positive\":{\"text\":\"b\",\"model\":\"qdrant/bm25\"},\"negative\":1}]}},\"using\":\"body_bm25\"}",
	                  french),
	          {"query", "discover", "context", "0", "positive", "options"}),
	       "{\"language\":\"french\"}");

	// REQUEST_FIELDS of an ATTACH.
	ExpectTrue("patterns", IsRequestFieldPattern("params.acorn") && !IsRequestFieldPattern("Params") &&
	                           !IsRequestFieldPattern("a.b.c.d.e.f") && !IsRequestFieldPattern(""));
	ExpectTrue("REQUEST_FIELDS read", ParseRequestFields("[\"params.acorn\",\"query.sample\"]") ==
	                                      std::vector<std::string> {"params.acorn", "query.sample"});
	Contains("REQUEST_FIELDS not an array", ErrorOf([] { ParseRequestFields("{}"); }),
	         "REQUEST_FIELDS must be a JSON array of request path patterns");
	Contains("REQUEST_FIELDS bad pattern", ErrorOf([] { ParseRequestFields("[\"Bad\"]"); }),
	         "REQUEST_FIELDS must be a JSON array of request path patterns");
	return Finish();
}
