#include "extension.hpp"

#include "catalog/config.hpp"
#include "catalog/secret.hpp"
#include "catalog/storage.hpp"
#include "duckdb/main/config.hpp"
#include "errors.hpp"
#include "host_flags.hpp"
#include "lower/search.hpp"
#include "settings.hpp"

#include <cstdlib>

namespace qod_qdrant {
using namespace duckdb;

void QdrantExtension::Load(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	RegisterSettings(config);
	RegisterSecretType(loader);
	auto raw = std::getenv(kAllowlistEnv);
	auto allowlist = ParseAllowlist(raw);
	if (!raw && HostIsStandalone()) {
		// Standalone with no allowlist: the DuckDB user owns this process, so every https origin is accepted.
		// Set, the variable means exactly what it means in QoD; QoD itself never takes this branch.
		allowlist.first = {kAnyHttpsOrigin};
	}
	if (allowlist.second > 0) {
		LogDetail("-", SearchCode::ConfigInvalid,
		          std::string(kAllowlistEnv) + ": " + std::to_string(allowlist.second) + " entries are not origins, ignored",
		          "");
	}
	RegisterStorage(config, std::move(allowlist.first));
	RegisterSearchFunctions(loader);
}

} // namespace qod_qdrant

extern "C" int32_t duckdb_extension_qod_qdrant_describe(duckdb_extension_descriptor *descriptor) {
	return duckdb::StaticExtensionDescriber<qod_qdrant::QdrantExtension>::Describe(descriptor);
}
