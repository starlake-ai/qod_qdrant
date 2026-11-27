#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qod_qdrant {

//! A JSON value as ParseJson reads it. Objects keep their members in document order. A number with no
//! fraction and no exponent is an Int, any other number a Decimal; both keep their literal text, so a
//! u64 point id or an int64 boundary value never passes through a double.
struct JsonNode {
	enum class Kind { Null, Bool, Int, Decimal, String, Array, Object };
	Kind kind = Kind::Null;
	bool boolean = false;
	//! Int and Decimal: the literal text. String: the decoded UTF-8 value.
	std::string text;
	std::vector<JsonNode> items;
	std::vector<std::pair<std::string, JsonNode>> members;

	static JsonNode Null();
	static JsonNode Bool(bool value);
	static JsonNode Int(int64_t value);
	static JsonNode Uint(uint64_t value);
	//! A finite float32 written with 9 significant digits, which reads back to the same float.
	static JsonNode Float(float value);
	static JsonNode String(std::string value);
	static JsonNode Array(std::vector<JsonNode> items);
	static JsonNode Object(std::vector<std::pair<std::string, JsonNode>> members);

	bool Is(Kind k) const {
		return kind == k;
	}
	//! The member `key` of an object, or nullptr (also when this is not an object).
	const JsonNode *Find(const std::string &key) const;
	//! An Int that fits; false for anything else.
	bool ToInt64(int64_t &out) const;
	bool ToUint64(uint64_t &out) const;
	//! An Int or Decimal as a finite double; false for anything else.
	bool ToDouble(double &out) const;
};

struct JsonLimits {
	size_t max_depth;
	size_t max_number_chars;
	//! Values (scalars, arrays and objects, the document itself included) one document may hold.
	size_t max_nodes;
};
//! The config reader's limits: Plan 2's StrictJson (MaxDepth 16, MaxNumberChars 64, no value cap; a config
//! is at most kMaxConfigBytes anyway).
constexpr JsonLimits kConfigJsonLimits {16, 64, SIZE_MAX};
//! Response bodies: payloads may nest deeper than a config. The value cap bounds the parsed tree: a body of
//! search_max_response_bytes (16 MiB) of tiny values would otherwise expand to ~700 MB of 80-byte JsonNodes.
//! search_max_k (1000) points of kMaxFields (256) fields at path depth 3 stay under it.
constexpr JsonLimits kResponseJsonLimits {64, 64, 1048576};

//! What ParseJson throws: "<what> at offset <n>", or for a duplicate the key; never a value.
class JsonError : public std::runtime_error {
public:
	explicit JsonError(const std::string &what) : std::runtime_error(what) {
	}
};

//! What ParseJson throws when a document holds more than limits.max_nodes values: a resource limit, so
//! callers may tell it from a malformed document.
class JsonBudgetError : public JsonError {
public:
	explicit JsonBudgetError(const std::string &what) : JsonError(what) {
	}
};

//! Strict RFC 8259 (Plan 2's StrictJson): exactly one value; whitespace only space, tab, LF, CR; no
//! duplicate key; no lone surrogate, escaped or raw, and raw strings must be valid UTF-8; nesting at
//! most limits.max_depth; number literals at most limits.max_number_chars characters; at most
//! limits.max_nodes values (JsonBudgetError).
JsonNode ParseJson(const std::string &text, const JsonLimits &limits);

//! A key as an error message shows it: quoted, at most 64 bytes, then "...".
std::string ShownKey(const std::string &key);

//! Compact JSON, members in their stored order. Strings escape only ", \ and U+0000..U+001F (as \b \f
//! \n \r \t or a lowercase \u00xx); everything else, ' and non-ASCII included, is written as itself.
std::string WriteJson(const JsonNode &value);

} // namespace qod_qdrant
