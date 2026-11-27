// Request JSON is exact and deterministic; responses are read strictly (duplicate and unknown keys refused);
// payloads are type checked against the registry; rows carry _qod_id and _qod_score.
#include "arrow/payload.hpp"
#include "ast/request.hpp"
#include "catalog/config.hpp"
#include "check.hpp"
#include "duckdb.hpp"
#include "lower/filter_expr.hpp"

#include <functional>

using namespace qod_qdrant;
using namespace qod_qdrant_test;

namespace {
CollectionSpec spec;

FieldValue Text(const std::string &s) {
	FieldValue v;
	v.type = FieldType::Varchar;
	v.text = s;
	return v;
}

FieldValue Integer(int64_t n) {
	FieldValue v;
	v.type = FieldType::Bigint;
	v.integer = n;
	return v;
}

//! The member `key` of `object` written back as JSON, or "absent".
std::string MemberJson(const JsonNode &object, const std::string &key) {
	auto member = object.Find(key);
	return member ? WriteJson(*member) : "absent";
}

std::string ResponseRefusal(const std::string &body, bool scroll = true) {
	try {
		if (scroll) {
			ParseScrollResponse(body);
		} else {
			ParseQueryResponse(body);
		}
	} catch (ResponseError &e) {
		return std::string("refused: ") + e.what();
	}
	return "accepted";
}

std::string DecodeRefusal(const std::string &payload, const std::set<size_t> &fields = {0, 3, 4}) {
	try {
		QdrantPoint p;
		p.id = JsonNode::Int(1);
		p.payload = ParseJson(payload, kResponseJsonLimits);
		DecodePoint(p, spec, fields);
	} catch (ResponseError &e) {
		return std::string("refused: ") + e.what();
	}
	return "accepted";
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	auto rich = Patch(ReadFile(fixtures + "sample.json"),
	                  "{ \"name\": \"title\", \"path\": \"title\", \"type\": \"VARCHAR\", \"required\": false }",
	                  "{ \"name\": \"title\", \"path\": \"meta.title\", \"type\": \"DOUBLE\", \"required\": false }, "
	                  "{ \"name\": \"published\", \"path\": \"meta.published\", \"type\": \"BOOLEAN\", \"required\": false }");
	// tenant_id 0, document_id 1, title 2 (DOUBLE, meta.title), published 3, status 4, owner_id 5, body 6
	spec = ParseConfig(rich, {"https://qdrant.internal:6333"}).config->collections.at(0);

	// A row policy on the ordinary field tenant_id (`tenant_id = 'acme'`): what separates tenants now.
	auto tenant_policy = FilterExpr::Match(0, Text("acme"));
	auto filter = Simplify(FilterExpr::And({tenant_policy, FilterExpr::Match(4, Text("active"))}));
	ScrollRequest scroll {filter, 256, std::nullopt, IncludePaths(spec, {0, 4, 5})};
	Expect("scroll path", ScrollPath(spec), "/collections/contracts_v3/points/scroll");
	Expect("query path", QueryPath(spec), "/collections/contracts_v3/points/query");
	Expect("scroll request", ScrollRequestJson(scroll, spec),
	       "{\"filter\":{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}},{\"key\":\"status\",\"match\":"
	       "{\"value\":\"active\"}}]},\"limit\":256,\"with_payload\":{\"include\":[\"tenant_id\",\"status\",\"owner_id\"]},"
	       "\"with_vector\":false}");
	scroll.offset = JsonNode::Uint(18446744073709551615ULL);
	scroll.include = {};
	Expect("scroll with u64 offset and no payload", ScrollRequestJson(scroll, spec),
	       "{\"filter\":{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}},{\"key\":\"status\",\"match\":"
	       "{\"value\":\"active\"}}]},\"limit\":256,\"offset\":18446744073709551615,\"with_payload\":false,\"with_vector\":false}");
	ScrollRequest whole {FilterExpr::True(), 256, std::nullopt, IncludePaths(spec, {1})};
	Expect("scroll without policy or filter: no filter key", ScrollRequestJson(whole, spec),
	       "{\"limit\":256,\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
	Contains("a FALSE filter is never written", ErrorOf([&] {
		         ScrollRequestJson({FilterExpr::False(), 256, std::nullopt, {}}, spec);
	         }),
	         "FALSE");
	Expect("include paths keep field order, nested paths whole", IncludePaths(spec, {5, 2, 0}).size() == 3
	                                                                 ? IncludePaths(spec, {5, 2, 0})[1]
	                                                                 : "wrong size",
	       "meta.title");
	QueryRequest query {{0.5f, -1.0f, 0.1f}, "text_dense", tenant_policy, 10, IncludePaths(spec, {1})};
	Expect("query request", QueryRequestJson(query, spec),
	       "{\"query\":[0.5,-1,0.100000001],\"using\":\"text_dense\",\"filter\":{\"must\":[{\"key\":\"tenant_id\",\"match\":"
	       "{\"value\":\"acme\"}}]},\"limit\":10,\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
	query.filter = FilterExpr::True();
	Expect("query request without policy or filter: no filter key", QueryRequestJson(query, spec),
	       "{\"query\":[0.5,-1,0.100000001],\"using\":\"text_dense\",\"limit\":10,"
	       "\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");

	HybridStage text_stage {true, {}, "merger", "body_bm25"};
	HybridStage dense_stage {false, {0.5f, -1.0f}, "", "text_dense"};
	HybridStage title_stage {true, {}, "deal", "title_bm25"};
	{
		// Slice 2: F = P AND U, C = P (the spike record's placements). P is a row policy on the ordinary field
		// tenant_id and on owner_id; it lowers into both the filter and the corpus.
		auto p = Simplify(FilterExpr::And({tenant_policy, FilterExpr::In(5, {Integer(7), Integer(8)})}));
		auto u = FilterExpr::Match(4, Text("active"));
		auto f = Simplify(FilterExpr::And({p, u}));
		auto c = Simplify(p);
		const std::string kF = "{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}},{\"key\":\"owner_id\","
		                       "\"match\":{\"any\":[7,8]}},{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]}";
		const std::string kC = "{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}},{\"key\":\"owner_id\","
		                       "\"match\":{\"any\":[7,8]}}]}";
		KeywordRequest keyword {"merger \"agreement\"\n", "body_bm25", f, c, 10, IncludePaths(spec, {1})};
		Expect("keyword request", KeywordRequestJson(keyword, spec),
		       "{\"query\":{\"text\":\"merger \\\"agreement\\\"\\n\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"filter\":" +
		           kF + ",\"params\":{\"idf\":{\"corpus\":" + kC +
		           "}},\"limit\":10,\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
		HybridRequest hybrid {{text_stage, dense_stage}, f, c, 100, 10, {}};
		Expect("hybrid request", HybridRequestJson(hybrid, spec),
		       "{\"prefetch\":[{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"filter\":" + kF +
		           ",\"params\":{\"idf\":{\"corpus\":" + kC + "}},\"limit\":100},{\"query\":[0.5,-1],\"using\":\"text_dense\","
		           "\"filter\":" + kF + ",\"limit\":100}],\"query\":{\"rrf\":{}},\"filter\":" + kF +
		           ",\"limit\":10,\"with_payload\":false,\"with_vector\":false}");
		// The isolation invariant, read back from the JSON: the corpus is inside every keyword stage and equals
		// P without U; the top level of a hybrid request never carries params; every stage carries F.
		ExpectTrue("C excludes U", kC.find("\"status\"") == std::string::npos && kF.find("\"status\"") != std::string::npos);
		auto sent_keyword = ParseJson(KeywordRequestJson(keyword, spec), kResponseJsonLimits);
		Expect("keyword: the corpus at the top level is C", MemberJson(*sent_keyword.Find("params")->Find("idf"), "corpus"), kC);
		Expect("keyword: the filter is F", MemberJson(sent_keyword, "filter"), kF);
		int set = 0;
		for (auto stages : {std::vector<HybridStage> {text_stage, dense_stage}, std::vector<HybridStage> {dense_stage, text_stage, title_stage},
		                    std::vector<HybridStage> {title_stage, text_stage}}) {
			HybridRequest r {stages, f, c, 50, 5, IncludePaths(spec, {0, 1})};
			auto sent = ParseJson(HybridRequestJson(r, spec), kResponseJsonLimits);
			auto label = "hybrid set " + std::to_string(++set);
			Expect(label + ": no top-level params", MemberJson(sent, "params"), "absent");
			Expect(label + ": top-level filter is F", MemberJson(sent, "filter"), kF);
			auto &prefetch = sent.Find("prefetch")->items;
			Expect(label + ": one prefetch per stage", std::to_string(prefetch.size()), std::to_string(stages.size()));
			for (size_t i = 0; i < prefetch.size(); ++i) {
				auto where = label + " prefetch " + std::to_string(i);
				Expect(where + ": filter is F", MemberJson(prefetch[i], "filter"), kF);
				Expect(where + ": limit is prefetch_k", MemberJson(prefetch[i], "limit"), "50");
				if (stages[i].keyword) {
					Expect(where + ": keyword stage carries C", MemberJson(*prefetch[i].Find("params")->Find("idf"), "corpus"), kC);
				} else {
					Expect(where + ": dense stage carries no params", MemberJson(prefetch[i], "params"), "absent");
				}
			}
		}
	}
	{
		// No row policy: C is TRUE and the statistics cover the whole collection. The node leaves `params` out
		// of every keyword stage (the spike's "no corpus" shape), never a match-all corpus; F is U alone.
		auto u = FilterExpr::Match(4, Text("active"));
		const std::string kU = "{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]}";
		KeywordRequest keyword {"merger", "body_bm25", u, FilterExpr::True(), 10, IncludePaths(spec, {1})};
		Expect("keyword request, no policy: no params", KeywordRequestJson(keyword, spec),
		       "{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"filter\":" + kU +
		           ",\"limit\":10,\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
		keyword.filter = FilterExpr::True();
		Expect("keyword request, no policy and no filter_sql: neither filter nor params", KeywordRequestJson(keyword, spec),
		       "{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"limit\":10,"
		       "\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
		HybridRequest hybrid {{text_stage, dense_stage, title_stage}, u, FilterExpr::True(), 100, 10, {}};
		Expect("hybrid request, no policy: no keyword prefetch carries params", HybridRequestJson(hybrid, spec),
		       "{\"prefetch\":[{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"filter\":" + kU +
		           ",\"limit\":100},{\"query\":[0.5,-1],\"using\":\"text_dense\",\"filter\":" + kU +
		           ",\"limit\":100},{\"query\":{\"text\":\"deal\",\"model\":\"qdrant/bm25\"},\"using\":\"title_bm25\",\"filter\":" +
		           kU + ",\"limit\":100}],\"query\":{\"rrf\":{}},\"filter\":" + kU +
		           ",\"limit\":10,\"with_payload\":false,\"with_vector\":false}");
		hybrid.filter = FilterExpr::True();
		Expect("hybrid request, no policy and no filter_sql: no filter, no params anywhere", HybridRequestJson(hybrid, spec),
		       "{\"prefetch\":[{\"query\":{\"text\":\"merger\",\"model\":\"qdrant/bm25\"},\"using\":\"body_bm25\",\"limit\":100},"
		       "{\"query\":[0.5,-1],\"using\":\"text_dense\",\"limit\":100},{\"query\":{\"text\":\"deal\",\"model\":"
		       "\"qdrant/bm25\"},\"using\":\"title_bm25\",\"limit\":100}],\"query\":{\"rrf\":{}},\"limit\":10,"
		       "\"with_payload\":false,\"with_vector\":false}");
		Contains("a FALSE corpus is never written", ErrorOf([&] {
			         KeywordRequestJson({"merger", "body_bm25", u, FilterExpr::False(), 10, {}}, spec);
		         }),
		         "FALSE");
	}

	auto page = ParseScrollResponse(
	    "{\"result\":{\"points\":[{\"id\":1,\"payload\":{\"status\":\"active\"}},"
	    "{\"id\":\"6F1C3F2E-0000-4000-8000-000000000003\",\"payload\":{}}],\"next_page_offset\":2},\"status\":\"ok\","
	    "\"time\":0.002,\"usage\":null}");
	Expect("scroll points", std::to_string(page.points.size()) + " next " + page.next_offset->text, "2 next 2");
	Expect("last page", ParseScrollResponse("{\"result\":{\"points\":[],\"next_page_offset\":null},\"status\":\"ok\",\"time\":0}")
	                        .next_offset.has_value()
	                        ? "more"
	                        : "done",
	       "done");
	auto hits = ParseQueryResponse("{\"result\":{\"points\":[{\"id\":7,\"version\":3,\"score\":0.875,\"payload\":{}}]},"
	                               "\"status\":\"ok\",\"time\":0.001}");
	Expect("query score", std::to_string(hits.at(0).score.value_or(-1)), "0.875000");
	Contains("duplicate key", ResponseRefusal("{\"result\":{\"points\":[]},\"result\":{},\"status\":\"ok\"}"), "duplicate key");
	Contains("unknown top-level key", ResponseRefusal("{\"result\":{\"points\":[]},\"status\":\"ok\",\"extra\":1}"),
	         "unknown key \"extra\"");
	Contains("status not ok", ResponseRefusal("{\"result\":{\"points\":[]},\"status\":\"error\"}"), "status");
	Contains("missing result", ResponseRefusal("{\"status\":\"ok\"}"), "result");
	Contains("points not an array", ResponseRefusal("{\"result\":{\"points\":{}},\"status\":\"ok\"}"), "points");
	Contains("unknown point key", ResponseRefusal("{\"result\":{\"points\":[{\"id\":1,\"weird\":1}]},\"status\":\"ok\"}"),
	         "unknown key \"weird\"");
	Contains("fractional id", ResponseRefusal("{\"result\":{\"points\":[{\"id\":1.5}]},\"status\":\"ok\"}"), "id");
	Contains("negative id", ResponseRefusal("{\"result\":{\"points\":[{\"id\":-1}]},\"status\":\"ok\"}"), "id");
	Contains("bad next offset", ResponseRefusal("{\"result\":{\"points\":[],\"next_page_offset\":{}},\"status\":\"ok\"}"),
	         "next_page_offset");
	Contains("scroll keys in a query result", ResponseRefusal("{\"result\":{\"points\":[],\"next_page_offset\":null},\"status\":\"ok\"}", false),
	         "unknown key \"next_page_offset\"");
	Contains("query point without score", ResponseRefusal("{\"result\":{\"points\":[{\"id\":1}]},\"status\":\"ok\"}", false),
	         "score");
	Contains("not JSON", ResponseRefusal("<html>"), "not valid JSON");
	Contains("id neither u64 nor uuid", ResponseRefusal("{\"result\":{\"points\":[{\"id\":\"abc\"}]},\"status\":\"ok\"}"),
	         "id", "abc");
	Contains("uuid without hyphens", ResponseRefusal("{\"result\":{\"points\":[{\"id\":\"6f1c3f2e000040008000000000000003\"}]},"
	                                                 "\"status\":\"ok\"}"),
	         "id");
	Contains("boolean id", ResponseRefusal("{\"result\":{\"points\":[{\"id\":true}]},\"status\":\"ok\"}"), "id");
	Contains("u64 overflow id", ResponseRefusal("{\"result\":{\"points\":[{\"id\":18446744073709551616}]},\"status\":\"ok\"}"),
	         "id");
	Contains("next offset not a uuid",
	         ResponseRefusal("{\"result\":{\"points\":[],\"next_page_offset\":\"page-2\"},\"status\":\"ok\"}"),
	         "next_page_offset", "page-2");
	Expect("uuid next offset", ParseScrollResponse("{\"result\":{\"points\":[],\"next_page_offset\":"
	                                               "\"6f1c3f2e-0000-4000-8000-000000000004\"},\"status\":\"ok\"}")
	                               .next_offset->text,
	       "6f1c3f2e-0000-4000-8000-000000000004");
	Contains("infinite score", ResponseRefusal("{\"result\":{\"points\":[{\"id\":1,\"score\":1e999}]},\"status\":\"ok\"}", false),
	         "score");
	Contains("payload an array", ResponseRefusal("{\"result\":{\"points\":[{\"id\":1,\"payload\":[]}]},\"status\":\"ok\"}"),
	         "payload");
	// 7 values around the usage array: top, result, points, next_page_offset, status, time, usage.
	auto values = [](size_t zeros) {
		std::string body = "{\"result\":{\"points\":[],\"next_page_offset\":null},\"status\":\"ok\",\"time\":0,\"usage\":[";
		for (size_t i = 0; i < zeros; ++i) {
			body += i ? ",0" : "0";
		}
		return body + "]}";
	};
	Expect("value budget: exactly the cap", ResponseRefusal(values(kResponseJsonLimits.max_nodes - 7)), "accepted");
	auto budget = [&](const std::string &body) -> std::string {
		try {
			ParseScrollResponse(body);
		} catch (ResponseBudgetError &e) {
			return std::string("budget: ") + e.what();
		} catch (ResponseError &e) {
			return std::string("schema: ") + e.what();
		}
		return "accepted";
	};
	Contains("value budget: one more is a budget error, not a schema error", budget(values(kResponseJsonLimits.max_nodes - 6)),
	         "budget: response exceeds the parse budget: more than 1048576 values at offset", "not valid JSON");
	Contains("malformed JSON stays a schema error", budget("{\"result\":"), "schema: response is not valid JSON");
	Contains("scroll result without next_page_offset", ResponseRefusal("{\"result\":{\"points\":[]},\"status\":\"ok\"}"),
	         "next_page_offset is missing");
	auto moved = ParseScrollResponse("{\"result\":{\"points\":[{\"id\":3,\"payload\":{\"status\":\"x\"}}],"
	                                 "\"next_page_offset\":null},\"status\":\"ok\"}");
	Expect("payload carried out of the parsed tree", WriteJson(moved.points.at(0).payload) + " " + moved.points.at(0).id.text,
	       "{\"status\":\"x\"} 3");

	Expect("u64 id", FormatPointId(JsonNode::Uint(18446744073709551615ULL)), "18446744073709551615");
	Expect("uuid id lowercased", FormatPointId(JsonNode::String("6F1C3F2E-0000-4000-8000-000000000003")),
	       "6f1c3f2e-0000-4000-8000-000000000003");
	Contains("not a uuid", ErrorOf([] { FormatPointId(JsonNode::String("not-a-uuid")); }), "id");

	QdrantPoint p;
	p.id = JsonNode::Int(42);
	p.score = 0.5;
	p.payload = ParseJson("{\"tenant_id\":\"acme\",\"status\":\"active\",\"owner_id\":9223372036854775807,"
	                      "\"meta\":{\"title\":3,\"published\":true}}",
	                      kResponseJsonLimits);
	auto row = DecodePoint(p, spec, {0, 2, 3, 4, 5, 6});
	Expect("decoded", row.id + " " + row.values[0]->text + " " + std::to_string(row.values[2]->real) + " " +
	                      (row.values[3]->boolean ? "true" : "false") + " " + row.values[4]->text + " " +
	                      std::to_string(row.values[5]->integer) + " body:" + (row.values[6] ? "set" : "null"),
	       "42 acme 3.000000 true active 9223372036854775807 body:null");
	ExpectTrue("undecoded field stays unset", !row.values[1].has_value());
	Expect("payload ok", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\"}"), "accepted");
	Contains("required missing", DecodeRefusal("{\"tenant_id\":\"acme\"}"), "status");
	Contains("required BIGINT key absent", DecodeRefusal("{\"tenant_id\":\"acme\"}", {5}), "required field owner_id is missing");
	Contains("required field with no payload at all", DecodeRefusal("null", {5}), "required field owner_id is missing");
	Contains("required null", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":null}"), "status");
	Contains("wrong type", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":5}"), "status");
	Contains("array value", DecodeRefusal("{\"tenant_id\":[\"acme\",\"bob\"],\"status\":\"x\"}"), "tenant_id");
	Contains("BOOLEAN as a string", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\",\"meta\":{\"published\":\"true\"}}"),
	         "published");
	Expect("optional absent under a missing parent", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\",\"meta\":null}"),
	       "accepted");
	Expect("optional absent", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\",\"meta\":{}}"), "accepted");
	Contains("array on the path", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\",\"meta\":[{\"published\":true}]}"),
	         "published");
	Contains("scalar on the path", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\",\"meta\":5}"), "published");
	Contains("wrong type never shows the value", DecodeRefusal("{\"tenant_id\":\"acme\",\"status\":\"x\",\"meta\":{\"published\":"
	                                                           "\"secret-value\"}}"),
	         "published", "secret-value");
	Contains("BIGINT beyond int64", DecodeRefusal("{\"owner_id\":9223372036854775808}", {5}), "owner_id",
	         "9223372036854775808");
	Expect("BIGINT int64 min", DecodeRefusal("{\"owner_id\":-9223372036854775808}", {5}), "accepted");
	Contains("BIGINT as a decimal", DecodeRefusal("{\"owner_id\":1.0}", {5}), "owner_id");
	Contains("BIGINT with an exponent", DecodeRefusal("{\"owner_id\":1e3}", {5}), "owner_id");
	Contains("BIGINT as a string", DecodeRefusal("{\"owner_id\":\"1\"}", {5}), "owner_id");
	Expect("DOUBLE decimal", DecodeRefusal("{\"meta\":{\"title\":-2.5e-3}}", {2}), "accepted");
	Contains("DOUBLE beyond finite", DecodeRefusal("{\"meta\":{\"title\":1e999}}", {2}), "title");
	Contains("DOUBLE as a string", DecodeRefusal("{\"meta\":{\"title\":\"3\"}}", {2}), "title");
	Contains("VARCHAR as an object", DecodeRefusal("{\"tenant_id\":{},\"status\":\"x\"}"), "tenant_id");

	duckdb::DataChunk chunk;
	duckdb::vector<duckdb::LogicalType> types {duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
	                                           duckdb::LogicalType::VARCHAR, duckdb::LogicalType::DOUBLE,
	                                           duckdb::LogicalType::VARCHAR};
	chunk.Initialize(duckdb::Allocator::DefaultAllocator(), types);
	EmitRow(row, {4, 5, IdColumn(), ScoreColumn(), 6}, spec, chunk, 0);
	chunk.SetChildCardinality(1);
	Expect("emitted row", chunk.GetValue(0, 0).ToString() + "|" + chunk.GetValue(1, 0).ToString() + "|" +
	                          chunk.GetValue(2, 0).ToString() + "|" + chunk.GetValue(3, 0).ToString() + "|" +
	                          (chunk.GetValue(4, 0).IsNull() ? "NULL" : "value"),
	       "active|9223372036854775807|42|0.5|NULL");
	auto invalid_input = [](const std::function<void()> &f) -> std::string {
		try {
			f();
		} catch (duckdb::InvalidInputException &e) {
			return duckdb::ErrorData(e).RawMessage();
		} catch (std::exception &e) {
			return std::string("other exception: ") + e.what();
		}
		return "no error";
	};
	Contains("unknown scan column is a query error, never INTERNAL",
	         invalid_input([&] { EmitRow(row, {7}, spec, chunk, 0); }), "search_query_unsupported: ");
	Expect("scan columns checked up front", invalid_input([] { CheckScanColumns({4, 5, IdColumn(), ScoreColumn(), 6}, spec); }),
	       "no error");
	Contains("unknown scan column refused up front", invalid_input([] { CheckScanColumns({0, 7}, spec); }),
	         "search_query_unsupported: ");
	Contains("row id column refused up front",
	         invalid_input([] { CheckScanColumns({duckdb::COLUMN_IDENTIFIER_ROW_ID}, spec); }), "search_query_unsupported: ");

	QdrantPoint nul;
	nul.id = JsonNode::Int(1);
	nul.payload = ParseJson("{\"status\":\"a\\u0000b\"}", kResponseJsonLimits);
	duckdb::DataChunk text_chunk;
	text_chunk.Initialize(duckdb::Allocator::DefaultAllocator(), duckdb::vector<duckdb::LogicalType> {duckdb::LogicalType::VARCHAR});
	EmitRow(DecodePoint(nul, spec, {4}), {4}, spec, text_chunk, 0);
	text_chunk.SetChildCardinality(1);
	auto emitted = duckdb::StringValue::Get(text_chunk.GetValue(0, 0));
	Expect("embedded NUL kept", std::to_string(emitted.size()) + (emitted == std::string("a\0b", 3) ? " exact" : " altered"),
	       "3 exact");
	auto virtuals = VirtualColumns();
	Expect("virtual columns", virtuals.at(IdColumn()).name.GetIdentifierName() + ":" +
	                              virtuals.at(IdColumn()).type.ToString() + "," +
	                              virtuals.at(ScoreColumn()).name.GetIdentifierName() + ":" +
	                              virtuals.at(ScoreColumn()).type.ToString(),
	       "_qod_id:VARCHAR,_qod_score:DOUBLE");
	{
		// Sparse vector options: copied verbatim into query.options of every BM25 text query on that vector.
		auto with_options = spec;
		SparseVectorSpec body {"body_bm25", "qdrant/bm25", {"body"}, ParseJson("{\"language\":\"french\"}", kConfigJsonLimits)};
		SparseVectorSpec title {"title_bm25", "qdrant/bm25", {"title"}, JsonNode::Object({})};
		with_options.sparse_vectors = {body, title};
		KeywordRequest keyword {"cartes", "body_bm25", FilterExpr::True(), FilterExpr::True(), 10, {}};
		Expect("keyword request carries the vector's options", KeywordRequestJson(keyword, with_options),
		       "{\"query\":{\"text\":\"cartes\",\"model\":\"qdrant/bm25\",\"options\":{\"language\":\"french\"}},"
		       "\"using\":\"body_bm25\",\"limit\":10,\"with_payload\":false,\"with_vector\":false}");
		keyword.using_vector = "title_bm25";
		Expect("keyword request without options is unchanged", KeywordRequestJson(keyword, with_options),
		       "{\"query\":{\"text\":\"cartes\",\"model\":\"qdrant/bm25\"},\"using\":\"title_bm25\",\"limit\":10,"
		       "\"with_payload\":false,\"with_vector\":false}");
		HybridStage fr {true, {}, "cartes", "body_bm25"};
		HybridStage dense {false, {0.5f}, "", "text_dense"};
		HybridRequest hybrid {{fr, dense}, FilterExpr::True(), FilterExpr::True(), 20, 5, {}};
		Expect("hybrid keyword stage carries the options, the dense stage does not", HybridRequestJson(hybrid, with_options),
		       "{\"prefetch\":[{\"query\":{\"text\":\"cartes\",\"model\":\"qdrant/bm25\",\"options\":{\"language\":"
		       "\"french\"}},\"using\":\"body_bm25\",\"limit\":20},{\"query\":[0.5],\"using\":\"text_dense\",\"limit\":20}],"
		       "\"query\":{\"rrf\":{}},\"limit\":5,\"with_payload\":false,\"with_vector\":false}");
	}
	return Finish();
}
