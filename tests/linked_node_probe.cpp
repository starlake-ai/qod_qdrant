// Through core's node hooks and the real HTTPS transport (no fake): attach a source whose endpoint is the
// local stub, scan it and search it under a tenant_id row policy (the stub also holds another tenant's
// point, which never comes back), lower an exact policy, and refuse an inexact one with no request.
// usage: linked_node_probe <port> <ca-file> <sample_canonical.json>; env QOD_QDRANT_TEST_REQUEST_LOG.
#include "check.hpp"
#include "engine.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace qod_qdrant_test;

namespace {
size_t Lines(const char *path) {
	std::ifstream in(path ? path : "");
	size_t n = 0;
	std::string line;
	while (std::getline(in, line)) {
		++n;
	}
	return n;
}

//! Installs the handle, or ends the probe: a handle refused here would surface below only as "no verified
//! principal" on every statement.
void Install(Engine &e) {
	auto installed = e.Install();
	if (installed != "true") {
		std::cerr << "install: " << installed << std::endl;
		std::exit(1);
	}
}

std::string Dense() {
	std::string v = "[";
	for (int i = 0; i < 768; ++i) {
		v += i ? ",0.5" : "0.5";
	}
	return "qod_dense('text_dense', " + v + "]::FLOAT[])";
}
} // namespace

int main(int argc, char **argv) {
	if (argc != 4) {
		std::cerr << "usage: linked_node_probe <port> <ca-file> <sample_canonical.json>" << std::endl;
		return 2;
	}
	std::string endpoint = std::string("https://localhost:") + argv[1];
	setenv("QOD_QDRANT_CA_FILE", argv[2], 1);
	auto log = std::getenv("QOD_QDRANT_TEST_REQUEST_LOG");
	Engine e(endpoint);
	// As qod-node points secret_directory at its own folder before any secret: the stored secrets of
	// whoever runs the test (~/.duckdb/stored_secrets) are never read, before or after external access ends.
	auto persistent = RunSql(*e.setup, "SET allow_persistent_secrets = false");
	if (!persistent.empty()) {
		std::cerr << "persistent secrets: " << persistent << std::endl;
		return 1;
	}
	auto config = Patch(ReadFile(argv[3]), "https://qdrant.internal:6333", endpoint);
	auto attached = e.Attach("search_docs", config);
	std::cout << "attach " << (attached.empty() ? "ok" : attached) << std::endl;
	// Plan 4 runs nodes whose init ends with this: the transport (CA file included) never uses DuckDB's
	// file system.
	auto external = RunSql(*e.setup, "SET enable_external_access = false");
	if (!external.empty()) {
		std::cerr << "external access: " << external << std::endl;
		return 1;
	}
	// Tenants are separated by a row policy on the ordinary field tenant_id, lowered into every request.
	e.handle.row_policies = {{"t1", "search_docs", "main", "contracts", "tenant_id = 'acme'"}};
	Install(e);
	std::cout << "scan " << RunSql(*e.user, "SELECT document_id FROM search_docs.main.contracts ORDER BY 1") << std::endl;
	auto search = "SELECT document_id, _qod_id, owner_id, _qod_score FROM qod_search('search_docs.main.contracts', query => " +
	              Dense() + ", filter_sql => 'status = ''active''', k => 10) ORDER BY _qod_score DESC";
	std::cout << "search " << RunSql(*e.user, search) << std::endl;
	e.handle.row_policies = {{"p1", "search_docs", "main", "contracts", "tenant_id = 'acme' AND owner_id IN (7)"}};
	Install(e);
	std::cout << "policy "
	          << RunSql(*e.user, "SELECT document_id FROM qod_search('search_docs.main.contracts', query => " + Dense() + ")")
	          << std::endl;
	auto before = Lines(log);
	e.handle.row_policies = {{"p2", "search_docs", "main", "contracts", "owner_id > 5"}};
	Install(e);
	auto refused = RunSql(*e.user, "SELECT document_id FROM qod_search('search_docs.main.contracts', query => " + Dense() + ")");
	std::cout << "inexact " << (refused.find("QoD: search_policy_unsupported: ") != std::string::npos ? "refused" : refused)
	          << std::endl;
	std::cout << "requests " << before << " then " << Lines(log) << std::endl;
	return 0;
}
