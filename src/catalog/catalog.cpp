#include "catalog/catalog.hpp"

#include "arrow/payload.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "errors.hpp"
#include "lower/lowering.hpp"
#include "lower/scroll_scan.hpp"

#include <algorithm>

namespace qod_qdrant {
using namespace duckdb;

namespace {
constexpr const char *kReadOnly = "this catalog is read-only";

class QdrantTransaction : public Transaction {
public:
	QdrantTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
};
} // namespace

RemoteTarget QdrantSource::Target() const {
	return RemoteTarget {source_id, endpoint, api_key, allowlist};
}

QdrantTableEntry::QdrantTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                   CollectionSpec spec, std::shared_ptr<const QdrantSource> source)
    : TableCatalogEntry(catalog, schema, info), columns_(std::move(info.columns)), spec_(std::move(spec)),
      source_(std::move(source)) {
}

unique_ptr<BaseStatistics> QdrantTableEntry::GetStatistics(ClientContext &, column_t) {
	return nullptr;
}

TableFunction QdrantTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	bind_data = MakeScrollBindData(*this, context);
	return ScrollFunction();
}

TableStorageInfo QdrantTableEntry::GetStorageInfo(ClientContext &) {
	return TableStorageInfo();
}

virtual_column_map_t QdrantTableEntry::GetVirtualColumns() const {
	return VirtualColumns();
}

QdrantSchemaEntry::QdrantSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

void QdrantSchemaEntry::AddTable(unique_ptr<QdrantTableEntry> table) {
	tables_.push_back(std::move(table));
}

void QdrantSchemaEntry::Scan(ClientContext &, CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	Scan(type, callback);
}

void QdrantSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &table : tables_) {
		callback(*table);
	}
}

optional_ptr<CatalogEntry> QdrantSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
optional_ptr<CatalogEntry> QdrantSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
optional_ptr<CatalogEntry> QdrantSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
optional_ptr<CatalogEntry> QdrantSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
optional_ptr<CatalogEntry> QdrantSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
optional_ptr<CatalogEntry> QdrantSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

optional_ptr<CatalogEntry> QdrantSchemaEntry::LookupEntry(CatalogTransaction, const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto wanted = StringUtil::Lower(lookup_info.GetEntryName());
	for (auto &table : tables_) {
		if (table->Spec().table == wanted) {
			return table.get();
		}
	}
	return nullptr;
}

void QdrantSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

void QdrantSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

QdrantCatalog::QdrantCatalog(AttachedDatabase &db, std::shared_ptr<const QdrantSource> source)
    : Catalog(db), source_(std::move(source)) {
	std::vector<std::string> schema_names;
	for (auto &c : source_->config.collections) {
		if (std::find(schema_names.begin(), schema_names.end(), c.schema) == schema_names.end()) {
			schema_names.push_back(c.schema);
		}
	}
	for (auto &schema_name : schema_names) {
		CreateSchemaInfo schema_info;
		schema_info.SetQualifiedName(QualifiedName({Identifier(schema_name)}, Identifier()));
		auto schema = make_uniq<QdrantSchemaEntry>(*this, schema_info);
		for (auto &c : source_->config.collections) {
			if (c.schema != schema_name) {
				continue;
			}
			CreateTableInfo table_info(*schema, Identifier(c.table));
			for (auto &f : c.fields) {
				table_info.columns.AddColumn(ColumnDefinition(Identifier(f.name), SqlType(f.type)));
			}
			schema->AddTable(make_uniq<QdrantTableEntry>(*this, *schema, table_info, c, source_));
		}
		schemas_.push_back(std::move(schema));
	}
}

void QdrantCatalog::Initialize(bool) {
}

optional_ptr<CatalogEntry> QdrantCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

optional_ptr<SchemaCatalogEntry> QdrantCatalog::LookupSchema(CatalogTransaction, const EntryLookupInfo &schema_lookup,
                                                             OnEntryNotFound if_not_found) {
	// A qualified lookup leads with the catalog component ([catalog, schema]); a bare one is [schema].
	auto &path = schema_lookup.GetQualifiedName().Path();
	std::vector<std::string> parts;
	for (idx_t i = 0; i < path.size(); ++i) {
		if ((i == 0 && path.size() > 1) || path[i].empty()) {
			continue;
		}
		parts.push_back(StringUtil::Lower(path[i].GetIdentifierName()));
	}
	if (parts.size() == 1) {
		for (auto &schema : schemas_) {
			if (StringUtil::Lower(schema->name.GetIdentifierName()) == parts[0]) {
				return schema.get();
			}
		}
	}
	if (if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw CatalogException("Schema with name " + (parts.empty() ? std::string() : parts[0]) + " does not exist");
	}
	return nullptr;
}

void QdrantCatalog::ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) {
	for (auto &schema : schemas_) {
		callback(*schema);
	}
}

PhysicalOperator &QdrantCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                   PhysicalOperator &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
PhysicalOperator &QdrantCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                            optional_ptr<PhysicalOperator>) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
PhysicalOperator &QdrantCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &, PhysicalOperator &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}
PhysicalOperator &QdrantCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &, PhysicalOperator &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

unique_ptr<LogicalOperator> QdrantCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                           unique_ptr<LogicalOperator>) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

DatabaseSize QdrantCatalog::GetDatabaseSize(ClientContext &) {
	return DatabaseSize();
}

std::string QdrantCatalog::GetDBPath() {
	// Never the endpoint: duckdb_databases() shows this path. DuckDB also stores every ATTACH option it does
	// not recognize (CONFIG, which holds the endpoint, among them) in duckdb_databases().options; core classes
	// duckdb_databases superuser-only, so a tenant reads neither column.
	return "qdrant:" + source_->source_id;
}

void QdrantCatalog::DropSchema(ClientContext &, DropInfo &) {
	Raise(SearchCode::QueryUnsupported, kReadOnly);
}

Transaction &QdrantTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<QdrantTransaction>(*this, context);
	auto &result = *transaction;
	std::lock_guard<std::mutex> lock(mutex_);
	transactions_[result] = std::move(transaction);
	return result;
}

ErrorData QdrantTransactionManager::CommitTransaction(ClientContext &, Transaction &transaction) {
	Remove(transaction);
	return ErrorData();
}

void QdrantTransactionManager::RollbackTransaction(Transaction &transaction) {
	Remove(transaction);
}

void QdrantTransactionManager::Remove(Transaction &transaction) {
	unique_ptr<Transaction> removed;
	std::lock_guard<std::mutex> lock(mutex_);
	auto entry = transactions_.find(transaction);
	if (entry != transactions_.end()) {
		removed = std::move(entry->second);
		transactions_.erase(entry);
	}
}

} // namespace qod_qdrant
