#include "ast/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>

namespace qod_qdrant {

JsonNode JsonNode::Null() {
	return JsonNode();
}
JsonNode JsonNode::Bool(bool value) {
	JsonNode n;
	n.kind = Kind::Bool;
	n.boolean = value;
	return n;
}
JsonNode JsonNode::Int(int64_t value) {
	JsonNode n;
	n.kind = Kind::Int;
	n.text = std::to_string(value);
	return n;
}
JsonNode JsonNode::Uint(uint64_t value) {
	JsonNode n;
	n.kind = Kind::Int;
	n.text = std::to_string(value);
	return n;
}
JsonNode JsonNode::Float(float value) {
	if (!std::isfinite(value)) {
		throw std::invalid_argument("JsonNode::Float needs a finite value");
	}
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
	JsonNode n;
	n.kind = Kind::Decimal;
	n.text = buffer;
	return n;
}
JsonNode JsonNode::String(std::string value) {
	JsonNode n;
	n.kind = Kind::String;
	n.text = std::move(value);
	return n;
}
JsonNode JsonNode::Array(std::vector<JsonNode> items) {
	JsonNode n;
	n.kind = Kind::Array;
	n.items = std::move(items);
	return n;
}
JsonNode JsonNode::Object(std::vector<std::pair<std::string, JsonNode>> members) {
	JsonNode n;
	n.kind = Kind::Object;
	n.members = std::move(members);
	return n;
}

const JsonNode *JsonNode::Find(const std::string &key) const {
	if (kind != Kind::Object) {
		return nullptr;
	}
	for (auto &member : members) {
		if (member.first == key) {
			return &member.second;
		}
	}
	return nullptr;
}

bool JsonNode::ToInt64(int64_t &out) const {
	if (kind != Kind::Int) {
		return false;
	}
	bool negative = !text.empty() && text[0] == '-';
	uint64_t magnitude = 0;
	for (size_t i = negative ? 1 : 0; i < text.size(); ++i) {
		uint64_t digit = uint64_t(text[i] - '0');
		if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
			return false;
		}
		magnitude = magnitude * 10 + digit;
	}
	if (negative) {
		if (magnitude > uint64_t(std::numeric_limits<int64_t>::max()) + 1) {
			return false;
		}
		out = magnitude == 0 ? 0 : int64_t(0 - magnitude);
		return true;
	}
	if (magnitude > uint64_t(std::numeric_limits<int64_t>::max())) {
		return false;
	}
	out = int64_t(magnitude);
	return true;
}

bool JsonNode::ToUint64(uint64_t &out) const {
	if (kind != Kind::Int || text.empty() || text[0] == '-') {
		return false;
	}
	uint64_t value = 0;
	for (auto c : text) {
		uint64_t digit = uint64_t(c - '0');
		if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
			return false;
		}
		value = value * 10 + digit;
	}
	out = value;
	return true;
}

bool JsonNode::ToDouble(double &out) const {
	if (kind != Kind::Int && kind != Kind::Decimal) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	double value = std::strtod(text.c_str(), &end);
	if (errno == ERANGE || end != text.c_str() + text.size() || !std::isfinite(value)) {
		return false;
	}
	out = value;
	return true;
}

std::string ShownKey(const std::string &key) {
	// Plan 2's StrictJson.shown cuts at 64 UTF-16 code units. The key is valid UTF-8 (ParseJson checked it),
	// so walk it code point by code point: 1 unit below U+10000, 2 above. One documented difference: when a
	// supplementary code point straddles unit 64, Scala keeps its lone high surrogate; UTF-8 cannot, so the
	// cut stops before that code point.
	size_t units = 0;
	size_t cut_at = key.size();
	bool cut = false;
	for (size_t i = 0; i < key.size();) {
		auto b = uint8_t(key[i]);
		size_t len = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
		size_t width = len == 4 ? 2 : 1;
		if (units + width > 64) {
			cut_at = i;
			cut = true;
			break;
		}
		units += width;
		i += len;
	}
	// Exactly 64 units or fewer shows whole; a longer key is cut and marked.
	if (!cut) {
		return "\"" + key + "\"";
	}
	return "\"" + key.substr(0, cut_at) + "...\"";
}

namespace {

class Parser {
public:
	Parser(const std::string &s, const JsonLimits &limits) : s_(s), limits_(limits) {
	}

	JsonNode Document() {
		SkipWs();
		auto v = Value(0);
		SkipWs();
		if (pos_ < s_.size()) {
			Fail("unexpected content after the JSON value");
		}
		return v;
	}

private:
	const std::string &s_;
	JsonLimits limits_;
	size_t pos_ = 0;
	size_t nodes_ = 0;

