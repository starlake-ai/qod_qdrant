#pragma once

#include "arrow/payload.hpp"
#include "catalog/catalog.hpp"
#include "lower/request_guard.hpp"
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "lower/filter_expr.hpp"
#include "lower/validate.hpp"
#include "protected_scan.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace qod_qdrant {

extern const char *const kSchemaMismatch;
//! The tenant text of a response over the parse budget (ResponseBudgetError): search_budget_exceeded.
extern const char *const kResponseOverBudget;

//! The bind data of both scans: the plain scroll (unbounded) and qod_search (bounded, top-k). Bind does no
//! I/O. Core hands it the scan's row policies at post_bind; it lowers P AND U (and, for a search, the
//! statistics corpus P), keeps the result as the only filters it will ever send, and seals them
//! together with the policy digest, the config digest, the source's guarantee, the collection generation
//! and schema version.
class QdrantScanData : public duckdb::FunctionData, public qod::ProtectedScan {
public:
	QdrantScanData(QdrantTableEntry &table_p, bool bounded_p) : table(table_p), bounded(bounded_p) {
	}

	QdrantTableEntry &table;
	bool bounded;
	//! qod_search only: U and its fields, the validated query (prefetch_k resolved for hybrid), k.
	FilterExpr user_filter;
	std::set<size_t> user_fields;
	SearchQuery query;
	int64_t k = 0;
	//! qod_search(request := ...) only, instead of `query`: the checked raw request, and its key for the seal
	//! and Equals (the checked request text and the source's enabled request fields).
	std::optional<CheckedRequest> raw;
	std::string raw_key;

	//! Set by AcceptRowPolicies.
	bool accepted = false;
	FilterExpr remote_filter;
	//! C = P: the IDF statistics scope of every keyword stage (bounded scans only, TRUE otherwise). TRUE also
	//! when no row policy applies: the keyword stages then carry no corpus (whole-collection statistics).
	FilterExpr corpus_filter;
	std::set<size_t> policy_fields;
	std::string accepted_digest;
	std::string sealed_config_digest;
	std::string sealed_guarantee;
	int64_t sealed_generation = 0;
	int64_t sealed_schema_version = 0;
	std::string seal;

	duckdb::unique_ptr<duckdb::FunctionData> Copy() const override;
	bool Equals(const duckdb::FunctionData &other) const override;

	bool IsBounded() const override {
		return bounded;
	}
	qod::LoweringResult AcceptRowPolicies(duckdb::ClientContext &context, const duckdb::LogicalGet &get,
	                                      const std::vector<qod::RowPolicy> &policies,
	                                      const std::vector<duckdb::unique_ptr<duckdb::Expression>> &predicates) override;
	std::vector<qod::FieldUse> FieldUses() const override;
	//! What AcceptRowPolicies does once the policies are lowered (`p`, nullopt when they cannot be): F = P AND
	//! U, the corpus C = P, the budgets, and the seal. A host without row policies calls it with TRUE.
	qod::LoweringResult LowerFilters(duckdb::ClientContext &context, std::optional<FilterExpr> p,
	                                 const std::vector<qod::RowPolicy> &policies);
	//! A host without row policies (standalone DuckDB): lowers U alone and seals, at bind, since no core hands
	//! the scan its policies. Does nothing in the QoD host.
	void LowerWithoutPolicies(duckdb::ClientContext &context);

	//! SHA-256 over the accepted policy digest, the config digest, the guarantee, generation, schema version,
	//! the filter, the corpus, the query's stages (kind, vector name, text) and prefetch_k, and k.
	std::string ComputeSeal() const;
	//! Before any I/O: RequireSealed, then search_policy_changed unless core sealed what was accepted and
	//! the attached table still has the sealed digest, guarantee, generation and schema version.
	void CheckAtExecution(duckdb::ClientContext &context) const;
	//! The fields the remote filter reads (decoded for the local recheck).
	std::set<size_t> FilterFields() const;
};

//! The scan's bind data, checked: Cast<> only asserts in debug builds, and a bind data that is not ours must
//! fail the statement with a coded error (search_query_unsupported), never an INTERNAL one.
const QdrantScanData &ScanDataOf(duckdb::optional_ptr<const duckdb::FunctionData> bind_data);

//! get_table_entry of both scans. A bind data that is not ours gets no entry: core matches no policy to it,
//! and ScanDataOf refuses it at init before any I/O.
duckdb::optional_ptr<duckdb::TableCatalogEntry> TableEntryOf(duckdb::optional_ptr<const duckdb::FunctionData> bind_data);

//! Execution state shared by both scans: decoded rows not yet emitted, the scroll cursor.
struct ScanState : public duckdb::GlobalTableFunctionState {
	std::vector<duckdb::column_t> column_ids;
	std::set<size_t> decode_fields;
	std::vector<DecodedRow> rows;
	size_t next = 0;
	bool emitted_any = false;
	bool done = false;
	std::optional<JsonNode> offset;
	int64_t read_points = 0;
};

//! The fields to decode: projected columns plus the filter's fields.
std::set<size_t> DecodeFields(const QdrantScanData &data, const std::vector<duckdb::column_t> &column_ids);

//! Decodes a point and rechecks it against the remote filter; search_schema_mismatch otherwise.
DecodedRow CheckedRow(const QdrantPoint &point, const QdrantScanData &data, const std::set<size_t> &decode_fields);

//! Moves buffered rows into `output` from index `out` on, up to STANDARD_VECTOR_SIZE.
void EmitBuffered(ScanState &state, const CollectionSpec &spec, duckdb::DataChunk &output, duckdb::idx_t &out);

} // namespace qod_qdrant
