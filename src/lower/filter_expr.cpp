#include "lower/filter_expr.hpp"

#include <stdexcept>

namespace qod_qdrant {

bool FieldValue::operator==(const FieldValue &other) const {
	if (type != other.type) {
		return false;
	}
	switch (type) {
	case FieldType::Varchar:
		return text == other.text;
	case FieldType::Bigint:
		return integer == other.integer;
	case FieldType::Boolean:
		return boolean == other.boolean;
	case FieldType::Double:
		return real == other.real;
	}
	return false;
}

FilterExpr FilterExpr::True() {
	return FilterExpr();
}
FilterExpr FilterExpr::False() {
	FilterExpr e;
	e.kind = Kind::False;
	return e;
}
FilterExpr FilterExpr::Match(size_t field, FieldValue value) {
	FilterExpr e;
	e.kind = Kind::Match;
	e.field = field;
	e.values.push_back(std::move(value));
	return e;
}
FilterExpr FilterExpr::In(size_t field, std::vector<FieldValue> values) {
	FilterExpr e;
	e.kind = Kind::In;
	e.field = field;
	e.values = std::move(values);
	return e;
}
FilterExpr FilterExpr::And(std::vector<FilterExpr> children) {
	FilterExpr e;
	e.kind = Kind::And;
	e.children = std::move(children);
	return e;
}
FilterExpr FilterExpr::Or(std::vector<FilterExpr> children) {
	FilterExpr e;
	e.kind = Kind::Or;
	e.children = std::move(children);
	return e;
}

FilterExpr Simplify(const FilterExpr &expr) {
	switch (expr.kind) {
	case FilterExpr::Kind::True:
	case FilterExpr::Kind::False:
	case FilterExpr::Kind::Match:
		return expr;
	case FilterExpr::Kind::In:
		return expr.values.empty() ? FilterExpr::False() : expr;
	case FilterExpr::Kind::And:
	case FilterExpr::Kind::Or: {
		bool is_and = expr.kind == FilterExpr::Kind::And;
		auto absorbing = is_and ? FilterExpr::Kind::False : FilterExpr::Kind::True;
		auto neutral = is_and ? FilterExpr::Kind::True : FilterExpr::Kind::False;
		std::vector<FilterExpr> kept;
		for (auto &child : expr.children) {
			auto s = Simplify(child);
			if (s.kind == absorbing) {
				return s;
			}
			if (s.kind == neutral) {
				continue;
			}
			if (s.kind == expr.kind) {
				for (auto &grandchild : s.children) {
					kept.push_back(grandchild);
				}
			} else {
				kept.push_back(std::move(s));
			}
		}
		if (kept.empty()) {
			return is_and ? FilterExpr::True() : FilterExpr::False();
		}
		if (kept.size() == 1) {
			return kept[0];
		}
		return is_and ? FilterExpr::And(std::move(kept)) : FilterExpr::Or(std::move(kept));
	}
	}
	return expr;
}

size_t NodeCount(const FilterExpr &expr) {
	switch (expr.kind) {
	case FilterExpr::Kind::True:
	case FilterExpr::Kind::False:
		return 0;
	case FilterExpr::Kind::Match:
	case FilterExpr::Kind::In:
		return 1;
	default: {
		size_t n = 1;
		for (auto &child : expr.children) {
			n += NodeCount(child);
		}
		return n;
	}
	}
}

void CollectFields(const FilterExpr &expr, std::set<size_t> &fields) {
	if (expr.kind == FilterExpr::Kind::Match || expr.kind == FilterExpr::Kind::In) {
		fields.insert(expr.field);
	}
	for (auto &child : expr.children) {
		CollectFields(child, fields);
	}
}

JsonNode ValueJson(const FieldValue &value) {
	switch (value.type) {
	case FieldType::Varchar:
		return JsonNode::String(value.text);
	case FieldType::Bigint:
		return JsonNode::Int(value.integer);
	case FieldType::Boolean:
		return JsonNode::Bool(value.boolean);
	case FieldType::Double:
		break;
	}
	throw std::logic_error("a DOUBLE value is never lowered");
}

namespace {

JsonNode Condition(const FilterExpr &expr, const CollectionSpec &spec) {
	switch (expr.kind) {
	case FilterExpr::Kind::Match:
		return JsonNode::Object({{"key", JsonNode::String(spec.fields.at(expr.field).path)},
		                         {"match", JsonNode::Object({{"value", ValueJson(expr.values.at(0))}})}});
	case FilterExpr::Kind::In: {
		// Simplify folds an empty IN to FALSE; never send an empty "any" whose backend meaning is unpinned.
		if (expr.values.empty()) {
			throw std::logic_error("an empty IN is simplified away before serialization");
		}
		std::vector<JsonNode> any;
		for (auto &v : expr.values) {
			any.push_back(ValueJson(v));
		}
		return JsonNode::Object({{"key", JsonNode::String(spec.fields.at(expr.field).path)},
		                         {"match", JsonNode::Object({{"any", JsonNode::Array(std::move(any))}})}});
	}
	case FilterExpr::Kind::And:
	case FilterExpr::Kind::Or: {
		// Simplify folds an empty AND to TRUE and an empty OR to FALSE; an empty "must" or "should" would not
		// carry that meaning reliably.
		if (expr.children.empty()) {
			throw std::logic_error("an empty AND or OR is simplified away before serialization");
		}
		std::vector<JsonNode> children;
		for (auto &child : expr.children) {
			children.push_back(Condition(child, spec));
		}
		return JsonNode::Object(
		    {{expr.kind == FilterExpr::Kind::And ? "must" : "should", JsonNode::Array(std::move(children))}});
	}
	default:
		throw std::logic_error("TRUE and FALSE are simplified away before serialization");
	}
}

} // namespace

JsonNode FilterJson(const FilterExpr &expr, const CollectionSpec &spec) {
	if (expr.kind == FilterExpr::Kind::Match || expr.kind == FilterExpr::Kind::In) {
		return JsonNode::Object({{"must", JsonNode::Array({Condition(expr, spec)})}});
	}
	return Condition(expr, spec);
}

bool Evaluate(const FilterExpr &expr, const std::vector<std::optional<FieldValue>> &row) {
	switch (expr.kind) {
	case FilterExpr::Kind::True:
		return true;
	case FilterExpr::Kind::False:
		return false;
	case FilterExpr::Kind::Match:
	case FilterExpr::Kind::In: {
		if (expr.field >= row.size() || !row[expr.field]) {
			return false;
		}
		for (auto &v : expr.values) {
			if (*row[expr.field] == v) {
				return true;
			}
		}
		return false;
	}
	case FilterExpr::Kind::And:
		for (auto &child : expr.children) {
			if (!Evaluate(child, row)) {
				return false;
			}
		}
		return true;
	case FilterExpr::Kind::Or:
		for (auto &child : expr.children) {
			if (Evaluate(child, row)) {
				return true;
			}
		}
		return false;
	}
	return false;
}

} // namespace qod_qdrant
