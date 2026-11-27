#include "catalog/config.hpp"

#include "ast/json.hpp"

#include <algorithm>
#include <map>

namespace qod_qdrant {

const char *FieldTypeSql(FieldType type) {
	switch (type) {
	case FieldType::Varchar:
		return "VARCHAR";
	case FieldType::Bigint:
		return "BIGINT";
	case FieldType::Boolean:
		return "BOOLEAN";
	case FieldType::Double:
		return "DOUBLE";
	}
	return "VARCHAR";
}

int CollectionSpec::FieldIndex(const std::string &name) const {
	for (size_t i = 0; i < fields.size(); ++i) {
		if (fields[i].name == name) {
			return int(i);
		}
	}
	return -1;
}

const VectorSpec *CollectionSpec::FindVector(const std::string &name) const {
	for (auto &v : vectors) {
		if (v.name == name) {
			return &v;
		}
	}
	return nullptr;
}

const SparseVectorSpec *CollectionSpec::FindSparseVector(const std::string &name) const {
	for (auto &v : sparse_vectors) {
		if (v.name == name) {
			return &v;
		}
	}
	return nullptr;
}

bool SourceConfig::AllowsLexical() const {
	return guarantee == kFilteredRetrieval;
}

namespace {

bool Lower(char c) {
	return c >= 'a' && c <= 'z';
}
bool Upper(char c) {
	return c >= 'A' && c <= 'Z';
}
bool Digit(char c) {
	return c >= '0' && c <= '9';
}

//! [a-z][a-z0-9_]{0,62}
bool IsName(const std::string &s) {
	if (s.empty() || s.size() > 63 || !Lower(s[0])) {
		return false;
	}
	for (auto c : s) {
		if (!(Lower(c) || Digit(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

//! [A-Z][A-Z0-9_]{0,63}
bool IsSecretName(const std::string &s) {
	if (s.empty() || s.size() > 64 || !Upper(s[0])) {
		return false;
	}
	for (auto c : s) {
		if (!(Upper(c) || Digit(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

//! [A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*
bool IsPath(const std::string &s) {
	if (s.empty() || s.front() == '.' || s.back() == '.') {
		return false;
	}
	char previous = '.';
	for (auto c : s) {
		if (c == '.') {
			if (previous == '.') {
				return false;
			}
		} else if (!(Lower(c) || Upper(c) || Digit(c) || c == '_')) {
			return false;
		}
		previous = c;
	}
	return true;
}

//! [A-Za-z0-9_-]{1,255}
bool IsCollectionName(const std::string &s) {
	if (s.empty() || s.size() > 255) {
		return false;
	}
	for (auto c : s) {
		if (!(Lower(c) || Upper(c) || Digit(c) || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

//! [a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?
bool IsLabel(const std::string &s) {
	if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') {
		return false;
	}
	for (auto c : s) {
		if (!(Lower(c) || Digit(c) || c == '-')) {
			return false;
		}
	}
	return true;
}

//! Plan 2's EdgeSpaceRule (QdrantConfig.scala): a modelVersion whose first or last character is <= U+0020,
//! Character.isWhitespace or U+00A0. Control characters inside stay data.
constexpr const char *kEdgeWhitespace = "must not start or end with whitespace";

bool HasBraces(const std::string &s) {
	return s.find("{{") != std::string::npos || s.find("}}") != std::string::npos;
}

bool LooksLikeCredential(const std::string &key) {
	std::string k;
	for (size_t i = 0; i < key.size(); ++i) {
		auto c = key[i];
		if (c == '_' || c == '-') {
			continue;
		}
		// Java's toLowerCase(Locale.ROOT) maps U+212A KELVIN SIGN (E2 84 AA) to 'k'.
		if (key.compare(i, 3, "\xe2\x84\xaa") == 0) {
			k += 'k';
			i += 2;
			continue;
		}
		k += Upper(c) ? char(c - 'A' + 'a') : c;
	}
	if (k == "apikeysecret") {
		return false;
	}
	for (auto word : {"apikey", "token", "password"}) {
		if (k.find(word) != std::string::npos) {
			return true;
		}
	}
	return false;
}

//! The code points of `s` (valid UTF-8, as the JSON reader guarantees).
std::vector<uint32_t> Decode(const std::string &s) {
	std::vector<uint32_t> out;
	for (size_t i = 0; i < s.size();) {
		auto b = static_cast<unsigned char>(s[i]);
		size_t len = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
		uint32_t cp = len == 1 ? b : len == 2 ? (b & 0x1f) : len == 3 ? (b & 0x0f) : (b & 0x07);
		for (size_t k = 1; k < len && i + k < s.size(); ++k) {
			cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
		}
		out.push_back(cp);
		i += len;
	}
	return out;
}

//! Plan 2's edgeSpace: `c <= ' ' || Character.isWhitespace(c) || c == '\u00a0'` (every C0 control and the
//! space, Java's whitespace, and U+00A0, which Java leaves out).
bool IsJavaWhitespaceOrNbsp(uint32_t cp) {
	if (cp <= 0x20 || cp == 0xa0 || cp == 0x1680 ||
	    (cp >= 0x2000 && cp <= 0x2006) || (cp >= 0x2008 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
	    cp == 0x205f || cp == 0x3000) {
		return true;
	}
	return false;
}

bool EdgeWhitespace(const std::string &s) {
	auto cps = Decode(s);
	return !cps.empty() && (IsJavaWhitespaceOrNbsp(cps.front()) || IsJavaWhitespaceOrNbsp(cps.back()));
}

//! The length Java's String.length reports: one unit per code point, two for a supplementary one.
size_t Utf16Length(const std::string &s) {
	size_t n = 0;
	for (unsigned char c : s) {
		if ((c & 0xc0) != 0x80) {
			n += c >= 0xf0 ? 2 : 1;
		}
	}
	return n;
}

size_t CodePoints(const std::string &s) {
	size_t n = 0;
	for (unsigned char c : s) {
		if ((c & 0xc0) != 0x80) {
			++n;
		}
	}
	return n;
}

std::string Trim(const std::string &s) {
	size_t b = 0;
	size_t e = s.size();
	while (b < e && static_cast<unsigned char>(s[b]) <= ' ') {
		++b;
	}
	while (e > b && static_cast<unsigned char>(s[e - 1]) <= ' ') {
		--e;
	}
	return s.substr(b, e - b);
}

using Members = std::map<std::string, const JsonNode *>;

const JsonNode *Get(const Members &m, const std::string &key) {
	auto found = m.find(key);
	return found == m.end() ? nullptr : found->second;
}

class Decoder {
public:
	Decoder(const std::set<std::string> &allowlist) : allowlist_(allowlist) {
	}

	std::vector<std::string> errs;

	std::optional<SourceConfig> Root(const JsonNode &v) {
		auto m = Obj("config", v, {"endpoint", "apiKeySecret", "collections"}, {"guarantee"});
		if (!m) {
			return std::nullopt;
		}
		std::optional<std::string> endpoint;
		if (auto e = Str("endpoint", Get(*m, "endpoint"))) {
			endpoint = CheckEndpoint(*e);
		}
		auto secret = Matching("apiKeySecret", Get(*m, "apiKeySecret"), IsSecretName, "[A-Z][A-Z0-9_]{0,63}");
		// Optional: absent means exact_isolated. Checked after apiKeySecret, before collections (contract 1).
		std::optional<std::string> guarantee = std::string(kExactIsolated);
		if (auto g = Get(*m, "guarantee")) {
			guarantee = Guarantee(*g);
		}
		std::optional<std::vector<std::optional<CollectionSpec>>> collections;
		if (auto items = Arr("collections", Get(*m, "collections"), 1, kMaxCollections)) {
			collections.emplace();
			for (size_t i = 0; i < items->size(); ++i) {
				collections->push_back(Collection("collections[" + std::to_string(i) + "]", (*items)[i]));
			}
			std::set<std::string> keys;
			bool repeated = false;
			for (auto &c : *collections) {
				if (c && !keys.insert(c->schema + "." + c->table).second) {
					repeated = true;
				}
			}
			if (repeated) {
				Err("collections", "(schema, table) must be unique per source");
			}
		}
		if (!endpoint || !secret || !guarantee || !collections) {
			return std::nullopt;
		}
		SourceConfig config {*endpoint, *secret, {}, *guarantee};
		for (auto &c : *collections) {
			if (!c) {
				return std::nullopt;
			}
			config.collections.push_back(*c);
		}
		return config;
	}

private:
	const std::set<std::string> &allowlist_;

	void Err(const std::string &path, const std::string &msg) {
		errs.push_back(path + ": " + msg);
	}

	//! The members of the object at `path`: `keys` are required, `optional` may be left out, anything else
	//! is an unknown (or credential-like) key.
	std::optional<Members> Obj(const std::string &path, const JsonNode &v, const std::vector<std::string> &keys,
	                           const std::vector<std::string> &optional = {}) {
		if (!v.Is(JsonNode::Kind::Object)) {
			Err(path, "must be an object");
			return std::nullopt;
		}
		Members m;
		for (auto &member : v.members) {
			bool known = false;
			for (auto &k : keys) {
				known |= k == member.first;
			}
			for (auto &k : optional) {
				known |= k == member.first;
			}
			if (!known) {
				if (LooksLikeCredential(member.first)) {
					Err(path, "key " + ShownKey(member.first) +
					              " looks like a literal credential; name a source secret in apiKeySecret instead");
				} else {
					Err(path, "unknown key " + ShownKey(member.first));
				}
			}
			m[member.first] = &member.second;
		}
		for (auto &k : keys) {
			if (!m.count(k)) {
				Err(path, "missing key \"" + k + "\"");
			}
		}
		return m;
	}

	std::optional<std::string> Str(const std::string &path, const JsonNode *v) {
		if (!v) {
			return std::nullopt;
		}
		if (!v->Is(JsonNode::Kind::String)) {
			Err(path, "must be a string");
			return std::nullopt;
		}
		if (HasBraces(v->text)) {
			Err(path, "must not contain '{{' or '}}'");
			return std::nullopt;
		}
		return v->text;
	}

	std::optional<std::string> Matching(const std::string &path, const JsonNode *v, bool (*ok)(const std::string &),
	                                    const std::string &rule) {
		auto s = Str(path, v);
		if (s && !ok(*s)) {
			Err(path, "must match " + rule);
			return std::nullopt;
		}
		return s;
	}

	std::optional<std::string> Name(const std::string &path, const JsonNode *v) {
		return Matching(path, v, IsName, "[a-z][a-z0-9_]{0,62}");
	}

	std::optional<bool> Bool(const std::string &path, const JsonNode *v) {
		if (!v) {
			return std::nullopt;
		}
		if (!v->Is(JsonNode::Kind::Bool)) {
			Err(path, "must be true or false");
			return std::nullopt;
		}
		return v->boolean;
	}

	std::optional<int64_t> Long(const std::string &path, const JsonNode *v, int64_t min, int64_t max) {
		if (!v) {
			return std::nullopt;
		}
		if (!v->Is(JsonNode::Kind::Int)) {
			Err(path, "must be an integer");
			return std::nullopt;
		}
		int64_t n = 0;
		if (!v->ToInt64(n) || n < min || n > max) {
			Err(path, "must be an integer in " + std::to_string(min) + ".." + std::to_string(max));
			return std::nullopt;
		}
		return n;
	}

	std::optional<std::vector<JsonNode>> Arr(const std::string &path, const JsonNode *v, size_t min, size_t max) {
		if (!v) {
			return std::nullopt;
		}
		if (!v->Is(JsonNode::Kind::Array)) {
			Err(path, "must be an array");
			return std::nullopt;
		}
		if (v->items.size() < min || v->items.size() > max) {
			Err(path, "must hold " + std::to_string(min) + ".." + std::to_string(max) + " entries");
			return std::nullopt;
		}
		return v->items;
	}

	void Unique(const std::string &path, const std::string &what, const std::vector<std::string> &names) {
		std::set<std::string> seen(names.begin(), names.end());
		if (seen.size() != names.size()) {
			Err(path, what + " names must be unique");
		}
	}

	std::optional<std::string> CheckEndpoint(const std::string &e) {
		if (auto why = EndpointSyntaxError(e)) {
			Err("endpoint", *why);
			return std::nullopt;
		}
		if (allowlist_.empty()) {
			Err("endpoint", std::string("the deployment allowlist (") + kAllowlistEnv +
			                    ") is empty or unset, so no endpoint is accepted");
			return std::nullopt;
		}
		if (!allowlist_.count(e) && !allowlist_.count(kAnyHttpsOrigin)) {
			Err("endpoint", std::string("is not in the deployment allowlist (") + kAllowlistEnv + ")");
			return std::nullopt;
		}
		return e;
	}

	std::optional<std::string> Guarantee(const JsonNode &v) {
		auto s = Str("guarantee", &v);
		if (!s) {
			return std::nullopt;
		}
		if (*s != kExactIsolated && *s != kFilteredRetrieval) {
			Err("guarantee", "must be exact_isolated or filtered_retrieval");
			return std::nullopt;
		}
		return s;
	}

	//! A vector's dependsOn: 1..kMaxFields declared field names, unique (dense and sparse vectors alike).
	std::optional<std::vector<std::optional<std::string>>> DependsOn(const std::string &path, const JsonNode *v,
	                                                                 const std::vector<FieldSpec> &declared) {
		auto items = Arr(path, v, 1, kMaxFields);
		if (!items) {
			return std::nullopt;
		}
		std::vector<std::optional<std::string>> deps;
		for (size_t i = 0; i < items->size(); ++i) {
			auto item_path = path + "[" + std::to_string(i) + "]";
			auto s = Str(item_path, &(*items)[i]);
			if (s) {
				bool found = false;
				for (auto &f : declared) {
					found |= f.name == *s;
				}
				if (!found) {
					Err(item_path, "must name a declared field");
					s.reset();
				}
			}
			deps.push_back(s);
		}
		std::vector<std::string> dep_names;
		for (auto &d : deps) {
			if (d) {
				dep_names.push_back(*d);
			}
		}
		Unique(path, "dependsOn", dep_names);
		return deps;
	}

	std::optional<CollectionSpec> Collection(const std::string &path, const JsonNode &v) {
		auto m = Obj(path, v, {"schema", "table", "collection", "generation", "schemaVersion", "fields", "vectors"},
		             {"sparseVectors"});
		if (!m) {
			return std::nullopt;
		}
		auto schema = Name(path + ".schema", Get(*m, "schema"));
		auto table = Name(path + ".table", Get(*m, "table"));
		auto coll = Matching(path + ".collection", Get(*m, "collection"), IsCollectionName, "[A-Za-z0-9_-]{1,255}");
		auto generation = Long(path + ".generation", Get(*m, "generation"), 1, INT64_MAX);
		auto schema_version = Long(path + ".schemaVersion", Get(*m, "schemaVersion"), 1, INT64_MAX);
		std::optional<std::vector<std::optional<FieldSpec>>> fields;
		if (auto items = Arr(path + ".fields", Get(*m, "fields"), 1, kMaxFields)) {
			fields.emplace();
			for (size_t i = 0; i < items->size(); ++i) {
				fields->push_back(Field(path + ".fields[" + std::to_string(i) + "]", (*items)[i]));
			}
		}
		std::vector<FieldSpec> declared;
		if (fields) {
			for (auto &f : *fields) {
				if (f) {
					declared.push_back(*f);
				}
			}
		}
		std::vector<std::string> field_names;
		for (auto &f : declared) {
			field_names.push_back(f.name);
		}
		Unique(path + ".fields", "field", field_names);
		std::optional<std::vector<std::optional<VectorSpec>>> vectors;
		std::vector<std::string> vector_names;
		if (auto items = Arr(path + ".vectors", Get(*m, "vectors"), 0, kMaxVectors)) {
			vectors.emplace();
			for (size_t i = 0; i < items->size(); ++i) {
				vectors->push_back(Vector(path + ".vectors[" + std::to_string(i) + "]", (*items)[i], declared));
			}
			for (auto &x : *vectors) {
				if (x) {
					vector_names.push_back(x->name);
				}
			}
			Unique(path + ".vectors", "vector", vector_names);
		}
		// Optional (absent is no sparse vector): each entry in order, then uniqueness, then the clash with the
		// dense names, both over the entries that parsed (contract section 1).
		std::optional<std::vector<std::optional<SparseVectorSpec>>> sparse;
		if (auto s = Get(*m, "sparseVectors")) {
			if (auto items = Arr(path + ".sparseVectors", s, 0, kMaxSparseVectors)) {
				sparse.emplace();
				for (size_t i = 0; i < items->size(); ++i) {
					sparse->push_back(Sparse(path + ".sparseVectors[" + std::to_string(i) + "]", (*items)[i], declared));
				}
				std::vector<std::string> sparse_names;
				for (auto &x : *sparse) {
					if (x) {
						sparse_names.push_back(x->name);
					}
				}
				Unique(path + ".sparseVectors", "sparse vector", sparse_names);
				bool clash = false;
				for (auto &name : sparse_names) {
					clash |= std::find(vector_names.begin(), vector_names.end(), name) != vector_names.end();
				}
				if (clash) {
					Err(path + ".sparseVectors", "names must differ from the dense vector names");
				}
			}
		} else {
			sparse.emplace();
		}
		if (!schema || !table || !coll || !generation || !schema_version || !fields || !vectors || !sparse) {
			return std::nullopt;
		}
		CollectionSpec result;
		result.schema = *schema;
		result.table = *table;
		result.collection = *coll;
		result.generation = *generation;
		result.schema_version = *schema_version;
		for (auto &f : *fields) {
			if (!f) {
				return std::nullopt;
			}
			result.fields.push_back(*f);
		}
		for (auto &x : *vectors) {
			if (!x) {
				return std::nullopt;
			}
			result.vectors.push_back(*x);
		}
		for (auto &x : *sparse) {
			if (!x) {
				return std::nullopt;
			}
			result.sparse_vectors.push_back(*x);
		}
		return result;
	}

	//! A sparse vector's options: an object of any keys, kept as declared, nesting at most kMaxOptionsDepth
	//! levels, with no '{{' or '}}' in a key or a string value (the manager's rules, QdrantConfig.optionsOf).
	std::optional<JsonNode> Options(const std::string &path, const JsonNode &v) {
		if (!v.Is(JsonNode::Kind::Object)) {
			Err(path, "must be an object");
			return std::nullopt;
		}
		auto before = errs.size();
		if (DepthOf(v) > kMaxOptionsDepth) {
			Err(path, "must nest at most " + std::to_string(kMaxOptionsDepth) + " levels");
		} else {
			BracesIn(path, v);
		}
		if (errs.size() != before) {
			return std::nullopt;
		}
		return v;
	}

	static size_t DepthOf(const JsonNode &v) {
		size_t deepest = 0;
		if (v.Is(JsonNode::Kind::Object)) {
			for (auto &m : v.members) {
				deepest = std::max(deepest, DepthOf(m.second));
			}
			return 1 + deepest;
		}
		if (v.Is(JsonNode::Kind::Array)) {
			for (auto &i : v.items) {
				deepest = std::max(deepest, DepthOf(i));
			}
			return 1 + deepest;
		}
		return 0;
	}

	void BracesIn(const std::string &path, const JsonNode &v) {
		if (v.Is(JsonNode::Kind::Object)) {
			bool braced_key = false;
			for (auto &m : v.members) {
				braced_key |= HasBraces(m.first);
			}
			if (braced_key) {
				Err(path, "a key must not contain '{{' or '}}'");
			}
			for (auto &m : v.members) {
				if (!HasBraces(m.first)) {
					BracesIn(path + "." + m.first, m.second);
				}
			}
		} else if (v.Is(JsonNode::Kind::Array)) {
			for (size_t i = 0; i < v.items.size(); i++) {
				BracesIn(path + "[" + std::to_string(i) + "]", v.items[i]);
			}
		} else if (v.Is(JsonNode::Kind::String) && HasBraces(v.text)) {
			Err(path, "must not contain '{{' or '}}'");
		}
	}

	std::optional<SparseVectorSpec> Sparse(const std::string &path, const JsonNode &v, const std::vector<FieldSpec> &declared) {
		auto m = Obj(path, v, {"name", "model", "dependsOn"}, {"options"});
		if (!m) {
			return std::nullopt;
		}
		auto name = Name(path + ".name", Get(*m, "name"));
		std::optional<JsonNode> options = JsonNode::Object({});
		if (auto o = Get(*m, "options")) {
			options = Options(path + ".options", *o);
		}
		std::optional<std::string> model;
		if (auto t = Str(path + ".model", Get(*m, "model"))) {
			if (*t == kBm25Model) {
				model = *t;
			} else {
				Err(path + ".model", "must be qdrant/bm25");
			}
		}
		auto deps = DependsOn(path + ".dependsOn", Get(*m, "dependsOn"), declared);
		if (!name || !model || !deps || !options) {
			return std::nullopt;
		}
		SparseVectorSpec result;
		result.name = *name;
		result.model = *model;
		result.options = std::move(*options);
		for (auto &d : *deps) {
			if (!d) {
				return std::nullopt;
			}
			result.depends_on.push_back(*d);
		}
		return result;
	}

	std::optional<FieldSpec> Field(const std::string &path, const JsonNode &v) {
		auto m = Obj(path, v, {"name", "path", "type", "required"});
		if (!m) {
			return std::nullopt;
		}
		auto name = Name(path + ".name", Get(*m, "name"));
		auto p = Matching(path + ".path", Get(*m, "path"), IsPath, "dot-separated [A-Za-z0-9_]+");
		std::optional<FieldType> type;
		if (auto t = Str(path + ".type", Get(*m, "type"))) {
			for (auto candidate : {FieldType::Varchar, FieldType::Bigint, FieldType::Boolean, FieldType::Double}) {
				if (*t == FieldTypeSql(candidate)) {
					type = candidate;
				}
			}
			if (!type) {
				Err(path + ".type", "must be one of VARCHAR, BIGINT, BOOLEAN, DOUBLE");
			}
		}
		auto required = Bool(path + ".required", Get(*m, "required"));
		if (!name || !p || !type || !required) {
			return std::nullopt;
		}
		return FieldSpec {*name, *p, *type, *required};
	}

	std::optional<VectorSpec> Vector(const std::string &path, const JsonNode &v, const std::vector<FieldSpec> &declared) {
		auto m = Obj(path, v, {"name", "dimensions", "datatype", "dependsOn", "modelVersion"});
		if (!m) {
			return std::nullopt;
		}
		auto name = Name(path + ".name", Get(*m, "name"));
		auto dims = Long(path + ".dimensions", Get(*m, "dimensions"), 1, kMaxDimensions);
		std::optional<std::string> datatype;
		if (auto t = Str(path + ".datatype", Get(*m, "datatype"))) {
			if (*t == "float32") {
				datatype = *t;
			} else {
				Err(path + ".datatype", "must be float32");
			}
		}
		auto deps = DependsOn(path + ".dependsOn", Get(*m, "dependsOn"), declared);
		std::optional<std::string> model;
		if (auto s = Str(path + ".modelVersion", Get(*m, "modelVersion"))) {
			auto n = CodePoints(*s);
			// Plan 2's order: edge whitespace before the length rule.
			if (EdgeWhitespace(*s)) {
				Err(path + ".modelVersion", kEdgeWhitespace);
			} else if (n >= 1 && n <= kMaxModelVersionChars) {
				model = *s;
			} else {
				Err(path + ".modelVersion", "must hold 1.." + std::to_string(kMaxModelVersionChars) + " characters");
			}
		}
		if (!name || !dims || !datatype || !deps || !model) {
			return std::nullopt;
		}
		VectorSpec result;
		result.name = *name;
		result.dimensions = *dims;
		result.datatype = *datatype;
		result.model_version = *model;
		for (auto &d : *deps) {
			if (!d) {
				return std::nullopt;
			}
			result.depends_on.push_back(*d);
		}
		return result;
	}
};

} // namespace

std::optional<std::string> EndpointSyntaxError(const std::string &endpoint) {
	static const std::string kGeneric =
	    "endpoint must be https://host[:port] with a lowercase host and no path, query, fragment or userinfo";
	const std::string scheme = "https://";
	// Plan 2's MaxLength (QdrantEndpoint.scala): "https://".length + 253 + ":65535".length = 267 UTF-16
	// units, checked before the origin rule.
	if (Utf16Length(endpoint) > 8 + 253 + 6) {
		return std::string("endpoint is longer than any valid origin");
	}
	if (endpoint.compare(0, scheme.size(), scheme) != 0) {
		return kGeneric;
	}
	auto rest = endpoint.substr(scheme.size());
	auto colon = rest.find(':');
	auto host = rest.substr(0, colon);
	std::string port;
	if (colon != std::string::npos) {
		port = rest.substr(colon + 1);
		if (port.empty() || port.size() > 5 || port[0] < '1' || port[0] > '9') {
			return kGeneric;
		}
		for (auto c : port) {
			if (!Digit(c)) {
				return kGeneric;
			}
		}
	}
	size_t start = 0;
	while (true) {
		auto dot = host.find('.', start);
		if (!IsLabel(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start))) {
			return kGeneric;
		}
		if (dot == std::string::npos) {
			break;
		}
		start = dot + 1;
	}
	if (host.size() > 253) {
		return std::string("endpoint host is longer than 253 characters");
	}
	if (!port.empty() && std::stol(port) > 65535) {
		return std::string("endpoint port must be 1..65535");
	}
	return std::nullopt;
}

std::pair<std::set<std::string>, size_t> ParseAllowlist(const char *raw) {
	std::set<std::string> origins;
	size_t dropped = 0;
	if (!raw) {
		return {origins, 0};
	}
	std::string text(raw);
	size_t start = 0;
	while (start <= text.size()) {
		auto comma = text.find(',', start);
		auto entry = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if (!entry.empty()) {
			if (EndpointSyntaxError(entry)) {
				++dropped;
			} else {
				origins.insert(entry);
			}
		}
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	return {origins, dropped};
}

ConfigResult ParseConfig(const std::string &raw, const std::set<std::string> &allowlist) {
	ConfigResult result;
	if (raw.size() > kMaxConfigBytes) {
		result.errors.push_back("config is larger than " + std::to_string(kMaxConfigBytes) + " bytes");
		return result;
	}
	JsonNode json;
	try {
		json = ParseJson(raw, kConfigJsonLimits);
	} catch (JsonError &e) {
		result.errors.push_back(std::string("config is not valid JSON: ") + e.what());
		return result;
	}
	Decoder d(allowlist);
	auto config = d.Root(json);
	std::set<std::string> seen;
	for (auto &e : d.errs) {
		if (seen.insert(e).second) {
			result.errors.push_back(e);
		}
	}
	if (result.errors.empty()) {
		if (config) {
			result.config = std::move(config);
		} else {
			result.errors.push_back("config could not be read");
		}
	}
	return result;
}

} // namespace qod_qdrant
