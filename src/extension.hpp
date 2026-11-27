#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb_static_extension.h"

namespace qod_qdrant {

//! The private static extension qod_qdrant, linked into qod-node through QOD_EXTRA_EXTENSIONS.
class QdrantExtension : public duckdb::Extension {
public:
	void Load(duckdb::ExtensionLoader &loader) override;
	std::string Name() override {
		return "qod_qdrant";
	}
	std::string Version() const override {
		return "0.1.0";
	}
};

} // namespace qod_qdrant

extern "C" int32_t duckdb_extension_qod_qdrant_describe(duckdb_extension_descriptor *descriptor);
