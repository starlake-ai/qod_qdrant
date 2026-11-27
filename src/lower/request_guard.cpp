#include "lower/request_guard.hpp"

#include "ast/request.hpp"
#include "crypto.hpp"
#include "errors.hpp"

#include <algorithm>
#include <cmath>

namespace qod_qdrant {

namespace {

using Kind = JsonNode::Kind;
using Members = std::vector<std::pair<std::string, JsonNode>>;

[[noreturn]] void Refuse(const std::string &path, const std::string &rule) {
	Raise(SearchCode::QueryUnsupported, path + ": " + rule);
}

std::string Join(const std::string &path, const std::string &key) {
	return path.empty() ? key : path + "." + key;
}

std::string Index(const std::string &path, size_t i) {
	return path + "[" + std::to_string(i) + "]";
}

bool IsNumber(const JsonNode &v) {
	return v.Is(Kind::Int) || v.Is(Kind::Decimal);
}

bool IntIn(const JsonNode &v, int64_t lo, int64_t hi) {
	int64_t n = 0;
	return v.Is(Kind::Int) && v.ToInt64(n) && n >= lo && n <= hi;
}

std::string Range(int64_t lo, int64_t hi) {
	return "must be an integer in " + std::to_string(lo) + ".." + std::to_string(hi);
}

//! A level searches a keyword (BM25) index when its `using` names a declared sparse vector: Qdrant then applies
//! the IDF modifier, so the level carries the statistics corpus.
bool IsKeywordLevel(const JsonNode &level, const CollectionSpec &spec) {
	auto u = level.Find("using");
	return u && u->Is(Kind::String) && spec.FindSparseVector(u->text);
}

bool IsDocument(const JsonNode &v) {
	return v.Is(Kind::Object) && (v.Find("text") || v.Find("model"));
}

class Checker {
public:
	Checker(const CollectionSpec &spec, const std::vector<std::string> &enabled, const SearchLimits &limits)
	    : spec_(spec), enabled_(enabled), limits_(limits) {
	}

	CheckedRequest Run(const JsonNode &request) {
		if (!request.Is(Kind::Object)) {
			Refuse("request", "must be a JSON object");
		}
		Level(request, "", 0);
		out_.request = request;
		return std::move(out_);
	}

private:
	const CollectionSpec &spec_;
	const std::vector<std::string> &enabled_;
	const SearchLimits &limits_;
	CheckedRequest out_;
	int64_t branches_ = 0;
	std::string level_path_;
	std::string using_;

	bool Enabled(const std::string &logical) const {
		return std::find(enabled_.begin(), enabled_.end(), logical) != enabled_.end();
	}

	//! A payload key the request reads: a declared field's path, recorded as a filter use.
	void KeyText(const std::string &path, const std::string &key) {
		for (size_t i = 0; i < spec_.fields.size(); i++) {
			if (spec_.fields[i].path == key) {
				out_.filter_fields.insert(i);
				return;
			}
		}
		Raise(SearchCode::FieldDenied, path + ": names no declared field");
	}

	void Key(const std::string &path, const JsonNode &v) {
		if (!v.Is(Kind::String)) {
			Refuse(path, "must be a string");
		}
		KeyText(path, v.text);
	}

