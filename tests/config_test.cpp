// The node's config rules: the cases of Plan 2's QdrantConfigSpec and QdrantEndpointSpec, with the same
// messages, so the node refuses exactly what the manager refuses.
#include "ast/json.hpp"
#include "catalog/config.hpp"
#include "check.hpp"

#include <algorithm>
#include <cstdlib>

using namespace qod_qdrant;
using namespace qod_qdrant_test;

namespace {
std::string sample;
const std::set<std::string> kAllowlist {"https://qdrant.internal:6333"};
const std::string kFieldLine = "{ \"name\": \"owner_id\", \"path\": \"owner_id\", \"type\": \"BIGINT\", \"required\": true }";
const std::string kVectorLine = "{ \"name\": \"text_dense\", \"dimensions\": 768, \"datatype\": \"float32\",\n"
                                "          \"dependsOn\": [\"body\", \"title\"], \"modelVersion\": \"e5-large-v2@1\" }";
const std::string kArchive =
    "{\"table\":\"contracts\",\"schema\":\"archive\",\"collection\":\"contracts_old\",\"generation\":2,"
    "\"schemaVersion\":1,\"fields\":[{\"type\":\"VARCHAR\",\"required\":true,\"path\":\"tenant_id\",\"name\":\"tenant_id\"}],"
    "\"vectors\":[]}";

std::string sample2;
const std::string kSparseLines =
    "      \"sparseVectors\": [\n"
    "        { \"name\": \"body_bm25\", \"model\": \"qdrant/bm25\", \"dependsOn\": [\"body\"] },\n"
    "        { \"name\": \"title_bm25\", \"model\": \"qdrant/bm25\", \"dependsOn\": [\"title\"] }\n"
    "      ]";
const std::string kBodyEntry = "{ \"name\": \"body_bm25\", \"model\": \"qdrant/bm25\", \"dependsOn\": [\"body\"] }";
//! Plan 6's canonical form of slice 1's sample (contract section 1): guarantee and sparseVectors written.
const std::string kSampleSlice2Canonical =
    "{\"apiKeySecret\":\"QDRANT_API_KEY\",\"collections\":[{\"collection\":\"contracts_v3\",\"fields\":["
    "{\"name\":\"tenant_id\",\"path\":\"tenant_id\",\"required\":true,\"type\":\"VARCHAR\"},"
    "{\"name\":\"document_id\",\"path\":\"document_id\",\"required\":true,\"type\":\"VARCHAR\"},"
    "{\"name\":\"title\",\"path\":\"title\",\"required\":false,\"type\":\"VARCHAR\"},"
    "{\"name\":\"status\",\"path\":\"status\",\"required\":true,\"type\":\"VARCHAR\"},"
    "{\"name\":\"owner_id\",\"path\":\"owner_id\",\"required\":true,\"type\":\"BIGINT\"},"
    "{\"name\":\"body\",\"path\":\"body\",\"required\":false,\"type\":\"VARCHAR\"}],"
    "\"generation\":1,\"schema\":\"main\",\"schemaVersion\":1,\"sparseVectors\":[],\"table\":\"contracts\","
    "\"vectors\":[{\"datatype\":\"float32\","
    "\"dependsOn\":[\"body\",\"title\"],\"dimensions\":768,\"modelVersion\":\"e5-large-v2@1\",\"name\":\"text_dense\"}]}],"
    "\"endpoint\":\"https://qdrant.internal:6333\",\"guarantee\":\"exact_isolated\"}";

std::string P(const std::string &from, const std::string &to) {
	return Patch(sample, from, to);
}

std::string P2(const std::string &from, const std::string &to) {
	return Patch(sample2, from, to);
}

//! sample2 with its sparseVectors array replaced by `entries` (the text between the brackets).
std::string WithSparse(const std::string &entries) {
	return P2(kSparseLines, "      \"sparseVectors\": [" + entries + "]");
}

// The manager's canonical writer (QdrantCanonical.scala, slice 2 form), rebuilt here so the node's fixture is
// checked against the form Plan 6 renders: keys sorted, collections by (schema, table), declared order elsewhere,
// guarantee and sparseVectors always written. WriteJson escapes strings exactly as QdrantCanonical.str does.
JsonNode Sorted(std::vector<std::pair<std::string, JsonNode>> members) {
	std::sort(members.begin(), members.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	return JsonNode::Object(std::move(members));
}

//! `v` with object keys sorted at every depth, as QdrantCanonical writes a sparse vector's options.
JsonNode SortedDeep(const JsonNode &v) {
	if (v.Is(JsonNode::Kind::Object)) {
		std::vector<std::pair<std::string, JsonNode>> members;
		for (auto &m : v.members) {
			members.emplace_back(m.first, SortedDeep(m.second));
		}
		return Sorted(std::move(members));
	}
	if (v.Is(JsonNode::Kind::Array)) {
		std::vector<JsonNode> items;
		for (auto &i : v.items) {
			items.push_back(SortedDeep(i));
		}
		return JsonNode::Array(std::move(items));
	}
	return v;
}

JsonNode Names(const std::vector<std::string> &names) {
	std::vector<JsonNode> out;
	for (auto &n : names) {
		out.push_back(JsonNode::String(n));
	}
	return JsonNode::Array(std::move(out));
}

std::string Canonical(const SourceConfig &config) {
	auto collections = config.collections;
	std::sort(collections.begin(), collections.end(), [](const CollectionSpec &a, const CollectionSpec &b) {
		return std::make_pair(a.schema, a.table) < std::make_pair(b.schema, b.table);
	});
	std::vector<JsonNode> cs;
	for (auto &c : collections) {
		std::vector<JsonNode> fields;
		for (auto &f : c.fields) {
			fields.push_back(Sorted({{"name", JsonNode::String(f.name)},
			                         {"path", JsonNode::String(f.path)},
			                         {"type", JsonNode::String(FieldTypeSql(f.type))},
			                         {"required", JsonNode::Bool(f.required)}}));
		}
		std::vector<JsonNode> vectors;
		for (auto &v : c.vectors) {
			vectors.push_back(Sorted({{"name", JsonNode::String(v.name)},
			                          {"dimensions", JsonNode::Int(v.dimensions)},
			                          {"datatype", JsonNode::String(v.datatype)},
			                          {"dependsOn", Names(v.depends_on)},
			                          {"modelVersion", JsonNode::String(v.model_version)}}));
		}
		std::vector<JsonNode> sparse;
		for (auto &v : c.sparse_vectors) {
			sparse.push_back(Sorted({{"name", JsonNode::String(v.name)},
			                         {"model", JsonNode::String(v.model)},
			                         {"dependsOn", Names(v.depends_on)},
			                         {"options", SortedDeep(v.options)}}));
		}
		cs.push_back(Sorted({{"schema", JsonNode::String(c.schema)},
		                     {"table", JsonNode::String(c.table)},
		                     {"collection", JsonNode::String(c.collection)},
		                     {"generation", JsonNode::Int(c.generation)},
		                     {"schemaVersion", JsonNode::Int(c.schema_version)},
		                     {"fields", JsonNode::Array(std::move(fields))},
		                     {"vectors", JsonNode::Array(std::move(vectors))},
		                     {"sparseVectors", JsonNode::Array(std::move(sparse))}}));
	}
	return WriteJson(Sorted({{"endpoint", JsonNode::String(config.endpoint)},
	                         {"apiKeySecret", JsonNode::String(config.api_key_secret)},
	                         {"collections", JsonNode::Array(std::move(cs))},
	                         {"guarantee", JsonNode::String(config.guarantee)}}));
}

//! The errors of `raw` joined by " | ", or "accepted".
std::string Errs(const std::string &raw, const std::set<std::string> &allowlist = kAllowlist) {
	auto result = ParseConfig(raw, allowlist);
	if (result.config) {
		return result.errors.empty() ? "accepted" : "accepted with errors";
	}
	std::string out;
	for (auto &e : result.errors) {
		out += (out.empty() ? "" : " | ") + e;
	}
	return out;
}

std::string Coll(int i, int fields, int vectors) {
	std::string fs = "{\"name\":\"tenant_id\",\"path\":\"tenant_id\",\"type\":\"VARCHAR\",\"required\":true}";
	for (int j = 0; j < fields - 1; ++j) {
		fs += ",{\"name\":\"f" + std::to_string(j) + "\",\"path\":\"f" + std::to_string(j) +
		      "\",\"type\":\"VARCHAR\",\"required\":false}";
	}
	std::string vs;
	for (int j = 0; j < vectors; ++j) {
		vs += std::string(j ? "," : "") + "{\"name\":\"v" + std::to_string(j) +
		      "\",\"dimensions\":4,\"datatype\":\"float32\",\"dependsOn\":[\"tenant_id\"],\"modelVersion\":\"m\"}";
	}
	return "{\"schema\":\"s" + std::to_string(i) + "\",\"table\":\"t\",\"collection\":\"c\",\"generation\":1,"
	       "\"schemaVersion\":1,\"fields\":[" + fs +
	       "],\"vectors\":[" + vs + "]}";
}

std::string Config(int collections, int fields, int vectors) {
	std::string cs;
	for (int i = 0; i < collections; ++i) {
		cs += std::string(i ? "," : "") + Coll(i, fields, vectors);
	}
	return "{\"endpoint\":\"https://qdrant.internal:6333\",\"apiKeySecret\":\"K\",\"collections\":[" + cs + "]}";
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	sample = ReadFile(fixtures + "sample.json");

	auto parsed = ParseConfig(sample, kAllowlist);
	ExpectTrue("sample accepted", parsed.config.has_value() && parsed.errors.empty());
	auto &c = parsed.config->collections.at(0);
	Expect("sample collection", c.schema + "." + c.table + "->" + c.collection + " g" + std::to_string(c.generation) +
	                                " v" + std::to_string(c.schema_version),
	       "main.contracts->contracts_v3 g1 v1");
	std::string names;
	for (auto &f : c.fields) {
		names += f.name + ":" + FieldTypeSql(f.type) + (f.required ? "!" : "?") + " ";
	}
	Expect("fields in order", names,
	       "tenant_id:VARCHAR! document_id:VARCHAR! title:VARCHAR? status:VARCHAR! owner_id:BIGINT! body:VARCHAR? ");
	Expect("vector", c.vectors.at(0).name + " " + std::to_string(c.vectors[0].dimensions) + " " +
	                     c.vectors[0].depends_on.at(0) + "," + c.vectors[0].depends_on.at(1) + " " +
	                     c.vectors[0].model_version,
	       "text_dense 768 body,title e5-large-v2@1");
	Expect("canonical fixture accepted", Errs(ReadFile(fixtures + "sample_canonical.json")), "accepted");

	Expect("tenant is an unknown key",
	       Errs(P("\"schemaVersion\": 1,", "\"schemaVersion\": 1, \"tenant\": { \"field\": \"tenant_id\", \"value\": \"acme\" },")),
	       "collections[0]: unknown key \"tenant\"");
	Expect("tenant_id is an ordinary field: optional, BIGINT or left out",
	       Errs(P("{ \"name\": \"tenant_id\", \"path\": \"tenant_id\", \"type\": \"VARCHAR\", \"required\": true }",
	              "{ \"name\": \"tenant_id\", \"path\": \"tenant_id\", \"type\": \"BIGINT\", \"required\": false }")) + " " +
	           Errs(P("        { \"name\": \"tenant_id\", \"path\": \"tenant_id\", \"type\": \"VARCHAR\", \"required\": true },\n", "")),
	       "accepted accepted");
	Contains("duplicate key", Errs("{\"endpoint\": \"x\", \"endpoint\": \"y\"}"), "duplicate key");
	Expect("array root", Errs("[]"), "config: must be an object");
	Contains("not JSON", Errs("{"), "config is not valid JSON");
	Expect("too large", Errs("{" + std::string(kMaxConfigBytes, ' ') + "}"), "config is larger than 262144 bytes");

	Expect("unknown root key", Errs(P("\"apiKeySecret\": \"QDRANT_API_KEY\",", "\"apiKeySecret\": \"QDRANT_API_KEY\", \"region\": \"eu\",")),
	       "config: unknown key \"region\"");
	Expect("unknown field key", Errs(P(kFieldLine, Patch(kFieldLine, "\"required\": true", "\"required\": true, \"index\": 1"))),
	       "collections[0].fields[4]: unknown key \"index\"");
	Expect("missing key", Errs(P("\"schemaVersion\": 1,", "")), "collections[0]: missing key \"schemaVersion\"");
	for (auto k : {"apiKey", "api_key", "API-KEY", "token", "accessToken", "Password"}) {
		auto errs = Errs(P("\"apiKeySecret\": \"QDRANT_API_KEY\",",
		                   std::string("\"apiKeySecret\": \"QDRANT_API_KEY\", \"") + k + "\": \"hunter2\","));
		Contains(std::string("credential key ") + k, errs, "looks like a literal credential", "hunter2");
	}
	Expect("braces in modelVersion", Errs(P("e5-large-v2@1", "e5{{x")),
	       "collections[0].vectors[0].modelVersion: must not contain '{{' or '}}'");

	Contains("Kelvin sign is a K", Errs(P("\"apiKeySecret\": \"QDRANT_API_KEY\",",
	                                      "\"apiKeySecret\": \"QDRANT_API_KEY\", \"to\\u212Aen\": \"x\",")),
	         "looks like a literal credential");
	auto dup_tenant = [&](const std::string &second) {
		return P(kFieldLine, kFieldLine + ", " + second);
	};
	Expect("duplicate tenant_id field: the first declaration wins",
	       Errs(dup_tenant("{ \"name\": \"tenant_id\", \"path\": \"t2\", \"type\": \"VARCHAR\", \"required\": false }")),
	       "collections[0].fields: field names must be unique");
	Expect("duplicate tenant_id field, swapped type",
	       Errs(dup_tenant("{ \"name\": \"tenant_id\", \"path\": \"t2\", \"type\": \"BIGINT\", \"required\": true }")),
	       "collections[0].fields: field names must be unique");

	auto userinfo = Errs(P("https://qdrant.internal:6333", "https://u:pw@qdrant.internal:6333"));
	Contains("userinfo endpoint", userinfo, "endpoint: endpoint must be https://host[:port]", "pw@");
	Expect("endpoint outside the allowlist", Errs(P("https://qdrant.internal:6333", "https://other.internal:6333")),
	       "endpoint: is not in the deployment allowlist (QOD_QDRANT_ALLOWED_ENDPOINTS)");
	Contains("empty allowlist", Errs(sample, {}), "is empty or unset");

	for (auto k : {"qdrant_api_key", "1KEY", "KEY-1", ""}) {
		Expect(std::string("secret name ") + k, Errs(P("\"QDRANT_API_KEY\"", std::string("\"") + k + "\"")),
		       "apiKeySecret: must match [A-Z][A-Z0-9_]{0,63}");
	}
	Expect("secret name too long", Errs(P("\"QDRANT_API_KEY\"", "\"" + std::string(65, 'K') + "\"")),
	       "apiKeySecret: must match [A-Z][A-Z0-9_]{0,63}");
	Expect("longest secret name", Errs(P("\"QDRANT_API_KEY\"", "\"K" + std::string(63, '_') + "\"")), "accepted");

	for (auto n : {"Main", "_qod_x", "1main", "ma-in", ""}) {
		Expect(std::string("schema name ") + n, Errs(P("\"schema\": \"main\"", std::string("\"schema\": \"") + n + "\"")),
		       "collections[0].schema: must match [a-z][a-z0-9_]{0,62}");
	}
	Expect("schema too long", Errs(P("\"schema\": \"main\"", "\"schema\": \"" + std::string(64, 'm') + "\"")),
	       "collections[0].schema: must match [a-z][a-z0-9_]{0,62}");
	Expect("reserved table", Errs(P("\"table\": \"contracts\"", "\"table\": \"_qod_score\"")),
	       "collections[0].table: must match [a-z][a-z0-9_]{0,62}");
	Contains("reserved field", Errs(P("{ \"name\": \"body\",", "{ \"name\": \"_qod_id\",")),
	         "collections[0].fields[5].name: must match [a-z][a-z0-9_]{0,62}");

	auto archive_main = kArchive;
	archive_main.replace(archive_main.find("archive"), 7, "main");
	Expect("repeated (schema, table)", Errs(P("\"collections\": [", "\"collections\": [ " + archive_main + ",")),
	       "collections: (schema, table) must be unique per source");
	Expect("repeated field", Errs(P(kFieldLine, kFieldLine + ", " + kFieldLine)),
	       "collections[0].fields: field names must be unique");
	Expect("repeated vector", Errs(P(kVectorLine, kVectorLine + ", " + kVectorLine)),
	       "collections[0].vectors: vector names must be unique");
	Expect("two collections", Errs(P("\"collections\": [", "\"collections\": [ " + kArchive + ",")), "accepted");

	for (auto p : {"a..b", "a-b", "", ".a", "a.", "a b"}) {
		Expect(std::string("path ") + p, Errs(P("\"path\": \"owner_id\"", std::string("\"path\": \"") + p + "\"")),
		       "collections[0].fields[4].path: must match dot-separated [A-Za-z0-9_]+");
	}
	Expect("nested path", Errs(P("\"path\": \"owner_id\"", "\"path\": \"meta.Owner_1.id\"")), "accepted");
	for (auto t : {"INTEGER", "varchar", "TEXT"}) {
		Expect(std::string("type ") + t, Errs(P("\"type\": \"BIGINT\"", std::string("\"type\": \"") + t + "\"")),
		       "collections[0].fields[4].type: must be one of VARCHAR, BIGINT, BOOLEAN, DOUBLE");
	}
	Expect("DOUBLE field", Errs(P("\"type\": \"BIGINT\"", "\"type\": \"DOUBLE\"")), "accepted");
	Expect("BOOLEAN field", Errs(P("\"type\": \"BIGINT\"", "\"type\": \"BOOLEAN\"")), "accepted");
	Expect("required not a boolean", Errs(P(kFieldLine, Patch(kFieldLine, "\"required\": true", "\"required\": 1"))),
	       "collections[0].fields[4].required: must be true or false");

	// Plan 2's EdgeSpaceRule, which only modelVersion is subject to.
	for (auto padded : {" e5", "e5 ", "\\te5", "e5\\n", "\\u00a0e5", "e5\\u3000", "e5\\u0001", "\\u0000e5"}) {
		Expect(std::string("modelVersion with edge whitespace ") + padded,
		       Errs(P("\"e5-large-v2@1\"", std::string("\"") + padded + "\"")),
		       "collections[0].vectors[0].modelVersion: must not start or end with whitespace");
	}
	Expect("edge whitespace is judged before length", Errs(P("e5-large-v2@1", " " + std::string(200, 'm'))),
	       "collections[0].vectors[0].modelVersion: must not start or end with whitespace");
	Expect("inner whitespace is data", Errs(P("e5-large-v2@1", "e5 large")), "accepted");
	Expect("U+2007 is not Java whitespace", Errs(P("e5-large-v2@1", "e5\\u2007")), "accepted");
	Expect("modelVersion with edge whitespace", Errs(P("e5-large-v2@1", "e5-large-v2@1 ")),
	       "collections[0].vectors[0].modelVersion: must not start or end with whitespace");
	auto nul_model = ParseConfig(P("e5-large-v2@1", "a\\u0000x\\u001fb"), kAllowlist);
	ExpectTrue("inner control characters are data: an embedded NUL is kept, length-delimited",
	           nul_model.config &&
	               nul_model.config->collections[0].vectors[0].model_version == std::string("a\0x\x1f" "b", 5));

	Expect("generation 0", Errs(P("\"generation\": 1", "\"generation\": 0")),
	       "collections[0].generation: must be an integer in 1..9223372036854775807");
	Expect("schemaVersion -1", Errs(P("\"schemaVersion\": 1", "\"schemaVersion\": -1")),
	       "collections[0].schemaVersion: must be an integer in 1..9223372036854775807");
	Expect("generation 1.0", Errs(P("\"generation\": 1", "\"generation\": 1.0")), "collections[0].generation: must be an integer");
	Expect("generation text", Errs(P("\"generation\": 1", "\"generation\": \"1\"")), "collections[0].generation: must be an integer");

	Expect("dimensions 0", Errs(P("\"dimensions\": 768", "\"dimensions\": 0")),
	       "collections[0].vectors[0].dimensions: must be an integer in 1..65536");
	Expect("dimensions 65537", Errs(P("\"dimensions\": 768", "\"dimensions\": 65537")),
	       "collections[0].vectors[0].dimensions: must be an integer in 1..65536");
	Expect("dimensions 65536", Errs(P("\"dimensions\": 768", "\"dimensions\": 65536")), "accepted");
	Expect("datatype", Errs(P("\"float32\"", "\"float16\"")), "collections[0].vectors[0].datatype: must be float32");
	Expect("undeclared dependsOn", Errs(P("[\"body\", \"title\"]", "[\"body\", \"summary\"]")),
	       "collections[0].vectors[0].dependsOn[1]: must name a declared field");
	Expect("empty dependsOn", Errs(P("[\"body\", \"title\"]", "[]")), "collections[0].vectors[0].dependsOn: must hold 1..256 entries");
	Expect("repeated dependsOn", Errs(P("[\"body\", \"title\"]", "[\"body\", \"body\"]")),
	       "collections[0].vectors[0].dependsOn: dependsOn names must be unique");
	Expect("vector name", Errs(P("\"text_dense\"", "\"Text\"")), "collections[0].vectors[0].name: must match [a-z][a-z0-9_]{0,62}");
	Expect("modelVersion 129", Errs(P("e5-large-v2@1", std::string(129, 'm'))),
	       "collections[0].vectors[0].modelVersion: must hold 1..128 characters");
	Expect("modelVersion empty", Errs(P("\"e5-large-v2@1\"", "\"\"")),
	       "collections[0].vectors[0].modelVersion: must hold 1..128 characters");
	std::string emoji;
	for (int i = 0; i < 128; ++i) {
		emoji += "\xf0\x9f\x98\x80";
	}
	Expect("128 code points", Errs(P("e5-large-v2@1", emoji)), "accepted");
	auto scan_only = ParseConfig(P(kVectorLine, ""), kAllowlist);
	ExpectTrue("scan-only collection", scan_only.config && scan_only.config->collections[0].vectors.empty());

	Expect("collection name", Errs(P("contracts_v3", "contracts/v3")),
	       "collections[0].collection: must match [A-Za-z0-9_-]{1,255}");
	Contains("empty collections", Errs(P("\"collections\": [", "\"collections\": [], \"x\": [")),
	         "collections: must hold 1..64 entries");
	Expect("64 collections", Errs(Config(64, 1, 0)), "accepted");
	Expect("65 collections", Errs(Config(65, 1, 0)), "collections: must hold 1..64 entries");
	Expect("256 fields, 8 vectors", Errs(Config(1, 256, 8)), "accepted");
	Expect("257 fields", Errs(Config(1, 257, 0)),
	       "collections[0].fields: must hold 1..256 entries");
	Expect("9 vectors", Errs(Config(1, 1, 9)), "collections[0].vectors: must hold 0..8 entries");
	Expect("every rule broken is reported",
	       Errs(Patch(P("\"generation\": 1", "\"generation\": 0"), "\"float32\"", "\"float16\"")),
	       "collections[0].generation: must be an integer in 1..9223372036854775807 | "
	       "collections[0].vectors[0].datatype: must be float32");

	for (auto e : {"https://qdrant.internal:6333", "https://qdrant", "https://10.0.0.7:443", "https://a-b.example.com:65535",
	               "https://x.y.z:1"}) {
		ExpectTrue(std::string("origin ") + e, !EndpointSyntaxError(e).has_value());
	}
	for (auto e : {"http://qdrant.internal:6333", "https://qdrant.internal:6333/", "https://qdrant.internal:6333/collections",
	               "https://qdrant.internal:6333?x=1", "https://qdrant.internal:6333#f", "https://user:pw@qdrant.internal:6333",
	               "https://Qdrant.Internal:6333", "https://qdrant.internal:0", "https://qdrant.internal:06333",
	               "https://qdrant.internal:", "https://-qdrant.internal", "https://qdrant..internal", "https://[::1]:6333",
	               " https://qdrant.internal:6333", "https://qdrant.internal:6333\n", "HTTPS://qdrant.internal", ""}) {
		ExpectTrue(std::string("not an origin: ") + e, EndpointSyntaxError(e).has_value());
	}
	Expect("port above 65535", EndpointSyntaxError("https://qdrant.internal:65536").value_or(""), "endpoint port must be 1..65535");
	Expect("longer than any origin", EndpointSyntaxError("https://" + std::string(260, 'a')).value_or(""),
	       "endpoint is longer than any valid origin");
	Expect("length in UTF-16 units, as Plan 2 counts", EndpointSyntaxError("https://" + std::string(258, 'a') + "\xf0\x9f\x98\x80").value_or(""),
	       "endpoint is longer than any valid origin");
	Expect("267 characters is still judged by the origin rule",
	       EndpointSyntaxError("https://" + std::string(253, 'a') + ":65535").value_or(""),
	       "endpoint must be https://host[:port] with a lowercase host and no path, query, fragment or userinfo");
	std::string long_host = std::string(63, 'a') + "." + std::string(63, 'a') + "." + std::string(63, 'a') + "." + std::string(62, 'a');
	ExpectTrue("long host is 254 characters", long_host.size() == 254);
	Expect("host above 253", EndpointSyntaxError("https://" + long_host).value_or(""), "endpoint host is longer than 253 characters");
	Contains("endpoint error never repeats userinfo", EndpointSyntaxError("https://user:s3cr3t@qdrant.internal").value_or(""),
	         "endpoint", "s3cr3t");
	auto allow = ParseAllowlist(" https://a.example , http://bad.example, ,https://b.example:8443,https://a.example");
	Expect("allowlist origins", std::to_string(allow.first.size()) + " " + *allow.first.begin() + " " +
	                                *allow.first.rbegin() + " dropped " + std::to_string(allow.second),
	       "2 https://a.example https://b.example:8443 dropped 1");
	ExpectTrue("unset allowlist", ParseAllowlist(nullptr).first.empty() && ParseAllowlist(nullptr).second == 0);
	ExpectTrue("blank allowlist", ParseAllowlist("  ").first.empty() && ParseAllowlist("  ").second == 0);
	ExpectTrue("the any-origin entry cannot come from the variable",
	           ParseAllowlist("*").first.empty() && ParseAllowlist("*").second == 1 &&
	               ParseAllowlist("https://a.example,*").first.count(kAnyHttpsOrigin) == 0);
	Contains("the QoD host still refuses every endpoint with the variable unset",
	         ParseConfig(sample, ParseAllowlist(nullptr).first).errors.empty() ? std::string("accepted")
	                                                                           : ParseConfig(sample, ParseAllowlist(nullptr).first).errors[0],
	         "is empty or unset, so no endpoint is accepted");
	sample2 = ReadFile(fixtures + "sample2.json");
	auto parsed2 = ParseConfig(sample2, kAllowlist);
	ExpectTrue("sample2 accepted", parsed2.config.has_value() && parsed2.errors.empty());
	if (parsed2.config) {
		auto &c2 = parsed2.config->collections.at(0);
		std::string sparse;
		for (auto &v : c2.sparse_vectors) {
			sparse += v.name + ":" + v.model + ":" + v.depends_on.at(0) + " ";
		}
		Expect("sparse vectors in declared order", sparse, "body_bm25:qdrant/bm25:body title_bm25:qdrant/bm25:title ");
		Expect("guarantee", parsed2.config->guarantee, "filtered_retrieval");
		ExpectTrue("filtered_retrieval allows lexical", parsed2.config->AllowsLexical());
		ExpectTrue("sparse lookup", c2.FindSparseVector("title_bm25") && !c2.FindSparseVector("text_dense"));
		ExpectTrue("dense lookup never finds a sparse name", c2.FindVector("text_dense") && !c2.FindVector("body_bm25"));
		Expect("sample2_canonical.json is the manager's canonical form of sample2.json", Canonical(*parsed2.config),
		       ReadFile(fixtures + "sample2_canonical.json"));
	}
	auto canonical2 = ParseConfig(ReadFile(fixtures + "sample2_canonical.json"), kAllowlist);
	ExpectTrue("sample2 canonical accepted", canonical2.config && canonical2.errors.empty());
	ExpectTrue("slice 1 form: exact_isolated, no sparse vectors",
	           parsed.config->guarantee == "exact_isolated" && !parsed.config->AllowsLexical() &&
	               parsed.config->collections.at(0).sparse_vectors.empty());
	Expect("slice 2 canonical form of slice 1's sample", Canonical(*parsed.config), kSampleSlice2Canonical);
	Expect("the node accepts the slice 2 canonical form of slice 1's sample", Errs(kSampleSlice2Canonical), "accepted");
	auto staging = ParseConfig(P2("  \"guarantee\": \"filtered_retrieval\",\n", ""), kAllowlist);
	ExpectTrue("sparse vectors under exact_isolated are valid (staging)",
	           staging.config && !staging.config->AllowsLexical() && staging.config->collections[0].sparse_vectors.size() == 2);

	Expect("explicit exact_isolated", Errs(P2("\"filtered_retrieval\"", "\"exact_isolated\"")), "accepted");
	Expect("guarantee not a string", Errs(P2("\"filtered_retrieval\"", "7")), "guarantee: must be a string");
	Expect("guarantee null", Errs(P2("\"filtered_retrieval\"", "null")), "guarantee: must be a string");
	Expect("guarantee unknown", Errs(P2("\"filtered_retrieval\"", "\"isolated\"")),
	       "guarantee: must be exact_isolated or filtered_retrieval");
	Expect("guarantee is case-sensitive", Errs(P2("\"filtered_retrieval\"", "\"Filtered_Retrieval\"")),
	       "guarantee: must be exact_isolated or filtered_retrieval");
	Expect("guarantee with braces", Errs(P2("\"filtered_retrieval\"", "\"{{x}}\"")), "guarantee: must not contain '{{' or '}}'");
	Expect("root order: endpoint, apiKeySecret, guarantee, collections",
	       Errs(Patch(Patch(Patch(P2("\"filtered_retrieval\"", "1"), "\"QDRANT_API_KEY\"", "\"k\""), "\"generation\": 1",
	                        "\"generation\": 0"),
	                  "https://qdrant.internal:6333", "https://other.internal:6333")),
	       "endpoint: is not in the deployment allowlist (QOD_QDRANT_ALLOWED_ENDPOINTS) | "
	       "apiKeySecret: must match [A-Z][A-Z0-9_]{0,63} | guarantee: must be a string | "
	       "collections[0].generation: must be an integer in 1..9223372036854775807");

	Expect("sparseVectors not an array", Errs(P2(kSparseLines, "      \"sparseVectors\": {}")),
	       "collections[0].sparseVectors: must be an array");
	Expect("empty sparseVectors", Errs(WithSparse("")), "accepted");
	std::string eight;
	std::string nine;
	for (int i = 0; i < 9; ++i) {
		auto entry = std::string(i ? "," : "") + "{\"name\":\"s" + std::to_string(i) + "\",\"model\":\"qdrant/bm25\",\"dependsOn\":[\"body\"]}";
		nine += entry;
		if (i < 8) {
			eight += entry;
		}
	}
	Expect("8 sparse vectors", Errs(WithSparse(eight)), "accepted");
	Expect("9 sparse vectors", Errs(WithSparse(nine)), "collections[0].sparseVectors: must hold 0..8 entries");
	// Sparse vector options: Qdrant's BM25 text-processing options, every key allowed, kept as sent.
	auto body_with = [](const std::string &options) {
		return P2(kBodyEntry, "{ \"name\": \"body_bm25\", \"model\": \"qdrant/bm25\", \"dependsOn\": [\"body\"], "
		                      "\"options\": " + options + " }");
	};
	auto with_options = ParseConfig(body_with("{\"language\":\"french\",\"stemmer\":{\"type\":\"none\"}}"), kAllowlist);
	ExpectTrue("sparse options accepted", with_options.config.has_value() && with_options.errors.empty());
	if (with_options.config) {
		auto &c = with_options.config->collections.at(0);
		Expect("sparse options kept as sent", WriteJson(c.FindSparseVector("body_bm25")->options),
		       "{\"language\":\"french\",\"stemmer\":{\"type\":\"none\"}}");
		Expect("absent sparse options are an empty object", WriteJson(c.FindSparseVector("title_bm25")->options), "{}");
	}
	Expect("sparse options not an object", Errs(body_with("[]")), "collections[0].sparseVectors[0].options: must be an object");
	std::string eight_deep = "1", nine_deep;
	for (int i = 0; i < 8; i++) {
		eight_deep = "{\"a\":" + eight_deep + "}";
	}
	nine_deep = "{\"a\":" + eight_deep + "}";
	Expect("sparse options 8 levels deep", Errs(body_with(eight_deep)), "accepted");
	Expect("sparse options 9 levels deep", Errs(body_with(nine_deep)),
	       "collections[0].sparseVectors[0].options: must nest at most 8 levels");
	Expect("sparse option value with braces", Errs(body_with("{\"language\":\"{{x}}\"}")),
	       "collections[0].sparseVectors[0].options.language: must not contain '{{' or '}}'");
	Expect("sparse option key with braces", Errs(body_with("{\"{{k\":1}")),
	       "collections[0].sparseVectors[0].options: a key must not contain '{{' or '}}'");
	Expect("sparse entry not an object", Errs(WithSparse("1")), "collections[0].sparseVectors[0]: must be an object");
	Expect("unknown sparse key", Errs(WithSparse(Patch(kBodyEntry, "\"dependsOn\"", "\"analyzer\": \"en\", \"dependsOn\""))),
	       "collections[0].sparseVectors[0]: unknown key \"analyzer\"");
	Contains("credential-like sparse key",
	         Errs(WithSparse(Patch(kBodyEntry, "\"dependsOn\"", "\"apiKey\": \"hunter2\", \"dependsOn\""))),
	         "collections[0].sparseVectors[0]: key \"apiKey\" looks like a literal credential", "hunter2");
	Expect("missing sparse key", Errs(WithSparse(Patch(kBodyEntry, ", \"dependsOn\": [\"body\"]", ""))),
	       "collections[0].sparseVectors[0]: missing key \"dependsOn\"");
	Expect("sparse name", Errs(WithSparse(Patch(kBodyEntry, "\"body_bm25\"", "\"Body\""))),
	       "collections[0].sparseVectors[0].name: must match [a-z][a-z0-9_]{0,62}");
	Expect("sparse model", Errs(WithSparse(Patch(kBodyEntry, "\"qdrant/bm25\"", "\"qdrant/bm42\""))),
	       "collections[0].sparseVectors[0].model: must be qdrant/bm25");
	Expect("sparse model not a string", Errs(WithSparse(Patch(kBodyEntry, "\"qdrant/bm25\"", "25"))),
	       "collections[0].sparseVectors[0].model: must be a string");
	Expect("sparse dependsOn empty", Errs(WithSparse(Patch(kBodyEntry, "[\"body\"]", "[]"))),
	       "collections[0].sparseVectors[0].dependsOn: must hold 1..256 entries");
	Expect("sparse dependsOn undeclared", Errs(WithSparse(Patch(kBodyEntry, "[\"body\"]", "[\"summary\"]"))),
	       "collections[0].sparseVectors[0].dependsOn[0]: must name a declared field");
	Expect("sparse dependsOn repeated", Errs(WithSparse(Patch(kBodyEntry, "[\"body\"]", "[\"body\", \"body\"]"))),
	       "collections[0].sparseVectors[0].dependsOn: dependsOn names must be unique");
	Expect("sparse names unique", Errs(WithSparse(kBodyEntry + ", " + kBodyEntry)),
	       "collections[0].sparseVectors: sparse vector names must be unique");
	Expect("sparse name equal to a dense name", Errs(WithSparse(Patch(kBodyEntry, "\"body_bm25\"", "\"text_dense\""))),
	       "collections[0].sparseVectors: names must differ from the dense vector names");
	auto entry = [](const std::string &name, const std::string &model, const std::string &deps) {
		return "{ \"name\": \"" + name + "\", \"model\": \"" + model + "\", \"dependsOn\": [" + deps + "] }";
	};
	Expect("collection order: slice 1 rules, each sparse entry (name, model, dependsOn), uniqueness, clash",
	       Errs(Patch(WithSparse(entry("Bad", "x", "\"body\"") + ", " + entry("title_bm25", "qdrant/bm25", "\"nope\"") + ", " +
	                             entry("text_dense", "qdrant/bm25", "\"body\"") + ", " +
	                             entry("text_dense", "qdrant/bm25", "\"title\"")),
	                  "\"generation\": 1", "\"generation\": 0")),
	       "collections[0].generation: must be an integer in 1..9223372036854775807 | "
	       "collections[0].sparseVectors[0].name: must match [a-z][a-z0-9_]{0,62} | "
	       "collections[0].sparseVectors[0].model: must be qdrant/bm25 | "
	       "collections[0].sparseVectors[1].dependsOn[0]: must name a declared field | "
	       "collections[0].sparseVectors: sparse vector names must be unique | "
	       "collections[0].sparseVectors: names must differ from the dense vector names");
	return Finish();
}
