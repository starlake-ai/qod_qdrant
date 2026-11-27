// The QoD host: core's own headers provide qod::ProtectedScan and the rest; core seals every scan.
#include "host_flags.hpp"

#include "duckdb/common/exception.hpp"

namespace qod_qdrant {

bool HostEnforcesPolicies() {
	return true;
}

bool HostIsStandalone() {
	return false;
}

void HostSealUnprotected(qod::ProtectedScan &) {
	throw duckdb::PermissionException("QoD: a qod_qdrant scan cannot seal itself; core seals every scan");
}

} // namespace qod_qdrant