	void Level(const JsonNode &level, const std::string &path, int64_t depth) {
		auto saved_path = level_path_;
		auto saved_using = using_;
		level_path_ = path;
		using_.clear();
		if (auto u = level.Find("using")) {
			if (!u->Is(Kind::String)) {
				Refuse(Join(path, "using"), "must be a string");
			}
			using_ = u->text;
			if (!spec_.FindVector(using_) && !spec_.FindSparseVector(using_)) {
				Refuse(Join(path, "using"), "must name a declared vector");
			}
			out_.vectors.insert(using_);
		}
		if (IsKeywordLevel(level, spec_)) {
			out_.keyword = true;
		}
		// Refused whatever else the level holds, before any other check names something else.
		for (auto *refused : {"lookup_from", "shard_key", "offset"}) {
			if (level.Find(refused)) {
				Refuse(Join(path, refused), "not supported");
			}
		}
		bool top = depth == 0;
		for (auto &m : level.members) {
			auto &k = m.first;
			auto &v = m.second;
			auto p = Join(path, k);
			if (k == "using") {
				continue;
			} else if (k == "query") {
				Query(v, p);
			} else if (k == "prefetch") {
				Prefetch(v, p, depth);
			} else if (k == "filter") {
				Filter(v, p);
			} else if (k == "params") {
				Params(v, p);
			} else if (k == "score_threshold") {
				if (!IsNumber(v)) {
					Refuse(p, "must be a number");
				}
			} else if (k == "limit") {
				if (!top && !IntIn(v, 1, limits_.max_prefetch_k)) {
					Refuse(p, Range(1, limits_.max_prefetch_k));
				}
			} else if (top && (k == "with_payload" || k == "with_vector")) {
				// Overwritten by WriteRequest: the selected declared paths, no vectors.
			} else if (k == "lookup_from" || k == "shard_key" || k == "offset") {
				Refuse(p, "not supported");
			} else if (!Enabled(k)) {
				Refuse(p, "not supported");
			}
		}
		level_path_ = saved_path;
		using_ = saved_using;
	}

	void Prefetch(const JsonNode &v, const std::string &p, int64_t depth) {
		if (depth + 1 > limits_.max_prefetch_depth) {
			Refuse(p, "at most " + std::to_string(limits_.max_prefetch_depth) + " levels");
		}
		std::vector<std::pair<std::string, const JsonNode *>> entries;
		if (v.Is(Kind::Array)) {
			for (size_t i = 0; i < v.items.size(); i++) {
				entries.emplace_back(Index(p, i), &v.items[i]);
			}
		} else {
			entries.emplace_back(p, &v);
		}
		branches_ += static_cast<int64_t>(entries.size());
		if (branches_ > limits_.max_fusion_branches) {
			Refuse(p, "at most " + std::to_string(limits_.max_fusion_branches) + " branches");
		}
		for (auto &e : entries) {
			if (!e.second->Is(Kind::Object)) {
				Refuse(e.first, "must be an object");
			}
			Level(*e.second, e.first, depth + 1);
		}
	}

	void Params(const JsonNode &v, const std::string &p) {
		if (!v.Is(Kind::Object)) {
			Refuse(p, "must be an object");
		}
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			if (m.first == "hnsw_ef") {
				if (!IntIn(m.second, 1, limits_.max_hnsw_ef)) {
					Refuse(q, Range(1, limits_.max_hnsw_ef));
				}
			} else if (m.first == "exact") {
				if (!m.second.Is(Kind::Bool) || m.second.boolean) {
					Refuse(q, "must be false");
				}
			} else if (m.first == "indexed_only") {
				if (!m.second.Is(Kind::Bool)) {
					Refuse(q, "must be a boolean");
				}
			} else if (m.first == "quantization") {
				if (!m.second.Is(Kind::Object)) {
					Refuse(q, "must be an object");
				}
				for (auto &n : m.second.members) {
					auto r = Join(q, n.first);
					double d = 0;
					if (n.first == "ignore" || n.first == "rescore") {
						if (!n.second.Is(Kind::Bool)) {
							Refuse(r, "must be a boolean");
						}
					} else if (n.first == "oversampling") {
						if (!n.second.ToDouble(d) || !std::isfinite(d) || d < 1 ||
						    d > double(limits_.max_oversampling)) {
							Refuse(r, "must be a number in 1.." + std::to_string(limits_.max_oversampling));
						}
					} else {
						Refuse(r, "not supported");
					}
				}
			} else if (m.first == "idf") {
				Refuse(q, "set by qod-search");
			} else if (!Enabled("params." + m.first)) {
				Refuse(q, "not supported");
			}
		}
	}

