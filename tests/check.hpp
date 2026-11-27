// Minimal test helpers, in the style of core's qod-node tests: every check counts, every failure
// prints, the program's exit status is the verdict.
#pragma once

#include "duckdb/common/error_data.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace qod_qdrant_test {

inline int &Failures() {
	static int failures = 0;
	return failures;
}
inline int &Checks() {
	static int checks = 0;
	return checks;
}

inline void Expect(const std::string &what, const std::string &got, const std::string &expected) {
	++Checks();
	if (got != expected) {
		std::cerr << "FAIL " << what << ":\n  expected: " << expected << "\n  got:      " << got << std::endl;
		++Failures();
	}
}

inline void ExpectTrue(const std::string &what, bool ok) {
	Expect(what, ok ? "true" : "false", "true");
}

//! `got` holds `needle` and, when given, not `absent`.
inline void Contains(const std::string &what, const std::string &got, const std::string &needle,
                     const std::string &absent = "") {
	++Checks();
	if (got.find(needle) == std::string::npos || (!absent.empty() && got.find(absent) != std::string::npos)) {
		std::cerr << "FAIL " << what << ": expected to contain '" << needle << "'"
		          << (absent.empty() ? "" : " and not '" + absent + "'") << ", got: " << got << std::endl;
		++Failures();
	}
}

//! The raw message of what `f` throws (DuckDB's, without its type prefix), or "no error".
template <class F>
std::string ErrorOf(F f) {
	try {
		f();
	} catch (std::exception &e) {
		return duckdb::ErrorData(e).RawMessage();
	}
	return "no error";
}

inline int Finish() {
	std::cout << Checks() << " checks, " << Failures() << " failures" << std::endl;
	return Failures() == 0 ? 0 : 1;
}

//! The file's bytes, one trailing newline dropped (fixture files end with one, the Scala strings do not).
inline std::string ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("cannot read " + path);
	}
	std::stringstream buffer;
	buffer << in.rdbuf();
	auto text = buffer.str();
	if (!text.empty() && text.back() == '\n') {
		text.pop_back();
	}
	return text;
}

//! `text` with `from`, which must occur exactly once, replaced by `to` (Plan 2's QdrantFixtures.patch).
inline std::string Patch(const std::string &text, const std::string &from, const std::string &to) {
	auto at = text.find(from);
	if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) {
		throw std::runtime_error("fixture must hold exactly one: " + from);
	}
	return text.substr(0, at) + to + text.substr(at + from.size());
}

} // namespace qod_qdrant_test
