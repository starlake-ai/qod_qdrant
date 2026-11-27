// The qdrant catalog and its plain scan through the real node hooks: ATTACH validation, columns, virtual
// columns, no filter without a row policy, P lowered when exact and enforced locally otherwise (a tenant_id
// row policy included), local recheck of every point, pagination, the scan budget, and no I/O on an unsealed
// scan.
#include "arrow/payload.hpp"
#include "catalog/catalog.hpp"
#include "check.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table_function.hpp"
#include "engine.hpp"
#include "fake_transport.hpp"
#include "lower/filter_expr.hpp"
#include "lower/scan_data.hpp"
#include "lower/scroll_scan.hpp"

using namespace qod_qdrant_test;

namespace {
const std::string kPoint1 =
    "{\"id\":1,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d1\",\"status\":\"active\",\"owner_id\":7}}";
const std::string kPoint2 =
    "{\"id\":2,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d2\",\"status\":\"draft\",\"owner_id\":3}}";

std::string Page(const std::string &points, const std::string &next = "null") {
	return "{\"result\":{\"points\":[" + points + "],\"next_page_offset\":" + next + "},\"status\":\"ok\",\"time\":0.001}";
}

std::string Body(size_t i) {
	std::lock_guard<std::mutex> lock(Backend().mutex);
	return i < Backend().calls.size() ? Backend().calls[i].body : "no call";
}
} // namespace