	void Filter(const JsonNode &v, const std::string &p) {
		if (!v.Is(Kind::Object)) {
			Refuse(p, "must be an object");
		}
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			if (m.first == "must" || m.first == "should" || m.first == "must_not") {
				Conditions(m.second, q);
			} else if (m.first == "min_should") {
				if (!m.second.Is(Kind::Object)) {
					Refuse(q, "must be an object");
				}
				for (auto &n : m.second.members) {
					if (n.first == "conditions") {
						Conditions(n.second, Join(q, n.first));
					} else if (n.first != "min_count") {
						Refuse(Join(q, n.first), "not supported");
					}
				}
			} else {
				Refuse(q, "not supported");
			}
		}
	}

	void Conditions(const JsonNode &v, const std::string &p) {
		if (v.Is(Kind::Array)) {
			for (size_t i = 0; i < v.items.size(); i++) {
				Condition(v.items[i], Index(p, i));
			}
		} else {
			Condition(v, p);
		}
	}

	//! A condition, as a closed shape (an operator beside a condition must never pass: Qdrant 1.20.2 then reads
	//! the object as that operator, in a formula). A field condition: a declared `key` and one or more of match,
	//! range, geo_bounding_box, geo_radius, geo_polygon, values_count (objects), is_empty, is_null (booleans).
	//! Alone: is_empty or is_null of {key}, has_id (point ids; it only narrows, under F), has_vector (a declared
	//! vector, reported for its lineage). A nested filter: only must, should, must_not, min_should. Qdrant's
	//! `nested` is refused: its key and filter are relative to an array the config does not map.
	void Condition(const JsonNode &c, const std::string &p) {
		if (!c.Is(Kind::Object) || c.members.empty()) {
			Refuse(p, "must be a condition object");
		}
		if (c.Find("key")) {
			FieldCondition(c, p);
			return;
		}
		if (c.members.size() == 1) {
			auto &name = c.members[0].first;
			auto &v = c.members[0].second;
			auto q = Join(p, name);
			if (name == "is_empty" || name == "is_null") {
				if (!v.Is(Kind::Object) || v.members.size() != 1 || !v.Find("key")) {
					Refuse(q, "must be {key}");
				}
				Key(Join(q, "key"), *v.Find("key"));
				return;
			}
			if (name == "has_id") {
				bool ok = v.Is(Kind::Array) && !v.items.empty();
				for (auto &id : v.items) {
					ok = ok && IsPointId(id);
				}
				if (!ok) {
					Refuse(q, "must be an array of point ids");
				}
				return;
			}
			if (name == "has_vector") {
				if (!v.Is(Kind::String) || (!spec_.FindVector(v.text) && !spec_.FindSparseVector(v.text))) {
					Refuse(q, "must name a declared vector");
				}
				out_.vectors.insert(v.text);
				return;
			}
			if (name == "nested") {
				Refuse(q, "not supported");
			}
		}
		bool filter = true;
		for (auto &m : c.members) {
			filter = filter && (m.first == "must" || m.first == "should" || m.first == "must_not" || m.first == "min_should");
		}
		if (!filter) {
			Refuse(p, "not supported");
		}
		Filter(c, p);
	}

	void FieldCondition(const JsonNode &c, const std::string &p) {
		bool kind = false;
		for (auto &m : c.members) {
			auto q = Join(p, m.first);
			if (m.first == "key") {
				Key(q, m.second);
			} else if (m.first == "match" || m.first == "range" || m.first == "geo_bounding_box" ||
			           m.first == "geo_radius" || m.first == "geo_polygon" || m.first == "values_count") {
				if (!m.second.Is(Kind::Object)) {
					Refuse(q, "must be an object");
				}
				kind = true;
			} else if (m.first == "is_empty" || m.first == "is_null") {
				if (!m.second.Is(Kind::Bool)) {
					Refuse(q, "must be a boolean");
				}
				kind = true;
			} else {
				Refuse(q, "not supported");
			}
		}
		if (!kind) {
			Refuse(p, "a field condition needs match, range, geo_*, values_count, is_empty or is_null");
		}
	}

	void PointId(const JsonNode &v) {
		auto text = WriteJson(v);
		for (auto &seen : out_.point_ids) {
			if (WriteJson(seen) == text) {
				return;
			}
		}
		out_.point_ids.push_back(v);
		if (static_cast<int64_t>(out_.point_ids.size()) > limits_.max_request_ids) {
			Refuse("query", "names more than " + std::to_string(limits_.max_request_ids) + " point ids");
		}
	}

