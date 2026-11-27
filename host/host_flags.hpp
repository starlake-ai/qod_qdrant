#pragma once

#include "protected_scan.hpp"

namespace qod_qdrant {

//! True in the QoD host: core hands every scan its row policies and seals it. False standalone, where a DuckDB
//! user owns their data and only filter_sql filters.
bool HostEnforcesPolicies();

//! True in the standalone host (lower/storage rules that differ: the endpoint allowlist is optional there).
bool HostIsStandalone();

//! Standalone only: seals a scan the host has no row policy for. The QoD host refuses the call (core alone
//! seals there), so no QoD scan can seal itself.
void HostSealUnprotected(qod::ProtectedScan &scan);

} // namespace qod_qdrant
