#pragma once

#include "duckdb.hpp"
#include "duckdb/storage/storage_extension.hpp"

#include <set>
#include <string>

namespace qod_qdrant {

//! The deployment allowlist, read once at extension load (QOD_QDRANT_ALLOWED_ENDPOINTS).
struct QdrantStorageInfo : public duckdb::StorageExtensionInfo {
	std::set<std::string> allowlist;
};

//! Registers storage type `qdrant`: ATTACH 'qdrant:<sourceId>' AS a (TYPE qdrant, SECRET s, CONFIG '<json>',
//! READ_ONLY) with exactly these options, the config re-validated with Plan 2's rules.
void RegisterStorage(duckdb::DBConfig &config, std::set<std::string> allowlist);

//! [A-Za-z0-9_-]{1,128}, as Plan 2 bounds FederatedSource.id.
bool IsSourceId(const std::string &id);

} // namespace qod_qdrant