	//! A vector, a point id, a sparse vector or a document: what nearest, recommend, discover and context take.
	void Input(const JsonNode &v, const std::string &p) {
		if (using_.empty()) {
			// Without `using`, Qdrant searches its unnamed default vector, which a config cannot declare: its
			// lineage would never reach core's column policies.
			Refuse(Join(level_path_, "using"), "must name a declared vector for this query");
		}
		if (IsPointId(v)) {
			PointId(v);
		} else if (v.Is(Kind::Array)) {
			auto vector = spec_.FindVector(using_);
			if (!vector) {
				Refuse(Join(level_path_, "using"), "must name a declared dense vector for a vector query");
			}
			bool ok = static_cast<int64_t>(v.items.size()) == vector->dimensions;
			for (auto &item : v.items) {
				double d = 0;
				ok = ok && item.ToDouble(d) && std::isfinite(d);
			}
			if (!ok) {
				Refuse(p, "must hold " + std::to_string(vector->dimensions) + " finite numbers");
			}
		} else if (IsDocument(v)) {
			Document(v, p);
		} else if (v.Is(Kind::Object) && v.Find("indices")) {
			Sparse(v, p);
		} else {
			Refuse(p, "must be a vector, a point id, a sparse vector or a document");
		}
	}

	const SparseVectorSpec &SparseUsing() {
		auto sparse = spec_.FindSparseVector(using_);
		if (!sparse) {
			Refuse(Join(level_path_, "using"), "must name a declared sparse vector");
		}
		return *sparse;
	}

	void Sparse(const JsonNode &v, const std::string &p) {
		SparseUsing();
		auto indices = v.Find("indices");
		auto values = v.Find("values");
		bool ok = v.members.size() == 2 && indices && values && indices->Is(Kind::Array) && values->Is(Kind::Array) &&
		          indices->items.size() == values->items.size();
		for (size_t i = 0; ok && i < indices->items.size(); i++) {
			double d = 0;
			ok = indices->items[i].Is(Kind::Int) && values->items[i].ToDouble(d) && std::isfinite(d);
		}
		if (!ok) {
			Refuse(p, "must be {indices, values} of equal length");
		}
	}

