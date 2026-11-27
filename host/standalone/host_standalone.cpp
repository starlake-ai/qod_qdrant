// The standalone host: the stand-ins of QoD core's hooks for plain DuckDB, and the loadable extension's entry.
// There are no row or column policies here, so only filter_sql filters; every scan is sealed by the extension
// itself, with the digest of no policy.
#include "crypto.hpp"
#include "extension.hpp"
#include "function_classes.hpp"
#include "host_flags.hpp"
#include "linked_function_classes.hpp"
#include "protected_scan.hpp"

#include "duckdb/common/exception.hpp"

#include <openssl/evp.h>

namespace qod {

void ProtectedScan::RequireSealed() const {
	if (!sealed_) {
		throw duckdb::PermissionException("qod_qdrant: this scan was not sealed");
	}
}

void SealProtectedScan(ProtectedScan &scan, const std::string &digest) {
	scan.sealed_ = true;
	scan.digest_ = digest;
}

void RegisterBoundedFunction(const std::string &) {
}

std::string PolicyDigest(const std::vector<RowPolicy> &policies) {
	std::string canonical;
	for (auto &p : policies) {
		canonical += p.id + '\x1f' + p.catalog + '\x1f' + p.schema + '\x1f' + p.table + '\x1f' + p.sql + '\x1e';
	}
	return Hex(Sha256(canonical));
}

std::optional<FunctionClass> FunctionClassOf(const std::string &, const std::string &) {
	return FunctionClass::Allowed;
}

void RegisterLinkedFunctionClass(const std::string &, const std::string &, FunctionClass) {
}

std::optional<FunctionClass> LinkedFunctionClassOf(const std::string &, const std::string &) {
	return FunctionClass::Allowed;
}

Digest Sha256(const std::string &data) {
	Digest out {};
	unsigned int length = 0;
	if (EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr) != 1 || length != out.size()) {
		throw duckdb::InternalException("qod_qdrant: SHA-256 failed");
	}
	return out;
}

std::string Hex(const uint8_t *bytes, size_t length) {
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.reserve(length * 2);
	for (size_t i = 0; i < length; i++) {
		out += digits[bytes[i] >> 4];
		out += digits[bytes[i] & 0x0f];
	}
	return out;
}

std::string Hex(const Digest &digest) {
	return Hex(digest.data(), digest.size());
}

} // namespace qod

namespace qod_qdrant {

bool HostEnforcesPolicies() {
	return false;
}

bool HostIsStandalone() {
	return true;
}

void HostSealUnprotected(qod::ProtectedScan &scan) {
	qod::SealProtectedScan(scan, qod::PolicyDigest({}));
}

} // namespace qod_qdrant

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(qod_qdrant, loader) {
	qod_qdrant::QdrantExtension().Load(loader);
}
}
