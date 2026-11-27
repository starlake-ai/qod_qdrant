// qod_search's check of its query value (ValidateQuery, ResolvePrefetchK), on values the query constructors
// or a hand-spelled struct build: every refusal of contract section 2, in its order and wording.
#include "catalog/config.hpp"
#include "check.hpp"
#include "duckdb.hpp"
#include "extension.hpp"
#include "lower/validate.hpp"

#include <optional>
#include <stdexcept>

using namespace qod_qdrant_test;

namespace {
const std::string kType = "STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], \"text\" VARCHAR, prefetch_k BIGINT, "
                          "branches STRUCT(kind VARCHAR, vector_name VARCHAR, vector FLOAT[], \"text\" VARCHAR)[])";

qod_qdrant::CollectionSpec spec;

std::string Vector(size_t n, const std::string &value = "0.5") {
	std::string out = "[";
	for (size_t i = 0; i < n; ++i) {
		out += (i ? "," : "") + value;
	}
	return out + "]::FLOAT[]";
}

const std::string kDense = "qod_dense('text_dense', " + Vector(768) + ")";

//! The SQL expression's value, computed on `con`.
duckdb::Value ValueOf(duckdb::Connection &con, const std::string &expr) {
	auto result = con.Query("SELECT " + expr);
	if (result->HasError()) {
		throw std::runtime_error(result->GetError());
	}
	return result->Fetch()->GetValue(0, 0);
}

std::string Describe(const qod_qdrant::SearchQuery &q) {
	using Mode = qod_qdrant::SearchQuery::Mode;
	std::string out = q.mode == Mode::Dense ? "dense" : q.mode == Mode::Keyword ? "keyword" : "hybrid";
	if (q.mode == Mode::Hybrid) {
		out += " pk=" + (q.prefetch_k ? std::to_string(*q.prefetch_k) : std::string("NULL"));
	}
	for (auto &b : q.branches) {
		out += b.kind == qod_qdrant::QueryBranch::Kind::Dense ? " [dense " + b.vector_name + "/" + std::to_string(b.vector.size()) + "]"
		                                                       : " [bm25 " + b.vector_name + " '" + b.text + "']";
	}
	return out;
}

//! What qod_search's check makes of the value of `expr`: the accepted query described, or the raw refusal.
std::string Check(duckdb::Connection &con, const std::string &expr, bool lexical = true,
                  qod_qdrant::SearchLimits limits = {}) {
	auto value = ValueOf(con, expr);
	std::string out;
	auto error = ErrorOf([&] { out = Describe(qod_qdrant::ValidateQuery(value, spec, lexical, limits)); });
	return error == "no error" ? out : error;
}

//! A hand-spelled value of the query type: the six fields as written, cast as qod_search's argument is.
std::string Spelled(const std::string &fields) {
	return "CAST({" + fields + "} AS " + kType + ")";
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	spec = qod_qdrant::ParseConfig(ReadFile(fixtures + "sample2.json"), {"https://qdrant.internal:6333"})
	           .config->collections.at(0);
	duckdb::DuckDB db(nullptr);
	db.LoadStaticExtension<qod_qdrant::QdrantExtension>();
	duckdb::Connection con(db);
	const std::string shape = "search_query_unsupported: query must be qod_dense, qod_bm25 or qod_rrf";
	const std::string gate = "search_query_unsupported: keyword search is not enabled for this source";
	Expect("dense", Check(con, kDense), "dense [dense text_dense/768]");
	Expect("keyword, text trimmed", Check(con, "qod_bm25('body_bm25', ' \t merger agreement \n')"),
	       "keyword [bm25 body_bm25 'merger agreement']");
	Expect("hybrid, prefetch_k as given", Check(con, "qod_rrf([qod_bm25('title_bm25', 'merger'), " + kDense + "], prefetch_k := 50)"),
	       "hybrid pk=50 [bm25 title_bm25 'merger'] [dense text_dense/768]");
	Expect("hybrid of two keyword branches", Check(con, "qod_rrf([qod_bm25('body_bm25', 'a'), qod_bm25('title_bm25', 'b')])"),
	       "hybrid pk=NULL [bm25 body_bm25 'a'] [bm25 title_bm25 'b']");
	Expect("a hand-spelled keyword value goes through the same check",
	       Check(con, Spelled("'kind': 'bm25', 'vector_name': 'body_bm25', 'vector': NULL, 'text': 'x', 'prefetch_k': NULL, "
	                          "'branches': NULL")),
	       "keyword [bm25 body_bm25 'x']");
	ExpectTrue("HasKeyword", qod_qdrant::ValidateQuery(ValueOf(con, "qod_bm25('body_bm25', 'x')"), spec, true, {}).HasKeyword() &&
	                             !qod_qdrant::ValidateQuery(ValueOf(con, kDense), spec, true, {}).HasKeyword());

	// 1. The shape.
	Expect("NULL", Check(con, "NULL::" + kType), shape);
	Expect("another type", Check(con, "{'kind': 'dense'}"), shape);
	Expect("unknown kind", Check(con, Spelled("'kind': 'sparse', 'vector_name': 'body_bm25', 'vector': NULL, 'text': 'x', "
	                                          "'prefetch_k': NULL, 'branches': NULL")),
	       shape);
	Expect("NULL kind", Check(con, Spelled("'kind': NULL, 'vector_name': 'body_bm25', 'vector': NULL, 'text': 'x', "
	                                       "'prefetch_k': NULL, 'branches': NULL")),
	       shape);
	Expect("dense with a text", Check(con, Spelled("'kind': 'dense', 'vector_name': 'text_dense', 'vector': " + Vector(768) +
	                                               ", 'text': 'x', 'prefetch_k': NULL, 'branches': NULL")),
	       shape);
	Expect("bm25 with a vector", Check(con, Spelled("'kind': 'bm25', 'vector_name': 'body_bm25', 'vector': [1.0]::FLOAT[], "
	                                                "'text': 'x', 'prefetch_k': NULL, 'branches': NULL")),
	       shape);
	Expect("bm25 with a prefetch_k", Check(con, Spelled("'kind': 'bm25', 'vector_name': 'body_bm25', 'vector': NULL, "
	                                                    "'text': 'x', 'prefetch_k': 5, 'branches': NULL")),
	       shape);
	Expect("NULL text (qod_bm25 of NULL)", Check(con, "qod_bm25('body_bm25', NULL)"), shape);
	Expect("rrf without branches", Check(con, "qod_rrf(NULL)"), shape);
	Expect("rrf with a NULL branch", Check(con, "qod_rrf([NULL, " + kDense + "])"), shape);
	Expect("rrf with a malformed branch",
	       Check(con, Spelled("'kind': 'rrf', 'vector_name': NULL, 'vector': NULL, 'text': NULL, 'prefetch_k': NULL, "
	                          "'branches': [{'kind': 'dense', 'vector_name': 'text_dense', 'vector': NULL, 'text': NULL}]")),
	       shape);
	Expect("shape before the profile gate",
	       Check(con, "qod_rrf([NULL, qod_bm25('body_bm25', 'x')])", false), shape);

	// 2. The profile gate, before any other per-branch check.
	Expect("keyword on exact_isolated", Check(con, "qod_bm25('body_bm25', 'x')", false), gate);
	Expect("hybrid with a keyword branch on exact_isolated",
	       Check(con, "qod_rrf([qod_bm25('body_bm25', 'x'), " + kDense + "])", false), gate);
	Expect("gate before the branch count", Check(con, "qod_rrf([qod_bm25('body_bm25', 'x')])", false), gate);
	Expect("gate before the branch kinds", Check(con, "qod_rrf([qod_rrf([]), qod_bm25('body_bm25', 'x')])", false), gate);
	Expect("gate before the vector name", Check(con, "qod_bm25('no_such', 'x')", false), gate);
	Expect("gate before the text", Check(con, "qod_bm25('body_bm25', '   ')", false), gate);
	Expect("dense on exact_isolated", Check(con, kDense, false), "dense [dense text_dense/768]");
	Expect("dense-only fusion on exact_isolated", Check(con, "qod_rrf([" + kDense + ", " + kDense + "])", false),
	       "hybrid pk=NULL [dense text_dense/768] [dense text_dense/768]");

	// 3. The branch count, then 4. the branch kinds.
	Expect("one branch", Check(con, "qod_rrf([" + kDense + "])"), "search_query_unsupported: qod_rrf takes 2..4 branches");
	Expect("no branch", Check(con, "qod_rrf([])"), "search_query_unsupported: qod_rrf takes 2..4 branches");
	Expect("five branches", Check(con, "qod_rrf([" + kDense + "," + kDense + "," + kDense + "," + kDense + "," + kDense + "])"),
	       "search_query_unsupported: qod_rrf takes 2..4 branches");
	qod_qdrant::SearchLimits two;
	two.max_fusion_branches = 2;
	Expect("search_max_fusion_branches", Check(con, "qod_rrf([" + kDense + "," + kDense + "," + kDense + "])", true, two),
	       "search_query_unsupported: qod_rrf takes 2..2 branches");
	Expect("count before kinds", Check(con, "qod_rrf([qod_rrf([])])"), "search_query_unsupported: qod_rrf takes 2..4 branches");
	Expect("RRF inside RRF", Check(con, "qod_rrf([qod_rrf([" + kDense + ", " + kDense + "]), " + kDense + "])"),
	       "search_query_unsupported: qod_rrf branches must be qod_dense or qod_bm25");
	Expect("kinds before the vector names", Check(con, "qod_rrf([qod_bm25('no_such', 'x'), qod_rrf([])])"),
	       "search_query_unsupported: qod_rrf branches must be qod_dense or qod_bm25");

	// 5. Each branch against the relation, in order.
	Expect("dense on a sparse name", Check(con, "qod_dense('body_bm25', " + Vector(768) + ")"),
	       "search_query_unsupported: qod_dense names a vector the relation does not declare");
	Expect("dense on an unknown name", Check(con, "qod_dense('nope', " + Vector(768) + ")"),
	       "search_query_unsupported: qod_dense names a vector the relation does not declare");
	Expect("bm25 on a dense name", Check(con, "qod_bm25('text_dense', 'x')"),
	       "search_query_unsupported: qod_bm25 names a vector the relation does not declare as sparse");
	Expect("bm25 on an unknown name", Check(con, "qod_bm25('nope', 'x')"),
	       "search_query_unsupported: qod_bm25 names a vector the relation does not declare as sparse");
	Expect("empty text", Check(con, "qod_bm25('body_bm25', '')"), "search_query_unsupported: the query text must be 1..4096 bytes");
	Expect("blank text", Check(con, "qod_bm25('body_bm25', ' \t\n ')"),
	       "search_query_unsupported: the query text must be 1..4096 bytes");
	Expect("4096 bytes after trimming", Check(con, "qod_bm25('body_bm25', '  ' || repeat('a', 4096) || '  ')").substr(0, 31),
	       "keyword [bm25 body_bm25 'aaaaaa");
	Expect("4097 bytes", Check(con, "qod_bm25('body_bm25', repeat('a', 4097))"),
	       "search_query_unsupported: the query text must be 1..4096 bytes");
	Expect("bytes, not characters: 2048 two-byte characters", Check(con, "qod_bm25('body_bm25', repeat('é', 2048))").substr(0, 7),
	       "keyword");
	Expect("2049 two-byte characters", Check(con, "qod_bm25('body_bm25', repeat('é', 2049))"),
	       "search_query_unsupported: the query text must be 1..4096 bytes");
	Expect("only ASCII whitespace is trimmed", Check(con, "qod_bm25('body_bm25', chr(160))"),
	       "keyword [bm25 body_bm25 '\xc2\xa0']");
	qod_qdrant::SearchLimits five;
	five.max_query_text_bytes = 5;
	Expect("search_max_query_text_bytes", Check(con, "qod_bm25('body_bm25', 'merger')", true, five),
	       "search_query_unsupported: the query text must be 1..5 bytes");
	Contains("a dense branch keeps the vector checks", Check(con, "qod_rrf([qod_bm25('body_bm25', 'x'), qod_dense('text_dense', " +
	                                                              Vector(3) + ")])"),
	         "search_query_unsupported: the query vector must hold 768 finite FLOAT values");
	Expect("branches checked in order", Check(con, "qod_rrf([qod_bm25('text_dense', 'x'), qod_dense('nope', " + Vector(768) + ")])"),
	       "search_query_unsupported: qod_bm25 names a vector the relation does not declare as sparse");

	// 6. prefetch_k, once k is resolved.
	qod_qdrant::SearchLimits limits;
	auto resolve = [&](std::optional<int64_t> requested, int64_t k, const qod_qdrant::SearchLimits &l) {
		std::string out;
		auto error = ErrorOf([&] { out = std::to_string(qod_qdrant::ResolvePrefetchK(requested, k, l)); });
		return error == "no error" ? out : error;
	};
	Expect("NULL is max(k, 100)", resolve(std::nullopt, 10, limits), "100");
	Expect("NULL with a large k", resolve(std::nullopt, 500, limits), "500");
	qod_qdrant::SearchLimits fifty;
	fifty.max_prefetch_k = 50;
	Expect("NULL capped at search_max_prefetch_k", resolve(std::nullopt, 10, fifty), "50");
	Expect("a cap below k refuses the default too", resolve(std::nullopt, 60, fifty),
	       "search_query_unsupported: prefetch_k must be in 60..50");
	Expect("prefetch_k equal to k", resolve(10, 10, limits), "10");
	Expect("prefetch_k below k", resolve(5, 10, limits), "search_query_unsupported: prefetch_k must be in 10..1000");
	Expect("prefetch_k above the cap", resolve(1001, 10, limits), "search_query_unsupported: prefetch_k must be in 10..1000");
	Expect("prefetch_k at the cap", resolve(1000, 10, limits), "1000");
	return Finish();
}