	//! A BM25 document: text, model qdrant/bm25, options. The using vector's declared options win: a declared key
	//! sent with another value is refused, a key the index does not declare is kept (WriteRequest merges them).
	void Document(const JsonNode &v, const std::string &p) {
		auto &sparse = SparseUsing();
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			if (m.first == "text") {
				if (!m.second.Is(Kind::String) ||
				    static_cast<int64_t>(m.second.text.size()) > limits_.max_query_text_bytes) {
					Refuse(q, "must be a string of at most " + std::to_string(limits_.max_query_text_bytes) + " bytes");
				}
			} else if (m.first == "model") {
				if (!m.second.Is(Kind::String) || m.second.text != kBm25Model) {
					Refuse(q, std::string("must be ") + kBm25Model);
				}
			} else if (m.first == "options") {
				if (!m.second.Is(Kind::Object)) {
					Refuse(q, "must be an object");
				}
				for (auto &declared : sparse.options.members) {
					auto sent = m.second.Find(declared.first);
					if (sent && WriteJson(*sent) != WriteJson(declared.second)) {
						Refuse(Join(q, declared.first), "differs from the index's declared options");
					}
				}
			} else {
				Refuse(q, "not supported");
			}
		}
		if (!v.Find("text")) {
			Refuse(p, "a document needs text");
		}
	}

	void Pairs(const JsonNode &v, const std::string &p) {
		auto one = [&](const JsonNode &pair, const std::string &q) {
			if (!pair.Is(Kind::Object)) {
				Refuse(q, "must be {positive, negative}");
			}
			for (auto &m : pair.members) {
				if (m.first != "positive" && m.first != "negative") {
					Refuse(Join(q, m.first), "not supported");
				}
				Input(m.second, Join(q, m.first));
			}
		};
		if (v.Is(Kind::Array)) {
			for (size_t i = 0; i < v.items.size(); i++) {
				one(v.items[i], Index(p, i));
			}
		} else {
			one(v, p);
		}
	}

	void Inputs(const JsonNode &v, const std::string &p) {
		if (!v.Is(Kind::Array)) {
			Refuse(p, "must be an array");
		}
		for (size_t i = 0; i < v.items.size(); i++) {
			Input(v.items[i], Index(p, i));
		}
	}

	void Query(const JsonNode &v, const std::string &p) {
		if (!v.Is(Kind::Object) || IsDocument(v) || v.Find("indices")) {
			Input(v, p);
			return;
		}
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			auto &x = m.second;
			if (m.first == "nearest") {
				Input(x, q);
			} else if (m.first == "mmr" && v.Find("nearest")) {
				Mmr(x, q);
			} else if (m.first == "recommend" || m.first == "discover") {
				if (!x.Is(Kind::Object)) {
					Refuse(q, "must be an object");
				}
				for (auto &n : x.members) {
					auto r = Join(q, n.first);
					if (m.first == "recommend" && (n.first == "positive" || n.first == "negative")) {
						Inputs(n.second, r);
					} else if (m.first == "recommend" && n.first == "strategy") {
						if (!n.second.Is(Kind::String)) {
							Refuse(r, "must be a string");
						}
					} else if (m.first == "discover" && n.first == "target") {
						Input(n.second, r);
					} else if (m.first == "discover" && n.first == "context") {
						Pairs(n.second, r);
					} else {
						Refuse(r, "not supported");
					}
				}
			} else if (m.first == "context") {
				Pairs(x, q);
			} else if (m.first == "order_by") {
				OrderBy(x, q);
			} else if (m.first == "fusion") {
				if (!x.Is(Kind::String) || (x.text != "rrf" && x.text != "dbsf")) {
					Refuse(q, "must be rrf or dbsf");
				}
			} else if (m.first == "rrf") {
				Rrf(x, q);
			} else if (m.first == "formula") {
				Formula(x, q);
			} else if (m.first == "defaults" && v.Find("formula")) {
				if (!x.Is(Kind::Object)) {
					Refuse(q, "must be an object");
				}
				for (auto &n : x.members) {
					KeyText(Join(q, n.first), n.first);
				}
			} else if (m.first == "sample") {
				if (!x.Is(Kind::String) || x.text != "random") {
					Refuse(q, "must be random");
				}
			} else if (!Enabled("query." + m.first)) {
				Refuse(q, "not supported");
			}
		}
	}

	void Mmr(const JsonNode &v, const std::string &p) {
		if (!v.Is(Kind::Object)) {
			Refuse(p, "must be an object");
		}
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			if (m.first == "diversity") {
				if (!IsNumber(m.second)) {
					Refuse(q, "must be a number");
				}
			} else if (m.first == "candidates_limit") {
				if (!IntIn(m.second, 1, limits_.max_prefetch_k)) {
					Refuse(q, Range(1, limits_.max_prefetch_k));
				}
			} else {
				Refuse(q, "not supported");
			}
		}
	}

	void OrderBy(const JsonNode &v, const std::string &p) {
		if (v.Is(Kind::String)) {
			Key(p, v);
			return;
		}
		if (!v.Is(Kind::Object) || !v.Find("key")) {
			Refuse(p, "must be a field path or {key, direction, start_from}");
		}
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			if (m.first == "key") {
				Key(q, m.second);
			} else if (m.first == "direction") {
				if (!m.second.Is(Kind::String) || (m.second.text != "asc" && m.second.text != "desc")) {
					Refuse(q, "must be asc or desc");
				}
			} else if (m.first == "start_from") {
				if (!IsNumber(m.second) && !m.second.Is(Kind::String)) {
					Refuse(q, "must be a number or a datetime");
				}
			} else {
				Refuse(q, "not supported");
			}
		}
	}

	void Rrf(const JsonNode &v, const std::string &p) {
		if (!v.Is(Kind::Object)) {
			Refuse(p, "must be an object");
		}
		for (auto &m : v.members) {
			auto q = Join(p, m.first);
			if (m.first == "k") {
				if (!IntIn(m.second, 1, 1000000)) {
					Refuse(q, Range(1, 1000000));
				}
			} else if (m.first == "weights") {
				bool ok = m.second.Is(Kind::Array);
				for (auto &w : m.second.items) {
					double d = 0;
					ok = ok && w.ToDouble(d) && std::isfinite(d);
				}
				if (!ok) {
					Refuse(q, "must be an array of numbers");
				}
			} else {
				Refuse(q, "not supported");
			}
		}
	}

	//! A formula expression. A string is a variable: `$score` (or `$score[i]`), else a payload key, which must name
	//! a declared field; condition objects name declared fields too. Anything the grammar below does not know is
	//! refused unless enabled as query.formula.<operator>.
	void Formula(const JsonNode &e, const std::string &p) {
		if (IsNumber(e)) {
			return;
		}
		if (e.Is(Kind::String)) {
			if (e.text.rfind("$score", 0) != 0) {
				Key(p, e);
			}
			return;
		}
		if (!e.Is(Kind::Object)) {
			Refuse(p, "not supported");
		}
		if (e.members.size() != 1) {
			Condition(e, p);
			return;
		}
		auto &op = e.members[0].first;
		auto &x = e.members[0].second;
		auto q = Join(p, op);
		auto fields = [&](std::initializer_list<const char *> exprs, std::initializer_list<const char *> numbers) {
			if (!x.Is(Kind::Object)) {
				Refuse(q, "must be an object");
			}
			for (auto &m : x.members) {
				bool known = false;
				for (auto *n : exprs) {
					if (m.first == n) {
						Formula(m.second, Join(q, m.first));
						known = true;
					}
				}
				for (auto *n : numbers) {
					if (m.first == n) {
						if (!IsNumber(m.second)) {
							Refuse(Join(q, m.first), "must be a number");
						}
						known = true;
					}
				}
				if (!known) {
					Refuse(Join(q, m.first), "not supported");
				}
			}
		};
		if (op == "sum" || op == "mult") {
			if (!x.Is(Kind::Array)) {
				Refuse(q, "must be an array");
			}
			for (size_t i = 0; i < x.items.size(); i++) {
				Formula(x.items[i], Index(q, i));
			}
		} else if (op == "neg" || op == "abs" || op == "sqrt" || op == "exp" || op == "log10" || op == "ln") {
			Formula(x, q);
		} else if (op == "div") {
			fields({"left", "right"}, {"by_zero_default"});
		} else if (op == "pow") {
			fields({"base", "exponent"}, {});
		} else if (op == "exp_decay" || op == "gauss_decay" || op == "lin_decay") {
			fields({"x", "target"}, {"scale", "midpoint"});
		} else if (op == "datetime") {
			if (!x.Is(Kind::String)) {
				Refuse(q, "must be a string");
			}
		} else if (op == "datetime_key") {
			Key(q, x);
		} else if (op == "geo_distance") {
			if (!x.Is(Kind::Object)) {
				Refuse(q, "must be an object");
			}
			for (auto &m : x.members) {
				if (m.first == "to") {
					Key(Join(q, m.first), m.second);
				} else if (m.first != "origin") {
					Refuse(Join(q, m.first), "not supported");
				}
			}
		} else if (op == "is_empty" || op == "is_null" || op == "has_id" || op == "has_vector" || op == "nested" ||
		           op == "must" || op == "should" || op == "must_not" || op == "min_should") {
			Condition(e, p);
		} else if (!Enabled("query.formula." + op)) {
			Refuse(q, "not supported");
		}
	}
};

