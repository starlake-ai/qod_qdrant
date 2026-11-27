#include "ast/request.hpp"

namespace qod_qdrant {

namespace {

JsonNode PayloadSelector(const std::vector<std::string> &include) {
	if (include.empty()) {
		return JsonNode::Bool(false);
	}
	std::vector<JsonNode> paths;
	for (auto &p : include) {
		paths.push_back(JsonNode::String(p));
	}
	return JsonNode::Object({{"include", JsonNode::Array(std::move(paths))}});
}

JsonNode VectorJson(const std::vector<float> &vector) {
	std::vector<JsonNode> items;
	items.reserve(vector.size());
	for (auto f : vector) {
		items.push_back(JsonNode::Float(f));
	}
	return JsonNode::Array(std::move(items));
}

//! Qdrant's server-side BM25 inference of `text` on the sparse vector `using_vector`: tokenized with the
//! options that vector declares (query.options, left out when it declares none), Qdrant's defaults otherwise.
JsonNode TextQuery(const std::string &text, const std::string &using_vector, const CollectionSpec &spec) {
	std::vector<std::pair<std::string, JsonNode>> members {{"text", JsonNode::String(text)},
	                                                        {"model", JsonNode::String(kBm25Model)}};
	auto vector = spec.FindSparseVector(using_vector);
	if (vector && !vector->options.members.empty()) {
		members.emplace_back("options", vector->options);
	}
	return JsonNode::Object(std::move(members));
}

//! The remote filter F, left out when F is TRUE (no row policy and no filter_sql): Qdrant reads a request
//! without a filter as every point of the collection. FilterJson refuses a FALSE F (never sent).
void AddFilter(std::vector<std::pair<std::string, JsonNode>> &members, const FilterExpr &filter,
               const CollectionSpec &spec) {
	if (filter.kind != FilterExpr::Kind::True) {
		members.emplace_back("filter", FilterJson(filter, spec));
	}
}

//! The IDF statistics scope (spike record): every keyword stage carries it, a hybrid top level never does.
//! When C is TRUE (no row policy) the statistics cover the whole collection: the stage carries no `params`
//! at all, the spike's "no corpus" request shape, rather than a match-all corpus filter. `params` holds
//! nothing else, so nothing is lost. FilterJson refuses a FALSE C (F = C AND U is then FALSE: never sent).
void AddCorpus(std::vector<std::pair<std::string, JsonNode>> &members, const FilterExpr &corpus,
               const CollectionSpec &spec) {
	if (corpus.kind != FilterExpr::Kind::True) {
		auto idf = JsonNode::Object({{"corpus", FilterJson(corpus, spec)}});
		members.emplace_back("params", JsonNode::Object({{"idf", std::move(idf)}}));
	}
}

void Keys(const JsonNode &object, const std::string &where, std::initializer_list<const char *> allowed) {
	for (auto &member : object.members) {
		bool known = false;
		for (auto k : allowed) {
			known |= member.first == k;
		}
		if (!known) {
			throw ResponseError(where + ": unknown key " + ShownKey(member.first));
		}
	}
}

//! The member `key` of `object`, mutable, so values can be moved out of the parsed document.
JsonNode *Member(JsonNode &object, const std::string &key) {
	return const_cast<JsonNode *>(object.Find(key));
}

JsonNode &Result(JsonNode &top) {
	if (!top.Is(JsonNode::Kind::Object)) {
		throw ResponseError("response: must be an object");
	}
	Keys(top, "response", {"result", "status", "time", "usage"});
	auto status = top.Find("status");
	if (!status || !status->Is(JsonNode::Kind::String) || status->text != "ok") {
		throw ResponseError("response: status must be \"ok\"");
	}
	auto result = Member(top, "result");
	if (!result || !result->Is(JsonNode::Kind::Object)) {
		throw ResponseError("response: result must be an object");
	}
	return *result;
}

JsonNode Parse(const std::string &body) {
	try {
		return ParseJson(body, kResponseJsonLimits);
	} catch (JsonBudgetError &e) {
		throw ResponseBudgetError(std::string("response exceeds the parse budget: ") + e.what());
	} catch (JsonError &e) {
		throw ResponseError(std::string("response is not valid JSON: ") + e.what());
	}
}

std::vector<QdrantPoint> Points(JsonNode &result, bool scored) {
	auto points = Member(result, "points");
	if (!points || !points->Is(JsonNode::Kind::Array)) {
		throw ResponseError("result: points must be an array");
	}
	std::vector<QdrantPoint> out;
	out.reserve(points->items.size());
	for (size_t i = 0; i < points->items.size(); ++i) {
		// Ids and payloads move out of the parsed document, so a page is held once, not twice.
		auto &p = points->items[i];
		auto where = "points[" + std::to_string(i) + "]";
		if (!p.Is(JsonNode::Kind::Object)) {
			throw ResponseError(where + ": must be an object");
		}
		Keys(p, where, {"id", "version", "score", "payload", "vector", "shard_key", "order_value"});
		QdrantPoint point;
		auto id = Member(p, "id");
		if (!id || !IsPointId(*id)) {
			throw ResponseError(where + ": id must be an unsigned integer or a UUID string");
		}
		point.id = std::move(*id);
		if (auto score = p.Find("score")) {
			double s = 0;
			if (!score->ToDouble(s)) {
				throw ResponseError(where + ": score must be a number");
			}
			point.score = s;
		} else if (scored) {
			throw ResponseError(where + ": score is missing");
		}
		if (auto payload = Member(p, "payload")) {
			if (!payload->Is(JsonNode::Kind::Object) && !payload->Is(JsonNode::Kind::Null)) {
				throw ResponseError(where + ": payload must be an object");
			}
			point.payload = std::move(*payload);
		}
		out.push_back(std::move(point));
	}
	return out;
}

} // namespace

bool IsPointId(const JsonNode &id) {
	uint64_t n = 0;
	if (id.Is(JsonNode::Kind::Int)) {
		return id.ToUint64(n);
	}
	if (!id.Is(JsonNode::Kind::String) || id.text.size() != 36) {
		return false;
	}
	for (size_t i = 0; i < id.text.size(); ++i) {
		char c = id.text[i];
		bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
		bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (hyphen ? c != '-' : !hex) {
			return false;
		}
	}
	return true;
}

std::string ScrollPath(const CollectionSpec &spec) {
	return "/collections/" + spec.collection + "/points/scroll";
}

std::string QueryPath(const CollectionSpec &spec) {
	return "/collections/" + spec.collection + "/points/query";
}

std::vector<std::string> IncludePaths(const CollectionSpec &spec, const std::set<size_t> &fields) {
	std::vector<std::string> out;
	for (size_t i = 0; i < spec.fields.size(); ++i) {
		if (fields.count(i)) {
			out.push_back(spec.fields[i].path);
		}
	}
	return out;
}

std::string ScrollRequestJson(const ScrollRequest &request, const CollectionSpec &spec) {
	std::vector<std::pair<std::string, JsonNode>> members;
	AddFilter(members, request.filter, spec);
	members.emplace_back("limit", JsonNode::Int(request.limit));
	if (request.offset) {
		members.emplace_back("offset", *request.offset);
	}
	members.emplace_back("with_payload", PayloadSelector(request.include));
	members.emplace_back("with_vector", JsonNode::Bool(false));
	return WriteJson(JsonNode::Object(std::move(members)));
}

std::string QueryRequestJson(const QueryRequest &request, const CollectionSpec &spec) {
	std::vector<std::pair<std::string, JsonNode>> members;
	members.emplace_back("query", VectorJson(request.vector));
	members.emplace_back("using", JsonNode::String(request.using_vector));
	AddFilter(members, request.filter, spec);
	members.emplace_back("limit", JsonNode::Int(request.limit));
	members.emplace_back("with_payload", PayloadSelector(request.include));
	members.emplace_back("with_vector", JsonNode::Bool(false));
	return WriteJson(JsonNode::Object(std::move(members)));
}

std::string KeywordRequestJson(const KeywordRequest &request, const CollectionSpec &spec) {
	std::vector<std::pair<std::string, JsonNode>> members;
	members.emplace_back("query", TextQuery(request.text, request.using_vector, spec));
	members.emplace_back("using", JsonNode::String(request.using_vector));
	AddFilter(members, request.filter, spec);
	AddCorpus(members, request.corpus, spec);
	members.emplace_back("limit", JsonNode::Int(request.limit));
	members.emplace_back("with_payload", PayloadSelector(request.include));
	members.emplace_back("with_vector", JsonNode::Bool(false));
	return WriteJson(JsonNode::Object(std::move(members)));
}

std::string HybridRequestJson(const HybridRequest &request, const CollectionSpec &spec) {
	std::vector<JsonNode> prefetch;
	for (auto &stage : request.stages) {
		std::vector<std::pair<std::string, JsonNode>> members;
		members.emplace_back("query", stage.keyword ? TextQuery(stage.text, stage.using_vector, spec) : VectorJson(stage.vector));
		members.emplace_back("using", JsonNode::String(stage.using_vector));
		AddFilter(members, request.filter, spec);
		if (stage.keyword) {
			AddCorpus(members, request.corpus, spec);
		}
		members.emplace_back("limit", JsonNode::Int(request.prefetch_limit));
		prefetch.push_back(JsonNode::Object(std::move(members)));
	}
	std::vector<std::pair<std::string, JsonNode>> members;
	members.emplace_back("prefetch", JsonNode::Array(std::move(prefetch)));
	members.emplace_back("query", JsonNode::Object({{"rrf", JsonNode::Object({})}}));
	AddFilter(members, request.filter, spec);
	members.emplace_back("limit", JsonNode::Int(request.limit));
	members.emplace_back("with_payload", PayloadSelector(request.include));
	members.emplace_back("with_vector", JsonNode::Bool(false));
	return WriteJson(JsonNode::Object(std::move(members)));
}

ScrollPage ParseScrollResponse(const std::string &body) {
	auto top = Parse(body);
	auto &result = Result(top);
	Keys(result, "result", {"points", "next_page_offset"});
	ScrollPage page;
	page.points = Points(result, false);
	// Fail closed: a result without the key is not read as the last page.
	auto next = Member(result, "next_page_offset");
	if (!next) {
		throw ResponseError("result: next_page_offset is missing");
	}
	if (!next->Is(JsonNode::Kind::Null)) {
		if (!IsPointId(*next)) {
			throw ResponseError("result: next_page_offset must be a point id or null");
		}
		page.next_offset = std::move(*next);
	}
	return page;
}

std::vector<QdrantPoint> ParseQueryResponse(const std::string &body) {
	auto top = Parse(body);
	auto &result = Result(top);
	Keys(result, "result", {"points"});
	return Points(result, true);
}

} // namespace qod_qdrant
