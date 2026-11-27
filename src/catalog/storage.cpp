#include "catalog/storage.hpp"

#include "catalog/catalog.hpp"
#include "catalog/config.hpp"
#include "catalog/secret.hpp"
#include "crypto.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "errors.hpp"
#include "lower/request_guard.hpp"

namespace qod_qdrant {
using namespace duckdb;

bool IsSourceId(const std::string &id) {
	if (id.empty() || id.size() > 128) {
		return false;
	}
	for (auto c : id) {
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

namespace {

std::string StringOption(AttachInfo &info, const std::string &key) {
	for (auto &entry : info.options) {
		if (StringUtil::Lower(entry.first) == key) {
			if (entry.second.IsNull() || entry.second.type().id() != LogicalTypeId::VARCHAR) {
				Raise(SearchCode::ConfigInvalid, StringUtil::Upper(key) + " must be a string");
			}
			return StringValue::Get(entry.second);
		}
	}
	Raise(SearchCode::ConfigInvalid, "ATTACH of a qdrant source needs " + StringUtil::Upper(key));
}

unique_ptr<Catalog> QdrantAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                 AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options) {
	auto state = dynamic_cast<QdrantStorageInfo *>(storage_info.get());
	if (!state) {
		Raise(SearchCode::ConfigInvalid, "the qdrant storage extension is not initialized");
	}
	bool read_only_given = false;
	bool type_given = false;
	for (auto &entry : info.options) {
		auto key = StringUtil::Lower(entry.first);
		if (key == "read_only") {
			read_only_given = true;
		} else if (key == "type") {
			type_given = true;
		} else if (key != "secret" && key != "config" && key != "request_fields") {
			Raise(SearchCode::ConfigInvalid,
			      "ATTACH of a qdrant source takes only TYPE, SECRET, CONFIG, REQUEST_FIELDS and READ_ONLY");
		}
	}
	if (!type_given || StringUtil::Lower(options.db_type) != "qdrant") {
		Raise(SearchCode::ConfigInvalid, "ATTACH of a qdrant source needs TYPE qdrant");
	}
	if (!read_only_given || options.access_mode != AccessMode::READ_ONLY) {
		Raise(SearchCode::ConfigInvalid, "ATTACH of a qdrant source must be READ_ONLY");
	}
	auto secret_name = StringOption(info, "secret");
	auto config_text = StringOption(info, "config");
	// With TYPE given DuckDB keeps the path verbatim (no prefix stripping), so the prefix is checked here.
	auto prefixed = StringUtil::StartsWith(info.path, "qdrant:");
	auto source_id = prefixed ? info.path.substr(7) : std::string();
	if (!prefixed || !IsSourceId(source_id)) {
		Raise(SearchCode::ConfigInvalid, "the ATTACH path must be qdrant:<source id>");
	}
	auto parsed = ParseConfig(config_text, state->allowlist);
	if (!parsed.config) {
		std::string joined;
		for (auto &e : parsed.errors) {
			joined += (joined.empty() ? "" : "; ") + e;
		}
		LogDetail(source_id, SearchCode::ConfigInvalid, joined, "");
		Raise(SearchCode::ConfigInvalid, "the qdrant source config is invalid: " + joined);
	}
	auto source = std::make_shared<QdrantSource>();
	source->source_id = source_id;
	source->alias = name;
	source->endpoint = parsed.config->endpoint;
	source->api_key = ReadApiKey(context, secret_name);
	source->config_digest = qod::Hex(qod::Sha256(config_text));
	source->config = std::move(*parsed.config);
	source->allowlist = state->allowlist;
	for (auto &entry : info.options) {
		if (StringUtil::Lower(entry.first) == "request_fields") {
			source->request_fields = ParseRequestFields(StringOption(info, "request_fields"));
		}
	}
	return make_uniq<QdrantCatalog>(db, std::move(source));
}

unique_ptr<TransactionManager> QdrantCreateTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
                                                              Catalog &) {
	return make_uniq<QdrantTransactionManager>(db);
}

class QdrantStorageExtension : public StorageExtension {
public:
	QdrantStorageExtension() {
		attach = QdrantAttach;
		create_transaction_manager = QdrantCreateTransactionManager;
	}
};

} // namespace

void RegisterStorage(DBConfig &config, std::set<std::string> allowlist) {
	auto info = make_shared_ptr<QdrantStorageInfo>();
	info->allowlist = std::move(allowlist);
	auto extension = make_shared_ptr<QdrantStorageExtension>();
	extension->storage_info = std::move(info);
	StorageExtension::Register(config, "qdrant", std::move(extension));
}

} // namespace qod_qdrant