	[[noreturn]] void Fail(const std::string &what) {
		throw JsonError(what + " at offset " + std::to_string(pos_));
	}
	[[noreturn]] void FailAt(const std::string &what, size_t at) {
		throw JsonError(what + " at offset " + std::to_string(at));
	}
	char Peek() {
		if (pos_ >= s_.size()) {
			Fail("unexpected end of input");
		}
		return s_[pos_];
	}
	void Expect(char c) {
		if (pos_ >= s_.size() || s_[pos_] != c) {
			Fail(std::string("expected '") + c + "'");
		}
		++pos_;
	}
	void SkipWs() {
		while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
			++pos_;
		}
	}

	JsonNode Value(size_t depth) {
		if (depth > limits_.max_depth) {
			Fail("nesting deeper than " + std::to_string(limits_.max_depth));
		}
		if (++nodes_ > limits_.max_nodes) {
			throw JsonBudgetError("more than " + std::to_string(limits_.max_nodes) + " values at offset " +
			                      std::to_string(pos_));
		}
		char c = Peek();
		if (c == '{') {
			return Object(depth);
		}
		if (c == '[') {
			return Array(depth);
		}
		if (c == '"') {
			return JsonNode::String(String());
		}
		if (c == 't') {
			Literal("true");
			return JsonNode::Bool(true);
		}
		if (c == 'f') {
			Literal("false");
			return JsonNode::Bool(false);
		}
		if (c == 'n') {
			Literal("null");
			return JsonNode::Null();
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return Number();
		}
		Fail("unexpected character");
	}

	void Literal(const char *word) {
		std::string w(word);
		if (s_.compare(pos_, w.size(), w) != 0) {
			Fail("invalid literal");
		}
		pos_ += w.size();
	}

	JsonNode Object(size_t depth) {
		Expect('{');
		JsonNode result;
		result.kind = JsonNode::Kind::Object;
		std::set<std::string> seen;
		SkipWs();
		if (Peek() == '}') {
			++pos_;
			return result;
		}
		while (true) {
			SkipWs();
			if (Peek() != '"') {
				Fail("expected an object key");
			}
			auto key_at = pos_;
			auto key = String();
			if (!seen.insert(key).second) {
				throw JsonError("duplicate key " + ShownKey(key) + " at offset " + std::to_string(key_at));
			}
			SkipWs();
			Expect(':');
			SkipWs();
			result.members.emplace_back(key, Value(depth + 1));
			SkipWs();
			char c = Peek();
			if (c == ',') {
				++pos_;
			} else if (c == '}') {
				++pos_;
				return result;
			} else {
				Fail("expected ',' or '}'");
			}
		}
	}

	JsonNode Array(size_t depth) {
		Expect('[');
		JsonNode result;
		result.kind = JsonNode::Kind::Array;
		SkipWs();
		if (Peek() == ']') {
			++pos_;
			return result;
		}
		while (true) {
			SkipWs();
			result.items.push_back(Value(depth + 1));
			SkipWs();
			char c = Peek();
			if (c == ',') {
				++pos_;
			} else if (c == ']') {
				++pos_;
				return result;
			} else {
				Fail("expected ',' or ']'");
			}
		}
	}