class Writer {
public:
	Writer(const CollectionSpec &spec, const FilterExpr &filter, const FilterExpr &corpus)
	    : spec_(spec), filter_(filter), corpus_(corpus) {
	}

	JsonNode Level(const JsonNode &level, bool top, const std::set<size_t> &selected, int64_t k) const {
		std::string using_vector;
		if (auto u = level.Find("using")) {
			using_vector = u->text;
		}
		Members out;
		const JsonNode *user_filter = nullptr;
		const JsonNode *user_params = nullptr;
		for (auto &m : level.members) {
			if (m.first == "filter") {
				user_filter = &m.second;
			} else if (m.first == "params") {
				user_params = &m.second;
			} else if (top && (m.first == "limit" || m.first == "with_payload" || m.first == "with_vector")) {
				continue;
			} else if (m.first == "prefetch") {
				if (m.second.Is(Kind::Array)) {
					std::vector<JsonNode> entries;
					for (auto &e : m.second.items) {
						entries.push_back(Level(e, false, {}, 0));
					}
					out.emplace_back("prefetch", JsonNode::Array(std::move(entries)));
				} else {
					out.emplace_back("prefetch", Level(m.second, false, {}, 0));
				}
			} else if (m.first == "query") {
				out.emplace_back("query", Query(m.second, using_vector));
			} else {
				out.push_back(m);
			}
		}
		bool has_f = filter_.kind != FilterExpr::Kind::True;
		if (has_f && user_filter) {
			out.emplace_back("filter",
			                 JsonNode::Object({{"must", JsonNode::Array({FilterJson(filter_, spec_), *user_filter})}}));
		} else if (has_f) {
			out.emplace_back("filter", FilterJson(filter_, spec_));
		} else if (user_filter) {
			out.emplace_back("filter", *user_filter);
		}
		Members params = user_params ? user_params->members : Members {};
		if (IsKeywordLevel(level, spec_) && corpus_.kind != FilterExpr::Kind::True) {
			params.emplace_back("idf", JsonNode::Object({{"corpus", FilterJson(corpus_, spec_)}}));
		}
		if (!params.empty()) {
			out.emplace_back("params", JsonNode::Object(std::move(params)));
		}
		if (top) {
			out.emplace_back("limit", JsonNode::Int(k));
			auto include = IncludePaths(spec_, selected);
			if (include.empty()) {
				out.emplace_back("with_payload", JsonNode::Bool(false));
			} else {
				std::vector<JsonNode> paths;
				for (auto &p : include) {
					paths.push_back(JsonNode::String(p));
				}
				out.emplace_back("with_payload", JsonNode::Object({{"include", JsonNode::Array(std::move(paths))}}));
			}
			out.emplace_back("with_vector", JsonNode::Bool(false));
		}
		return JsonNode::Object(std::move(out));
	}

private:
	const CollectionSpec &spec_;
	const FilterExpr &filter_;
	const FilterExpr &corpus_;

