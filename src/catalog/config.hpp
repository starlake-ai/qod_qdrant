#pragma once

#include "ast/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace qod_qdrant {

//! The deployment allowlist: comma separated exact origins (contract section 1).
constexpr const char *kAllowlistEnv = "QOD_QDRANT_ALLOWED_ENDPOINTS";
//! The allowlist entry that accepts every https origin: what the standalone host uses when kAllowlistEnv is
//! unset. ParseAllowlist never yields it (it is not an origin), so a deployment cannot set it by accident.
constexpr const char *kAnyHttpsOrigin = "*";
constexpr size_t kMaxConfigBytes = 262144;
constexpr size_t kMaxCollections = 64;
constexpr size_t kMaxFields = 256;
constexpr size_t kMaxVectors = 8;
constexpr int64_t kMaxDimensions = 65536;
constexpr size_t kMaxModelVersionChars = 128;
constexpr size_t kMaxSparseVectors = 8;
//! The source profiles (contract section 1): keyword and hybrid search run only under filtered_retrieval.
constexpr const char *kExactIsolated = "exact_isolated";
constexpr const char *kFilteredRetrieval = "filtered_retrieval";
//! The one lexical model slice 2 accepts: Qdrant's server-side BM25.
constexpr const char *kBm25Model = "qdrant/bm25";

//! The SQL type of a registered payload field (slice 1). Double is projection-only: never lowered.
enum class FieldType { Varchar, Bigint, Boolean, Double };
const char *FieldTypeSql(FieldType type);

struct FieldSpec {
	std::string name;
	std::string path;
	FieldType type;
	bool required;
};

struct VectorSpec {
	std::string name;
	int64_t dimensions = 0;
	std::string datatype;
	std::vector<std::string> depends_on;
	std::string model_version;
};

//! A sparse vector indexed server-side with kBm25Model (slice 2). `depends_on` are declared field names.
//! `options` are Qdrant's BM25 text-processing options the index was built with, kept as declared (an empty
//! object when absent); every BM25 query on this vector sends them as query.options.
struct SparseVectorSpec {
	std::string name;
	std::string model;
	std::vector<std::string> depends_on;
	JsonNode options = JsonNode::Object({});
};

//! How deep a sparse vector's options may nest, the options object itself counting as 1 (QdrantConfig.scala).
constexpr size_t kMaxOptionsDepth = 8;

struct CollectionSpec {
	std::string schema;
	std::string table;
	std::string collection;
	int64_t generation = 0;
	int64_t schema_version = 0;
	std::vector<FieldSpec> fields;
	std::vector<VectorSpec> vectors;
	//! Empty when the config declares none (the key is optional).
	std::vector<SparseVectorSpec> sparse_vectors;

	//! The index of field `name` in `fields`, or -1.
	int FieldIndex(const std::string &name) const;
	//! The dense vector `name`, or nullptr (also for a sparse vector's name).
	const VectorSpec *FindVector(const std::string &name) const;
	//! The sparse vector `name`, or nullptr (also for a dense vector's name).
	const SparseVectorSpec *FindSparseVector(const std::string &name) const;
};

//! A source's CONFIG (contract section 1). Only ParseConfig builds one with no errors.
struct SourceConfig {
	std::string endpoint;
	std::string api_key_secret;
	std::vector<CollectionSpec> collections;
	//! kExactIsolated (also when the config leaves the key out) or kFilteredRetrieval.
	std::string guarantee = kExactIsolated;

	//! Whether keyword (BM25) stages may run against this source: only under filtered_retrieval.
	bool AllowsLexical() const;
};

struct ConfigResult {
	std::optional<SourceConfig> config;
	//! Every rule broken, in Plan 2's order and wording; each names a JSON path, never a value.
	std::vector<std::string> errors;
};

//! Why `endpoint` is not an `https://host[:port]` origin (Plan 2's QdrantEndpoint.syntaxError), or none.
std::optional<std::string> EndpointSyntaxError(const std::string &endpoint);
//! The origins of an allowlist value and how many entries were dropped as not being origins. Null or
//! blank is the empty allowlist, which accepts no endpoint.
std::pair<std::set<std::string>, size_t> ParseAllowlist(const char *raw);
//! Plan 2's QdrantConfig.parse: the config `raw` declares, or every rule it breaks.
ConfigResult ParseConfig(const std::string &raw, const std::set<std::string> &allowlist);

} // namespace qod_qdrant