	static void AppendUtf8(std::string &out, uint32_t cp) {
		if (cp < 0x80) {
			out += char(cp);
		} else if (cp < 0x800) {
			out += char(0xc0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			out += char(0xe0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3f));
			out += char(0x80 | (cp & 0x3f));
		} else {
			out += char(0xf0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3f));
			out += char(0x80 | ((cp >> 6) & 0x3f));
			out += char(0x80 | (cp & 0x3f));
		}
	}

	uint32_t Hex4() {
		if (pos_ + 4 > s_.size()) {
			Fail("truncated unicode escape");
		}
		uint32_t v = 0;
		for (int i = 0; i < 4; ++i) {
			char c = s_[pos_ + i];
			int d = (c >= '0' && c <= '9') ? c - '0'
			        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
			        : (c >= 'A' && c <= 'F') ? c - 'A' + 10
			                                 : -1;
			if (d < 0) {
				Fail("invalid unicode escape");
			}
			v = v * 16 + uint32_t(d);
		}
		pos_ += 4;
		return v;
	}

	//! One raw UTF-8 sequence starting at pos_ (a byte >= 0x80), copied to out.
	void RawUtf8(std::string &out, size_t start) {
		auto b0 = uint8_t(s_[pos_]);
		size_t len = (b0 >= 0xc2 && b0 <= 0xdf) ? 2 : (b0 >= 0xe0 && b0 <= 0xef) ? 3 : (b0 >= 0xf0 && b0 <= 0xf4) ? 4 : 0;
		if (len == 0 || pos_ + len > s_.size()) {
			Fail("invalid UTF-8 in a string");
		}
		for (size_t i = 1; i < len; ++i) {
			if ((uint8_t(s_[pos_ + i]) & 0xc0) != 0x80) {
				Fail("invalid UTF-8 in a string");
			}
		}
		auto b1 = uint8_t(s_[pos_ + 1]);
		if (b0 == 0xed && b1 >= 0xa0) {
			FailAt("lone surrogate in the string", start);
		}
		if ((b0 == 0xe0 && b1 < 0xa0) || (b0 == 0xf0 && b1 < 0x90) || (b0 == 0xf4 && b1 >= 0x90)) {
			Fail("invalid UTF-8 in a string");
		}
		out.append(s_, pos_, len);
		pos_ += len;
	}

	std::string String() {
		auto start = pos_;
		Expect('"');
		std::string out;
		while (true) {
			if (pos_ >= s_.size()) {
				Fail("unterminated string");
			}
			char c = s_[pos_];
			if (c == '"') {
				++pos_;
				return out;
			}
			if (c == '\\') {
				++pos_;
				if (pos_ >= s_.size()) {
					Fail("unterminated escape");
				}
				char e = s_[pos_++];
				switch (e) {
				case '"':
					out += '"';
					break;
				case '\\':
					out += '\\';
					break;
				case '/':
					out += '/';
					break;
				case 'b':
					out += '\b';
					break;
				case 'f':
					out += '\f';
					break;
				case 'n':
					out += '\n';
					break;
				case 'r':
					out += '\r';
					break;
				case 't':
					out += '\t';
					break;
				case 'u': {
					auto cp = Hex4();
					if (cp >= 0xdc00 && cp <= 0xdfff) {
						FailAt("lone surrogate in the string", start);
					}
					if (cp >= 0xd800 && cp <= 0xdbff) {
						if (pos_ + 6 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
							FailAt("lone surrogate in the string", start);
						}
						pos_ += 2;
						auto low = Hex4();
						if (low < 0xdc00 || low > 0xdfff) {
							FailAt("lone surrogate in the string", start);
						}
						cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
					}
					AppendUtf8(out, cp);
					break;
				}
				default:
					--pos_;
					Fail("invalid escape");
				}
			} else if (uint8_t(c) < 0x20) {
				Fail("unescaped control character in a string");
			} else if (uint8_t(c) >= 0x80) {
				RawUtf8(out, start);
			} else {
				out += c;
				++pos_;
			}
		}
	}

	size_t Digits() {
		auto start = pos_;
		while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
			++pos_;
		}
		return pos_ - start;
	}

	JsonNode Number() {
		auto start = pos_;
		if (s_[pos_] == '-') {
			++pos_;
		}
		if (pos_ >= s_.size()) {
			Fail("invalid number");
		}
		if (s_[pos_] == '0') {
			++pos_;
		} else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
			Digits();
		} else {
			Fail("invalid number");
		}
		bool integral = true;
		if (pos_ < s_.size() && s_[pos_] == '.') {
			integral = false;
			++pos_;
			if (Digits() == 0) {
				Fail("invalid number");
			}
		}
		if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
			integral = false;
			++pos_;
			if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
				++pos_;
			}
			if (Digits() == 0) {
				Fail("invalid number");
			}
		}
		auto text = s_.substr(start, pos_ - start);
		if (text.size() > limits_.max_number_chars) {
			FailAt("number longer than " + std::to_string(limits_.max_number_chars) + " characters", start);
		}
		JsonNode n;
		n.kind = integral ? JsonNode::Kind::Int : JsonNode::Kind::Decimal;
		// -0 is the integer zero, as Plan 2's BigInt reads it.
		n.text = (integral && text == "-0") ? "0" : text;
		return n;
	}
};

void WriteString(std::string &out, const std::string &s) {
	static const char *hex = "0123456789abcdef";
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += hex[c >> 4];
				out += hex[c & 0xf];
			} else {
				out += char(c);
			}
		}
	}
	out += '"';
}

void Write(std::string &out, const JsonNode &v) {
	switch (v.kind) {
	case JsonNode::Kind::Null:
		out += "null";
		break;
	case JsonNode::Kind::Bool:
		out += v.boolean ? "true" : "false";
		break;
	case JsonNode::Kind::Int:
	case JsonNode::Kind::Decimal:
		out += v.text;
		break;
	case JsonNode::Kind::String:
		WriteString(out, v.text);
		break;
	case JsonNode::Kind::Array:
		out += '[';
		for (size_t i = 0; i < v.items.size(); ++i) {
			if (i) {
				out += ',';
			}
			Write(out, v.items[i]);
		}
		out += ']';
		break;
	case JsonNode::Kind::Object:
		out += '{';
		for (size_t i = 0; i < v.members.size(); ++i) {
			if (i) {
				out += ',';
			}
			WriteString(out, v.members[i].first);
			out += ':';
			Write(out, v.members[i].second);
		}
		out += '}';
		break;
	}
}

} // namespace

JsonNode ParseJson(const std::string &text, const JsonLimits &limits) {
	return Parser(text, limits).Document();
}

std::string WriteJson(const JsonNode &value) {
	std::string out;
	Write(out, value);
	return out;
}

} // namespace qod_qdrant
