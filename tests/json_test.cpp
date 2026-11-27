// The strict JSON reader: the cases of Plan 2's StrictJsonSpec, so both sides refuse the same text.
#include "ast/json.hpp"
#include "check.hpp"

using namespace qod_qdrant;
using namespace qod_qdrant_test;

namespace {
std::string Refusal(const std::string &text, const JsonLimits &limits = kConfigJsonLimits) {
	try {
		ParseJson(text, limits);
	} catch (JsonError &e) {
		return e.what();
	}
	return "accepted";
}
} // namespace

int main() {
	auto v = ParseJson(" {\"b\": [1, -2.5e3, true, false, null], \"a\": \"x\"} ", kConfigJsonLimits);
	Expect("kinds and key order", WriteJson(v), "{\"b\":[1,-2.5e3,true,false,null],\"a\":\"x\"}");
	Expect("Int kind", v.members[0].second.items[0].Is(JsonNode::Kind::Int) ? "int" : "other", "int");
	Expect("Decimal kind", v.members[0].second.items[1].Is(JsonNode::Kind::Decimal) ? "decimal" : "other", "decimal");

	auto escapes = ParseJson("\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t \\u00e9 \\ud83d\\ude00\"", kConfigJsonLimits);
	Expect("every escape", escapes.text, "\" \\ / \b \f \n \r \t \xc3\xa9 \xf0\x9f\x98\x80");

	int64_t i64 = 0;
	uint64_t u64 = 0;
	auto big = ParseJson("12345678901234567890", kConfigJsonLimits);
	Expect("big integer stays text", big.text, "12345678901234567890");
	ExpectTrue("big integer does not fit int64", !big.ToInt64(i64));
	ExpectTrue("big integer fits uint64", big.ToUint64(u64) && u64 == 12345678901234567890ULL);
	ExpectTrue("int64 min", ParseJson("-9223372036854775808", kConfigJsonLimits).ToInt64(i64) &&
	                            i64 == INT64_MIN);
	ExpectTrue("int64 max + 1 refused", !ParseJson("9223372036854775808", kConfigJsonLimits).ToInt64(i64));
	ExpectTrue("-0 is zero", ParseJson("-0", kConfigJsonLimits).ToInt64(i64) && i64 == 0);

	auto dup = Refusal("{\"endpoint\": \"a-secret-value\", \"endpoint\": \"b\"}");
	Contains("duplicate key named, value not", dup, "duplicate key \"endpoint\"", "a-secret-value");
	Contains("nested duplicate", Refusal("{\"a\": [{\"x\": 1, \"x\": 1}]}"), "duplicate key \"x\"");
	Contains("trailing content", Refusal("{} {}"), "unexpected content after the JSON value");
	for (auto n : {"01", "1.", ".5", "-", "1e", "1e+", "+1", "0x10"}) {
		ExpectTrue(std::string("malformed number ") + n, Refusal(std::string("[") + n + "]") != "accepted");
	}
	Contains("long number", Refusal(std::string(65, '1')), "number longer than 64");
	std::string deep = std::string(18, '[') + std::string(18, ']');
	std::string ok = std::string(17, '[') + std::string(17, ']');
	Contains("too deep", Refusal(deep), "nesting deeper than 16");
	Expect("deepest accepted", Refusal(ok), "accepted");
	Expect("responses nest deeper", Refusal(deep, kResponseJsonLimits), "accepted");
	constexpr JsonLimits three {16, 64, 3};
	Expect("value cap counts the document, members and items", Refusal("{\"a\":[1]}", three), "accepted");
	Contains("one value more refuses", Refusal("{\"a\":[1],\"b\":null}", three), "more than 3 values at offset 13");
	Contains("escaped lone high surrogate", Refusal("\"\\ud800\""), "lone surrogate");
	Contains("escaped lone low surrogate", Refusal("\"\\udc00x\""), "lone surrogate");
	Contains("raw lone surrogate", Refusal("\"a\xed\xa0\x80" "b\""), "lone surrogate");
	Contains("invalid UTF-8", Refusal("\"a\xff" "b\""), "invalid UTF-8");
	Contains("raw control character", Refusal("\"a\nb\""), "unescaped control character");
	Contains("bad escape", Refusal("\"\\x\""), "invalid escape");
	Contains("non-ASCII hex digit", Refusal("\"\\u00\xd9\xa3\xd9\xa3\""), "invalid unicode escape");
	Contains("tru", Refusal("tru"), "invalid literal");
	Contains("nul", Refusal("nul"), "invalid literal");
	Contains("unterminated", Refusal("\"abc"), "unterminated string");
	Expect("JSON whitespace", Refusal(" \t\n\r{}\r\n\t "), "accepted");
	ExpectTrue("no-break space refused", Refusal("\xc2\xa0{}") != "accepted");
	ExpectTrue("byte order mark refused", Refusal("\xef\xbb\xbf{}") != "accepted");
	Contains("empty input", Refusal(""), "unexpected end of input");
	Contains("missing colon", Refusal("{\"a\" 1}"), "expected ':'");
	Contains("missing comma", Refusal("[1 2]"), "expected ',' or ']'");

	double d = 0;
	ExpectTrue("decimal to double", ParseJson("0.25", kResponseJsonLimits).ToDouble(d) && d == 0.25);
	ExpectTrue("huge decimal does not fit", !ParseJson("1e999", kResponseJsonLimits).ToDouble(d));
	Expect("writer escapes only what JSON requires",
	       WriteJson(JsonNode::String(std::string("it's \"q\" \\ / \xc3\xa9\n") + '\x01')),
	       "\"it's \\\"q\\\" \\\\ / \xc3\xa9\\n\\u0001\"");
	Expect("float32 round trip", WriteJson(JsonNode::Float(0.1f)), "0.100000001");
	Expect("uint64", WriteJson(JsonNode::Uint(18446744073709551615ULL)), "18446744073709551615");
	Expect("find", ParseJson("{\"a\":1}", kConfigJsonLimits).Find("a")->text, "1");
	ExpectTrue("find missing", ParseJson("{\"a\":1}", kConfigJsonLimits).Find("b") == nullptr);
	Expect("shown key cut", ShownKey(std::string(70, 'k')), "\"" + std::string(64, 'k') + "...\"");
	std::string e_acute = "\xc3\xa9";
	auto repeat = [](const std::string &unit, size_t n) {
		std::string out;
		for (size_t i = 0; i < n; ++i) {
			out += unit;
		}
		return out;
	};
	Expect("shown key, 40 non-ASCII chars whole", ShownKey(repeat(e_acute, 40)), "\"" + repeat(e_acute, 40) + "\"");
	Expect("shown key, 70 non-ASCII chars cut at 64 chars", ShownKey(repeat(e_acute, 70)),
	       "\"" + repeat(e_acute, 64) + "...\"");
	std::string emoji = "\xf0\x9f\x98\x80";
	// 63 units then an emoji (2 units) would end at 65: stopped before it.
	Expect("shown key, emoji straddling unit 64", ShownKey(repeat("a", 63) + emoji + "zz"),
	       "\"" + repeat("a", 63) + "...\"");
	Expect("shown key, emoji ending exactly at 64", ShownKey(repeat("a", 62) + emoji + "zz"),
	       "\"" + repeat("a", 62) + emoji + "...\"");
	return Finish();
}
