#include "arrow/payload.hpp"

#include "errors.hpp"
#include "lower/lowering.hpp"

namespace qod_qdrant {
using namespace duckdb;

column_t IdColumn() {
	return VIRTUAL_COLUMN_START;
}

column_t ScoreColumn() {
	return VIRTUAL_COLUMN_START + 1;
}

virtual_column_map_t VirtualColumns() {
	virtual_column_map_t result;
	result.insert(make_pair(IdColumn(), TableColumn(Identifier("_qod_id"), LogicalType::VARCHAR)));
	result.insert(make_pair(ScoreColumn(), TableColumn(Identifier("_qod_score"), LogicalType::DOUBLE)));
	return result;
}

namespace {

constexpr const char *kUnknownColumn = "the scan requests a column the table does not have";

bool KnownColumn(column_t id, const CollectionSpec &spec) {
	return id == IdColumn() || id == ScoreColumn() || id < spec.fields.size();
}

//! The value at `field`'s dotted path, or nullptr when a key on the way is absent or null. A present
//! intermediate that is not an object refuses: Qdrant reads a path through an array element by element,
//! so a filter may have matched a row this decoder would otherwise read as NULL.
const JsonNode *Lookup(const JsonNode &payload, const FieldSpec &field) {
	const JsonNode *node = &payload;
	const auto &path = field.path;
	size_t start = 0;
	while (true) {
		if (node->Is(JsonNode::Kind::Null)) {
			return nullptr;
		}
		if (!node->Is(JsonNode::Kind::Object)) {
			throw ResponseError("field " + field.name + ": its payload path crosses a value that is not an object");
		}
		auto dot = path.find('.', start);
		node = node->Find(path.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
		if (!node) {
			return nullptr;
		}
		if (dot == std::string::npos) {
			return node;
		}
		start = dot + 1;
	}
}

} // namespace

std::string FormatPointId(const JsonNode &id) {
	if (!IsPointId(id)) {
		throw ResponseError("point id is neither an unsigned integer nor a UUID");
	}
	uint64_t n = 0;
	if (id.ToUint64(n)) {
		return std::to_string(n);
	}
	std::string out = id.text;
	for (auto &c : out) {
		if (c >= 'A' && c <= 'F') {
			c = char(c - 'A' + 'a');
		}
	}
	return out;
}

DecodedRow DecodePoint(const QdrantPoint &point, const CollectionSpec &spec, const std::set<size_t> &fields) {
	DecodedRow row;
	row.id = FormatPointId(point.id);
	row.score = point.score;
	row.values.resize(spec.fields.size());
	for (auto index : fields) {
		auto &field = spec.fields.at(index);
		auto value = Lookup(point.payload, field);
		if (!value || value->Is(JsonNode::Kind::Null)) {
			if (field.required) {
				throw ResponseError("required field " + field.name + " is missing or null");
			}
			continue;
		}
		FieldValue v;
		v.type = field.type;
		bool ok = false;
		switch (field.type) {
		case FieldType::Varchar:
			ok = value->Is(JsonNode::Kind::String);
			v.text = value->text;
			break;
		case FieldType::Bigint:
			ok = value->ToInt64(v.integer);
			break;
		case FieldType::Boolean:
			ok = value->Is(JsonNode::Kind::Bool);
			v.boolean = value->boolean;
			break;
		case FieldType::Double:
			ok = value->ToDouble(v.real);
			break;
		}
		if (!ok) {
			throw ResponseError("field " + field.name + " is not a " + FieldTypeSql(field.type) + " value");
		}
		row.values[index] = std::move(v);
	}
	return row;
}

Value ToDuckValue(const std::optional<FieldValue> &value, FieldType type) {
	if (!value) {
		return Value(SqlType(type));
	}
	switch (type) {
	case FieldType::Varchar:
		return Value(value->text);
	case FieldType::Bigint:
		return Value::BIGINT(value->integer);
	case FieldType::Boolean:
		return Value::BOOLEAN(value->boolean);
	case FieldType::Double:
		return Value::DOUBLE(value->real);
	}
	return Value();
}

void CheckScanColumns(const std::vector<column_t> &column_ids, const CollectionSpec &spec) {
	for (auto id : column_ids) {
		if (!KnownColumn(id, spec)) {
			Raise(SearchCode::QueryUnsupported, kUnknownColumn);
		}
	}
}

void EmitRow(const DecodedRow &row, const std::vector<column_t> &column_ids, const CollectionSpec &spec,
             DataChunk &output, idx_t row_index) {
	for (idx_t c = 0; c < column_ids.size(); ++c) {
		auto id = column_ids[c];
		Value value;
		if (id == IdColumn()) {
			value = Value(row.id);
		} else if (id == ScoreColumn()) {
			value = row.score ? Value::DOUBLE(*row.score) : Value(LogicalType::DOUBLE);
		} else if (id < spec.fields.size()) {
			value = ToDuckValue(row.values[id], spec.fields[id].type);
		} else {
			// CheckScanColumns refused it at init; never an INTERNAL error, which would invalidate the database.
			Raise(SearchCode::QueryUnsupported, kUnknownColumn);
		}
		output.data[c].SetValue(row_index, value);
	}
}

} // namespace qod_qdrant
