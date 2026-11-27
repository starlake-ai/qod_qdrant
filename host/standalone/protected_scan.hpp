#pragma once

// Standalone stand-in for QoD core's protected_scan.hpp: the same names and virtuals the extension overrides,
// written for a host with no row or column policies. Not a copy of core's header.
#include "duckdb.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <string>
#include <vector>

namespace qod {

//! A row policy as QoD core describes one; the standalone host never has any.
struct RowPolicy {
	std::string id;
	std::string catalog;
	std::string schema;
	std::string table;
	std::string sql;

	bool operator==(const RowPolicy &other) const {
		return id == other.id && catalog == other.catalog && schema == other.schema && table == other.table &&
		       sql == other.sql;
	}
};

struct LoweringResult {
	bool exact = false;
	std::string refusal_code;
};

struct FieldUse {
	enum class Kind { Filter, VectorLineage, Projection };
	std::string column;
	Kind kind;
};

class ProtectedScan {
public:
	virtual ~ProtectedScan() = default;
	virtual bool IsBounded() const = 0;
	virtual LoweringResult AcceptRowPolicies(duckdb::ClientContext &context, const duckdb::LogicalGet &get,
	                                         const std::vector<RowPolicy> &policies,
	                                         const std::vector<duckdb::unique_ptr<duckdb::Expression>> &predicates) = 0;
	virtual std::vector<FieldUse> FieldUses() const = 0;
	bool IsSealed() const {
		return sealed_;
	}
	const std::string &SealedDigest() const {
		return digest_;
	}
	//! Throws when nothing sealed the scan: standalone, the extension seals it itself (HostSealUnprotected).
	void RequireSealed() const;

protected:
	void CopySealFrom(const ProtectedScan &other) {
		sealed_ = other.sealed_;
		digest_ = other.digest_;
	}

private:
	friend void SealProtectedScan(ProtectedScan &scan, const std::string &digest);
	bool sealed_ = false;
	std::string digest_;
};

void SealProtectedScan(ProtectedScan &scan, const std::string &digest);

//! No-op standalone: no core walks the plan for bounded functions.
void RegisterBoundedFunction(const std::string &name);

std::string PolicyDigest(const std::vector<RowPolicy> &policies);

} // namespace qod