int main(int argc, char **argv) {
	std::string fixtures = std::string(argc > 1 ? argv[1] : "tests") + "/fixtures/";
	auto config = ReadFile(fixtures + "sample_canonical.json");
	InstallFakeTransport();
	Engine e;
	Expect("attach", e.Attach("search_docs", config), "");
	Expect("columns in declared order",
	       RunSql(*e.setup, "SELECT name || ':' || type FROM pragma_table_info('search_docs.main.contracts')"),
	       "tenant_id:VARCHAR,document_id:VARCHAR,title:VARCHAR,status:VARCHAR,owner_id:BIGINT,body:VARCHAR");
	Contains("not read-only", e.Attach("a1", config, ""), "search_config_invalid: ");
	Contains("read-write", e.Attach("a2", config, ", READ_ONLY false"), "search_config_invalid: ");
	Contains("extra option", e.Attach("a3", config, ", READ_ONLY, HIDDEN"), "search_config_invalid: ");
	Contains("bad source id", e.Attach("a4", config, ", READ_ONLY", "qdrant:bad id!"), "search_config_invalid: ");
	auto foreign = Patch(config, "https://qdrant.internal:6333", "https://other.internal:6333");
	Contains("endpoint outside the allowlist", e.Attach("a5", foreign),
	         "endpoint: is not in the deployment allowlist (QOD_QDRANT_ALLOWED_ENDPOINTS)");
	Contains("config errors", e.Attach("a6", Patch(config, "\"generation\":1", "\"generation\":0")),
	         "collections[0].generation: must be an integer in 1..9223372036854775807");
	Contains("missing secret", RunSql(*e.setup, "ATTACH 'qdrant:fs-x' AS \"a7\" (TYPE qdrant, SECRET nope, CONFIG '" + config +
	                                                "', READ_ONLY)"),
	         "search_config_invalid: ");
	Contains("writes refused", RunSql(*e.setup, "INSERT INTO search_docs.main.contracts (tenant_id) VALUES ('x')"), "ERROR");

	// ATTACH takes exactly TYPE, SECRET, CONFIG and READ_ONLY on a qdrant:<source id> path.
	auto attach = [&](const std::string &alias, const std::string &path, const std::string &options) {
		return RunSql(*e.setup, "ATTACH '" + path + "' AS \"" + alias + "\" (" + options + ")");
	};
	auto with_config = "CONFIG '" + config + "'";
	Contains("TYPE required", attach("b1", "qdrant:fs-b", "SECRET qod_qdrant_search_docs, " + with_config + ", READ_ONLY"),
	         "search_config_invalid: ");
	Contains("SECRET required", attach("b2", "qdrant:fs-b", "TYPE qdrant, " + with_config + ", READ_ONLY"),
	         "search_config_invalid: ");
	Contains("CONFIG required", attach("b3", "qdrant:fs-b", "TYPE qdrant, SECRET qod_qdrant_search_docs, READ_ONLY"),
	         "search_config_invalid: ");
	Contains("path needs the qdrant: prefix",
	         attach("b4", "fs-b", "TYPE qdrant, SECRET qod_qdrant_search_docs, " + with_config + ", READ_ONLY"),
	         "search_config_invalid: ");
	Contains("another access mode spelling",
	         attach("b5", "qdrant:fs-b", "TYPE qdrant, SECRET qod_qdrant_search_docs, " + with_config + ", READONLY"),
	         "search_config_invalid: ");
	Contains("a non-string CONFIG", attach("b6", "qdrant:fs-b", "TYPE qdrant, SECRET qod_qdrant_search_docs, CONFIG 1, READ_ONLY"),
	         "search_config_invalid: ");
	Contains("UPDATE refused without an INTERNAL error", RunSql(*e.setup, "UPDATE search_docs.main.contracts SET status = 'x'"),
	         "search_query_unsupported: ", "INTERNAL");
	Contains("DELETE refused without an INTERNAL error", RunSql(*e.setup, "DELETE FROM search_docs.main.contracts"),
	         "search_query_unsupported: ", "INTERNAL");
	Contains("CREATE INDEX refused without an INTERNAL error",
	         RunSql(*e.setup, "CREATE INDEX i ON search_docs.main.contracts (status)"), "search_query_unsupported: ",
	         "INTERNAL");
	Expect("read-only refusal names no backend", RunSql(*e.setup, "DELETE FROM search_docs.main.contracts"),
	       "ERROR: Invalid Input Error: search_query_unsupported: this catalog is read-only");
	auto plan = RunSql(*e.setup, "EXPLAIN SELECT document_id FROM search_docs.main.contracts");
	Contains("scan operator name", plan, "QOD_SEARCH_SCAN", "QDRANT");
	Contains("scan operator title", plan, "Qod Search Scan", "Qdrant");
	Expect("the path shown never holds the endpoint",
	       RunSql(*e.setup, "SELECT path, readonly FROM duckdb_databases() WHERE database_name = 'search_docs'"),
	       "qdrant:fs-qdrant-1|true");
	{
		// The allowlist is the snapshot taken at load: a database loaded with an empty one refuses every
		// endpoint, and the first database keeps its own although the environment changed since.
		Engine empty("");
		Contains("empty allowlist at load", empty.Attach("search_docs", config),
		         "endpoint: the deployment allowlist (QOD_QDRANT_ALLOWED_ENDPOINTS) is empty or unset");
		Expect("allowlist snapshot survives an env change", e.Attach("a8", config), "");
		Expect("detach", RunSql(*e.setup, "DETACH a8"), "");
	}
	Expect("no request at ATTACH or for any refusal", std::to_string(Backend().CallCount()), "0");

	{
		// The scan's global init checks the column ids once, before the seal and any I/O.
		e.setup->BeginTransaction();
		auto &context = *e.setup->context;
		auto entry = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(
		    context, duckdb::QualifiedName(duckdb::Identifier("search_docs"), duckdb::Identifier("main"),
		                                   duckdb::Identifier("contracts")),
		    duckdb::OnEntryNotFound::RETURN_NULL);
		auto table = entry ? dynamic_cast<qod_qdrant::QdrantTableEntry *>(entry.get()) : nullptr;
		ExpectTrue("the table entry is a qdrant table", table != nullptr);
		if (table) {
			auto bind = qod_qdrant::MakeScrollBindData(*table, context);
			auto function = qod_qdrant::ScrollFunction();
			duckdb::vector<duckdb::idx_t> no_projection;
			duckdb::TableFunctionInitInput bad(bind.get(), duckdb::vector<duckdb::column_t> {1, qod_qdrant::IdColumn(), 6},
			                                   no_projection, nullptr);
			Contains("unknown column id refused at init", ErrorOf([&] { function.init_global(context, bad); }),
			         "search_query_unsupported: ");
			duckdb::TableFunctionInitInput good(bind.get(),
			                                    duckdb::vector<duckdb::column_t> {1, qod_qdrant::ScoreColumn()},
			                                    no_projection, nullptr);
			Contains("known column ids reach the seal check", ErrorOf([&] { function.init_global(context, good); }),
			         "QoD: connector scan ran without a sealed policy obligation");

			// A bind data that is not ours fails closed with a coded error, never an INTERNAL one.
			duckdb::TableFunctionData foreign_data;
			duckdb::TableFunctionInitInput foreign_input(&foreign_data, duckdb::vector<duckdb::column_t> {1}, no_projection,
			                                             nullptr);
			Contains("foreign bind data refused at init", ErrorOf([&] { function.init_global(context, foreign_input); }),
			         "search_query_unsupported: ");

			// The execution recheck (search_policy_changed): what core sealed must be what was accepted, the
			// seal must still match the filter, and the live table entry must still have the sealed config
			// digest, generation and schema version.
			auto &spec = table->Spec();
			// A tenant row policy on the ordinary field tenant_id, as lowered.
			auto tenant_policy = qod_qdrant::FilterExpr::Match(
			    size_t(spec.FieldIndex("tenant_id")), qod_qdrant::FieldValue {qod_qdrant::FieldType::Varchar, "acme"});
			auto sealed = [&](const std::function<void(qod_qdrant::QdrantScanData &)> &tamper, const std::string &digest) {
				auto made = qod_qdrant::MakeScrollBindData(*table, context);
				auto &data = dynamic_cast<qod_qdrant::QdrantScanData &>(*made);
				data.accepted = true;
				data.remote_filter = tenant_policy;
				data.accepted_digest = qod::PolicyDigest({});
				data.sealed_config_digest = table->Source().config_digest;
				data.sealed_guarantee = table->Source().config.guarantee;
				data.sealed_generation = spec.generation;
				data.sealed_schema_version = spec.schema_version;
				tamper(data);
				data.seal = data.ComputeSeal();
				SealProtectedScan(data, digest.empty() ? data.accepted_digest : digest);
				return made;
			};
			auto init_with = [&](duckdb::unique_ptr<duckdb::FunctionData> made,
			                     const std::function<void(qod_qdrant::QdrantScanData &)> &after_seal) {
				after_seal(dynamic_cast<qod_qdrant::QdrantScanData &>(*made));
				duckdb::TableFunctionInitInput input(made.get(), duckdb::vector<duckdb::column_t> {1}, no_projection,
				                                     nullptr);
				return ErrorOf([&] { function.init_global(context, input); });
			};
			auto none = [](qod_qdrant::QdrantScanData &) {};
			Expect("a consistent seal initializes (no I/O at init)", init_with(sealed(none, ""), none), "no error");
			Contains("filter changed after the seal", init_with(sealed(none, ""), [&](qod_qdrant::QdrantScanData &d) {
				         d.remote_filter = qod_qdrant::FilterExpr::And(
				             {tenant_policy,
				              qod_qdrant::FilterExpr::Match(3, qod_qdrant::FieldValue {qod_qdrant::FieldType::Varchar, "x"})});
			         }),
			         "search_policy_changed: ");
			Contains("core sealed another digest than the accepted one",
			         init_with(sealed(none, std::string(64, '0')), none), "search_policy_changed: ");
			Contains("not accepted", init_with(sealed([](qod_qdrant::QdrantScanData &d) { d.accepted = false; }, ""), none),
			         "search_policy_changed: ");
			Contains("generation mismatch with the live entry",
			         init_with(sealed([](qod_qdrant::QdrantScanData &d) { ++d.sealed_generation; }, ""), none),
			         "search_policy_changed: ");
			Contains("schema version mismatch with the live entry",
			         init_with(sealed([](qod_qdrant::QdrantScanData &d) { ++d.sealed_schema_version; }, ""), none),
			         "search_policy_changed: ");
			Contains("config digest mismatch with the live entry",
			         init_with(sealed([](qod_qdrant::QdrantScanData &d) { d.sealed_config_digest = "other"; }, ""), none),
			         "search_policy_changed: ");
			Contains("guarantee mismatch with the live entry",
			         init_with(sealed([](qod_qdrant::QdrantScanData &d) { d.sealed_guarantee = "filtered_retrieval"; }, ""), none),
			         "search_policy_changed: ");
			Contains("the seal covers the guarantee", init_with(sealed(none, ""), [](qod_qdrant::QdrantScanData &d) {
				         d.sealed_guarantee = "filtered_retrieval";
			         }),
			         "search_policy_changed: ");
			Contains("the seal covers the corpus", init_with(sealed(none, ""), [&](qod_qdrant::QdrantScanData &d) {
				         d.corpus_filter = tenant_policy;
			         }),
			         "search_policy_changed: ");
			Contains("the seal covers the query", init_with(sealed(none, ""), [](qod_qdrant::QdrantScanData &d) {
				         d.query.mode = qod_qdrant::SearchQuery::Mode::Keyword;
			         }),
			         "search_policy_changed: ");
		}
		e.setup->Rollback();
		Expect("no request from init refusals", std::to_string(Backend().CallCount()), "0");
	}

	Backend().Reset();
	Contains("internal connection never sealed", RunSql(*e.setup, "SELECT document_id FROM search_docs.main.contracts"),
	         "QoD: connector scan ran without a sealed policy obligation");
	Expect("no request for an unsealed scan", std::to_string(Backend().CallCount()), "0");

	// The gateway lists the alias in the handle's read-only catalogs: core refuses a tenant's writes and
	// DETACH before the connector sees them.
	e.handle.read_only = {"search_docs"};
	Expect("install read-only handle", e.Install(), "true");
	Contains("tenant INSERT refused by core",
	         RunSql(*e.user, "INSERT INTO search_docs.main.contracts (tenant_id) VALUES ('x')"), "is read-only");
	Contains("tenant DETACH refused by core", RunSql(*e.user, "DETACH search_docs"), "is read-only");
	Expect("still attached", RunSql(*e.setup, "SELECT count(*) FROM duckdb_databases() WHERE database_name = 'search_docs'"),
	       "1");
	Expect("no request for read-only refusals", std::to_string(Backend().CallCount()), "0");
	Expect("install", e.Install(), "true");
	{
		// Bind, post_bind and pre_optimize do no remote I/O: a prepared or explained statement sends nothing.
		auto prepared = e.user->Prepare("SELECT document_id FROM search_docs.main.contracts");
		ExpectTrue("prepare", !prepared->HasError());
		RunSql(*e.user, "EXPLAIN SELECT document_id FROM search_docs.main.contracts");
		Expect("no request before execution", std::to_string(Backend().CallCount()), "0");
	}
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	Expect("scan rows and virtual columns",
	       RunSql(*e.user, "SELECT document_id, _qod_id, _qod_score FROM search_docs.main.contracts ORDER BY document_id"),
	       "d1|1|NULL,d2|2|NULL");
	Expect("no row policy: no filter, projected fields only", Body(0),
	       "{\"limit\":256,\"with_payload\":{\"include\":[\"document_id\"]},\"with_vector\":false}");
	Expect("api key from the secret", Backend().api_keys.at(0), "test-key");

	e.handle.row_policies = {{"p1", "search_docs", "main", "contracts", "status = 'active'"}};
	Expect("install policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1))};
	Expect("exact policy", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"), "d1");
	Contains("policy lowered alone", Body(0), "\"filter\":{\"must\":[{\"key\":\"status\",\"match\":{\"value\":\"active\"}}]},");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	auto leaked = RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts");
	Contains("a point outside the lowered filter fails the statement", leaked, "search_schema_mismatch: ", "draft");

	e.handle.row_policies = {{"p2", "search_docs", "main", "contracts", "owner_id > 5"}};
	Expect("install inexact policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	Expect("inexact policy enforced by core's local filter", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"),
	       "d1");
	Contains("nothing went remote", Body(0), "\"limit\":256,", "\"filter\"");

	// Column mapping (Task 4 invariant): a policy on a field of the same SQL type as another field, with a
	// projection in another order than the table's, lowers onto the right payload key.
	const std::string kTitled =
	    "{\"id\":5,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d5\",\"title\":\"T5\",\"status\":\"active\","
	    "\"owner_id\":9}}";
	e.handle.row_policies = {{"p3", "search_docs", "main", "contracts", "document_id = 'd5'"}};
	Expect("install same-type policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kTitled))};
	Expect("projection out of table order", RunSql(*e.user, "SELECT owner_id, status FROM search_docs.main.contracts"),
	       "9|active");
	Expect("lowered on document_id, projected and policy fields in declared order", Body(0),
	       "{\"filter\":{\"must\":[{\"key\":\"document_id\",\"match\":{\"value\":\"d5\"}}]},\"limit\":256,"
	       "\"with_payload\":{\"include\":[\"document_id\",\"status\",\"owner_id\"]},\"with_vector\":false}");
	e.handle.row_policies = {{"p4", "search_docs", "main", "contracts", "title = 'T5' AND status = 'active'"}};
	Expect("install two-field policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kTitled))};
	Expect("two same-type policy fields", RunSql(*e.user, "SELECT _qod_id, document_id FROM search_docs.main.contracts"),
	       "5|d5");
	Contains("title lowered on its own key", Body(0), "{\"key\":\"title\",\"match\":{\"value\":\"T5\"}}");
	Contains("status lowered on its own key", Body(0), "{\"key\":\"status\",\"match\":{\"value\":\"active\"}}");
	Contains("decoded fields are the requested ones", Body(0),
	         "\"include\":[\"document_id\",\"title\",\"status\"]");
	// All or nothing: one exact and one inexact predicate send no filter; core's local filter applies both.
	e.handle.row_policies = {{"p5", "search_docs", "main", "contracts", "status = 'active' AND owner_id > 5"}};
	Expect("install mixed policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	Expect("mixed policy enforced locally", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"), "d1");
	Contains("mixed policy sends no filter", Body(0), "\"limit\":256,", "\"filter\"");

	// Tenants are separated by a row policy on the ordinary field tenant_id. Exact: it is the remote filter.
	const std::string kBob =
	    "{\"id\":6,\"payload\":{\"tenant_id\":\"bob\",\"document_id\":\"d6\",\"status\":\"active\",\"owner_id\":9}}";
	e.handle.row_policies = {{"t1", "search_docs", "main", "contracts", "tenant_id = 'acme'"}};
	Expect("install tenant policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	Expect("tenant policy", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts ORDER BY 1"), "d1,d2");
	Expect("tenant policy is the remote filter", Body(0),
	       "{\"filter\":{\"must\":[{\"key\":\"tenant_id\",\"match\":{\"value\":\"acme\"}}]},\"limit\":256,"
	       "\"with_payload\":{\"include\":[\"tenant_id\",\"document_id\"]},\"with_vector\":false}");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kBob))};
	Contains("another tenant's point fails the statement", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"),
	         "search_schema_mismatch: ", "bob");
	// Inexact (mixed with a range): nothing goes remote, other tenants' points reach the node, never the user.
	e.handle.row_policies = {{"t2", "search_docs", "main", "contracts", "tenant_id = 'acme' AND owner_id > 5"}};
	Expect("install inexact tenant policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2 + "," + kBob))};
	Expect("inexact tenant policy enforced by core's local filter",
	       RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts ORDER BY 1"), "d1");
	Contains("inexact tenant policy sends no filter", Body(0), "\"limit\":256,", "\"filter\"");
	// A row policy that itself lowers to FALSE (an empty IN) grants nothing: the remote filter is FALSE, the
	// scan answers no rows and sends no request, never a scroll without a filter.
	e.handle.row_policies = {{"t3", "search_docs", "main", "contracts", "tenant_id IN ()"}};
	Expect("install a FALSE policy", e.Install(), "true");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kBob))};
	Expect("FALSE policy: no rows", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"), "");
	Expect("FALSE policy: no request", std::to_string(Backend().CallCount()), "0");

	// A statement core refuses at bind (a denied column) sends nothing.
	e.handle.row_policies = {};
	e.handle.column_policies = {{"c1", "search_docs", "main", "contracts", "title", "deny", ""}};
	Expect("install column policy", e.Install(), "true");
	Backend().Reset();
	Contains("denied column", RunSql(*e.user, "SELECT title FROM search_docs.main.contracts"),
	         "QoD: access to a column is denied by a column policy");
	Expect("no request for a refused statement", std::to_string(Backend().CallCount()), "0");
	e.handle.column_policies = {};

	e.handle.row_policies = {};
	Expect("install no policy", e.Install(), "true");
	Backend().Reset();
	Backend().handler = [](const FakeCall &call) {
		if (call.body.find("\"offset\"") == std::string::npos) {
			return Reply(200, Page(kPoint1, "2"));
		}
		return Reply(200, Page(kPoint2));
	};
	Expect("pages", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts ORDER BY 1"), "d1,d2");
	Expect("two pages fetched", std::to_string(Backend().CallCount()), "2");
	Contains("second page from the offset", Body(1), "\"offset\":2");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	Expect("count", RunSql(*e.user, "SELECT count(*) FROM search_docs.main.contracts"), "2");

	Expect("budget", RunSql(*e.setup, "SET GLOBAL search_scan_budget_points = 1"), "");
	Backend().Reset();
	Backend().replies = {Reply(200, Page(kPoint1 + "," + kPoint2))};
	Contains("budget exhausted errors, never truncates", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"),
	         "search_budget_exceeded: ");
	Contains("page limit is budget + 1", Body(0), "\"limit\":2");
	Expect("budget 2", RunSql(*e.setup, "SET GLOBAL search_scan_budget_points = 2"), "");
	Backend().Reset();
	Backend().handler = [](const FakeCall &call) {
		if (call.body.find("\"offset\"") == std::string::npos) {
			return Reply(200, Page(kPoint1 + "," + kPoint2, "3"));
		}
		return Reply(200, Page("{\"id\":3,\"payload\":{\"tenant_id\":\"acme\",\"document_id\":\"d3\"}}"));
	};
	Contains("budget spent exactly, then one more point", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"),
	         "search_budget_exceeded: ");
	Contains("the next page asks for one point only", Body(1), "\"limit\":1,");
	Expect("reset budget", RunSql(*e.setup, "RESET search_scan_budget_points"), "");

	Backend().Reset();
	Backend().replies = {Reply(200, Page("{\"id\":4,\"payload\":{\"tenant_id\":\"acme\"}}"))};
	Contains("missing required field", RunSql(*e.user, "SELECT status FROM search_docs.main.contracts"),
	         "search_schema_mismatch: ");
	Backend().Reset();
	Backend().replies = {Reply(500, "{\"status\":{\"error\":\"contracts_v3 exploded\"}}")};
	Contains("source failure", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"),
	         "search_source_unavailable: ", "contracts_v3");
	Backend().Reset();
	Backend().replies = {Reply(200, "{\"result\":{\"points\":[]},\"status\":\"ok\",\"surprise\":1}")};
	Contains("unknown response key", RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts"),
	         "search_schema_mismatch: ");
	Backend().Reset();
	{
		// Over the parse budget (kResponseJsonLimits.max_nodes): search_budget_exceeded, the parser's detail
		// only in the privileged log.
		std::string many = "{\"result\":{\"points\":[0";
		for (int i = 0; i < 1048576; ++i) {
			many += ",0";
		}
		many += "],\"next_page_offset\":null},\"status\":\"ok\"}";
		Backend().replies = {Reply(200, many)};
	}
	auto over = RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts");
	Contains("response over the parse budget", over, "search_budget_exceeded: ", "search_schema_mismatch");
	Contains("parse detail kept out of tenant text", over, "search_budget_exceeded: ", "offset");
	Backend().Reset();
	Backend().replies = {Reply(200, "{\"result\":{\"points\":[{\"id\":-1}],\"next_page_offset\":null},\"status\":\"ok\"}")};
	auto shape = RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts");
	Contains("bad point id", shape, "search_schema_mismatch: ", "points[0]");
	Contains("no backend text in tenant text", shape, "Invalid Input Error: search_schema_mismatch: the search source", "{");
	return Finish();
}
