#include "catalog/secret.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "errors.hpp"

namespace qod_qdrant {
using namespace duckdb;

namespace {

unique_ptr<BaseSecret> CreateFromConfig(ClientContext &, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.emplace_back("qdrant:");
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	bool has_key = false;
	for (auto &option : input.options) {
		if (StringUtil::Lower(option.first) != "api_key") {
			Raise(SearchCode::ConfigInvalid, "a qdrant secret takes only api_key");
		}
		if (option.second.IsNull() || option.second.ToString().empty()) {
			Raise(SearchCode::ConfigInvalid, "a qdrant secret needs a non-empty api_key");
		}
		secret->secret_map[Identifier("api_key")] = Value(option.second.ToString());
		has_key = true;
	}
	if (!has_key) {
		Raise(SearchCode::ConfigInvalid, "a qdrant secret needs a non-empty api_key");
	}
	secret->redact_keys = {Identifier("api_key")};
	return std::move(secret);
}

} // namespace

void RegisterSecretType(ExtensionLoader &loader) {
	SecretType type;
	type.name = Identifier(kSecretType);
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "config";
	type.extension = "qod_qdrant";
	loader.RegisterSecretType(type);

	CreateSecretFunction function = {kSecretType, Identifier("config"), CreateFromConfig};
	function.named_parameters["api_key"] = LogicalType::VARCHAR;
	loader.RegisterFunction(function);
}

std::string ReadApiKey(ClientContext &context, const std::string &secret_name) {
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = SecretManager::Get(context).GetSecretByName(transaction, secret_name);
	if (!entry || entry->secret->GetType() != Identifier(kSecretType)) {
		Raise(SearchCode::ConfigInvalid, "SECRET must name a secret of type qdrant");
	}
	auto kv = dynamic_cast<const KeyValueSecret *>(entry->secret.get());
	Value key;
	if (!kv || !kv->TryGetValue(Identifier("api_key"), key) || key.IsNull() || key.ToString().empty()) {
		Raise(SearchCode::ConfigInvalid, "the qdrant secret holds no api_key");
	}
	return key.ToString();
}

} // namespace qod_qdrant
