#pragma once

#include "catalog/config.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "transport/remote.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace qod_qdrant {

//! What one ATTACH established: the validated config, the key read from the SECRET, the allowlist
//! snapshot and the digest of the CONFIG text (the seal's source generation).
struct QdrantSource {
	std::string source_id;
	std::string alias;
	std::string endpoint;
	std::string api_key;
	std::string config_digest;
	SourceConfig config;
	std::set<std::string> allowlist;
	//! The tenant's enabled request fields (the ATTACH's REQUEST_FIELDS, none when absent): the paths a raw
	//! qod_search(request := ...) may pass to Qdrant unchecked.
	std::vector<std::string> request_fields;

	RemoteTarget Target() const;
};

//! One registered collection: `<alias>.<schema>.<table>`, the declared fields as columns in order, the
//! virtual columns _qod_id and _qod_score. Its scan is the scroll (lower/scroll_scan.hpp).
class QdrantTableEntry : public duckdb::TableCatalogEntry {
public:
	QdrantTableEntry(duckdb::Catalog &catalog, duckdb::SchemaCatalogEntry &schema, duckdb::CreateTableInfo &info,
	                 CollectionSpec spec, std::shared_ptr<const QdrantSource> source);

	const CollectionSpec &Spec() const {
		return spec_;
	}
	const QdrantSource &Source() const {
		return *source_;
	}

	const duckdb::ColumnList &GetColumns() const override {
		return columns_;
	}
	duckdb::unique_ptr<duckdb::BaseStatistics> GetStatistics(duckdb::ClientContext &context,
	                                                          duckdb::column_t column_id) override;
	duckdb::TableFunction GetScanFunction(duckdb::ClientContext &context,
	                                      duckdb::unique_ptr<duckdb::FunctionData> &bind_data) override;
	duckdb::TableStorageInfo GetStorageInfo(duckdb::ClientContext &context) override;
	duckdb::virtual_column_map_t GetVirtualColumns() const override;
	//! None: the table has no rowid, so UPDATE and DELETE reach the read-only refusal instead of the
	//! binder's INTERNAL error for a row id column missing from GetVirtualColumns.
	duckdb::vector<duckdb::column_t> GetRowIdColumns() const override {
		return {};
	}

private:
	duckdb::ColumnList columns_;
	CollectionSpec spec_;
	std::shared_ptr<const QdrantSource> source_;
};

class QdrantSchemaEntry : public duckdb::SchemaCatalogEntry {
public:
	QdrantSchemaEntry(duckdb::Catalog &catalog, duckdb::CreateSchemaInfo &info);

	void AddTable(duckdb::unique_ptr<QdrantTableEntry> table);

	void Scan(duckdb::ClientContext &context, duckdb::CatalogType type,
	          const std::function<void(duckdb::CatalogEntry &)> &callback) override;
	void Scan(duckdb::CatalogType type, const std::function<void(duckdb::CatalogEntry &)> &callback) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateIndex(duckdb::CatalogTransaction transaction,
	                                                       duckdb::CreateIndexInfo &info,
	                                                       duckdb::TableCatalogEntry &table) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateFunction(duckdb::CatalogTransaction transaction,
	                                                          duckdb::CreateFunctionInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateTable(duckdb::CatalogTransaction transaction,
	                                                       duckdb::BoundCreateTableInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateView(duckdb::CatalogTransaction transaction,
	                                                      duckdb::CreateViewInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateSequence(duckdb::CatalogTransaction transaction,
	                                                          duckdb::CreateSequenceInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateType(duckdb::CatalogTransaction transaction,
	                                                      duckdb::CreateTypeInfo &info) override;
	duckdb::optional_ptr<duckdb::CatalogEntry> LookupEntry(duckdb::CatalogTransaction transaction,
	                                                       const duckdb::EntryLookupInfo &lookup_info) override;
	void DropEntry(duckdb::ClientContext &context, duckdb::DropInfo &info) override;
	void Alter(duckdb::CatalogTransaction transaction, duckdb::AlterInfo &info) override;

private:
	std::vector<duckdb::unique_ptr<QdrantTableEntry>> tables_;
};

//! The read-only catalog one ATTACH ... (TYPE qdrant) creates. Fixed at attach: a config change reaches
//! the node when the pool recycles.
class QdrantCatalog : public duckdb::Catalog {
public:
	QdrantCatalog(duckdb::AttachedDatabase &db, std::shared_ptr<const QdrantSource> source);

	const QdrantSource &Source() const {
		return *source_;
	}

	void Initialize(bool load_builtin) override;
	std::string GetCatalogType() override {
		return "qdrant";
	}
	duckdb::optional_ptr<duckdb::CatalogEntry> CreateSchema(duckdb::CatalogTransaction transaction,
	                                                        duckdb::CreateSchemaInfo &info) override;
	duckdb::optional_ptr<duckdb::SchemaCatalogEntry> LookupSchema(duckdb::CatalogTransaction transaction,
	                                                              const duckdb::EntryLookupInfo &schema_lookup,
	                                                              duckdb::OnEntryNotFound if_not_found) override;
	void ScanSchemas(duckdb::ClientContext &context, std::function<void(duckdb::SchemaCatalogEntry &)> callback) override;
	duckdb::PhysicalOperator &PlanCreateTableAs(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner,
	                                            duckdb::LogicalCreateTable &op, duckdb::PhysicalOperator &plan) override;
	duckdb::PhysicalOperator &PlanInsert(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner,
	                                     duckdb::LogicalInsert &op,
	                                     duckdb::optional_ptr<duckdb::PhysicalOperator> plan) override;
	duckdb::PhysicalOperator &PlanDelete(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner,
	                                     duckdb::LogicalDelete &op, duckdb::PhysicalOperator &plan) override;
	duckdb::PhysicalOperator &PlanUpdate(duckdb::ClientContext &context, duckdb::PhysicalPlanGenerator &planner,
	                                     duckdb::LogicalUpdate &op, duckdb::PhysicalOperator &plan) override;
	//! Refused before the default would bind a rowid the table does not have (an INTERNAL error).
	duckdb::unique_ptr<duckdb::LogicalOperator> BindCreateIndex(duckdb::Binder &binder, duckdb::CreateStatement &stmt,
	                                                            duckdb::TableCatalogEntry &table,
	                                                            duckdb::unique_ptr<duckdb::LogicalOperator> plan) override;
	duckdb::DatabaseSize GetDatabaseSize(duckdb::ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	std::string GetDBPath() override;

private:
	void DropSchema(duckdb::ClientContext &context, duckdb::DropInfo &info) override;

	std::shared_ptr<const QdrantSource> source_;
	std::vector<duckdb::unique_ptr<QdrantSchemaEntry>> schemas_;
};

class QdrantTransactionManager : public duckdb::TransactionManager {
public:
	explicit QdrantTransactionManager(duckdb::AttachedDatabase &db) : duckdb::TransactionManager(db) {
	}
	duckdb::Transaction &StartTransaction(duckdb::ClientContext &context) override;
	duckdb::ErrorData CommitTransaction(duckdb::ClientContext &context, duckdb::Transaction &transaction) override;
	void RollbackTransaction(duckdb::Transaction &transaction) override;
	void Checkpoint(duckdb::ClientContext &context, bool force = false) override {
	}

private:
	void Remove(duckdb::Transaction &transaction);
	std::mutex mutex_;
	duckdb::reference_map_t<duckdb::Transaction, duckdb::unique_ptr<duckdb::Transaction>> transactions_;
};

} // namespace qod_qdrant
