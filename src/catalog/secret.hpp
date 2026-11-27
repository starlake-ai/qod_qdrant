#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <string>

namespace qod_qdrant {

constexpr const char *kSecretType = "qdrant";

//! Secret type `qdrant`, provider `config`, one parameter `api_key` (required, redacted).
void RegisterSecretType(duckdb::ExtensionLoader &loader);

//! The api_key of the qdrant secret `secret_name`; search_config_invalid when there is none.
//! Runs inside a statement: `context` must have an active transaction.
std::string ReadApiKey(duckdb::ClientContext &context, const std::string &secret_name);

} // namespace qod_qdrant