	//! A document with the using vector's declared options merged in: the sent keys first, the declared keys the
	//! request left out after them.
	JsonNode Document(const JsonNode &doc, const std::string &using_vector) const {
		auto sparse = spec_.FindSparseVector(using_vector);
		Members out;
		Members options;
		for (auto &m : doc.members) {
			if (m.first == "options") {
				options = m.second.members;
			} else {
				out.push_back(m);
			}
		}
		if (sparse) {
			for (auto &declared : sparse->options.members) {
				bool sent = false;
				for (auto &o : options) {
					sent |= o.first == declared.first;
				}
				if (!sent) {
					options.push_back(declared);
				}
			}
		}
		if (!options.empty()) {
			out.emplace_back("options", JsonNode::Object(std::move(options)));
		}
		return JsonNode::Object(std::move(out));
	}

	//! An input position (what Checker::Input checked): a document gets the declared options, anything else
	//! is copied.
	JsonNode Input(const JsonNode &v, const std::string &using_vector) const {
		return IsDocument(v) ? Document(v, using_vector) : v;
	}

	JsonNode Inputs(const JsonNode &v, const std::string &using_vector) const {
		if (!v.Is(Kind::Array)) {
			return Input(v, using_vector);
		}
		std::vector<JsonNode> items;
		for (auto &i : v.items) {
			items.push_back(Input(i, using_vector));
		}
		return JsonNode::Array(std::move(items));
	}

	JsonNode Pairs(const JsonNode &v, const std::string &using_vector) const {
		auto one = [&](const JsonNode &pair) {
			Members out;
			for (auto &m : pair.members) {
				out.emplace_back(m.first, Input(m.second, using_vector));
			}
			return JsonNode::Object(std::move(out));
		};
		if (!v.Is(Kind::Array)) {
			return v.Is(Kind::Object) ? one(v) : v;
		}
		std::vector<JsonNode> items;
		for (auto &i : v.items) {
			items.push_back(i.Is(Kind::Object) ? one(i) : i);
		}
		return JsonNode::Array(std::move(items));
	}

	//! The query with the declared options written into every document at an input position: the query itself,
	//! nearest, recommend positive and negative, discover target and context, context pairs.
	JsonNode Query(const JsonNode &q, const std::string &using_vector) const {
		if (!q.Is(Kind::Object) || IsDocument(q)) {
			return Input(q, using_vector);
		}
		Members out;
		for (auto &m : q.members) {
			if (m.first == "nearest") {
				out.emplace_back(m.first, Input(m.second, using_vector));
			} else if (m.first == "recommend" && m.second.Is(Kind::Object)) {
				Members r;
				for (auto &n : m.second.members) {
					r.emplace_back(n.first, n.first == "positive" || n.first == "negative" ? Inputs(n.second, using_vector)
					                                                                     : n.second);
				}
				out.emplace_back(m.first, JsonNode::Object(std::move(r)));
			} else if (m.first == "discover" && m.second.Is(Kind::Object)) {
				Members d;
				for (auto &n : m.second.members) {
					d.emplace_back(n.first, n.first == "target"    ? Input(n.second, using_vector)
					                        : n.first == "context" ? Pairs(n.second, using_vector)
					                                               : n.second);
				}
				out.emplace_back(m.first, JsonNode::Object(std::move(d)));
			} else if (m.first == "context") {
				out.emplace_back(m.first, Pairs(m.second, using_vector));
			} else {
				out.push_back(m);
			}
		}
		return JsonNode::Object(std::move(out));
	}
};

} // namespace

CheckedRequest CheckRequest(const JsonNode &request, const CollectionSpec &spec,
                            const std::vector<std::string> &enabled, const SearchLimits &limits) {
	return Checker(spec, enabled, limits).Run(request);
}

GuardedRequest WriteRequest(const CheckedRequest &checked, const CollectionSpec &spec, const FilterExpr &filter,
                            const FilterExpr &corpus, const std::set<size_t> &selected, int64_t k) {
	GuardedRequest out;
	out.body = WriteJson(Writer(spec, filter, corpus).Level(checked.request, true, selected, k));
	out.digest = qod::Hex(qod::Sha256(out.body));
	return out;
}

bool IsRequestFieldPattern(const std::string &pattern) {
	size_t segments = 0;
	size_t start = 0;
	while (true) {
		auto dot = pattern.find('.', start);
		auto segment = pattern.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
		if (segment.empty()) {
			return false;
		}
		for (auto c : segment) {
			if (!((c >= 'a' && c <= 'z') || c == '_')) {
				return false;
			}
		}
		if (++segments > 5) {
			return false;
		}
		if (dot == std::string::npos) {
			return true;
		}
		start = dot + 1;
	}
}

std::vector<std::string> ParseRequestFields(const std::string &text) {
	const char *rule = "REQUEST_FIELDS must be a JSON array of request path patterns";
	JsonNode parsed;
	try {
		parsed = ParseJson(text, kConfigJsonLimits);
	} catch (std::exception &) {
		Raise(SearchCode::ConfigInvalid, rule);
	}
	if (!parsed.Is(Kind::Array)) {
		Raise(SearchCode::ConfigInvalid, rule);
	}
	std::vector<std::string> out;
	for (auto &item : parsed.items) {
		if (!item.Is(Kind::String) || !IsRequestFieldPattern(item.text)) {
			Raise(SearchCode::ConfigInvalid, rule);
		}
		out.push_back(item.text);
	}
	return out;
}

} // namespace qod_qdrant
