#pragma once

namespace qod {

//! The client state key QoD registers on a tenant connection; never registered standalone, so every
//! connection is the owner's.
constexpr const char *kQuackSessionKey = "quack_session";

} // namespace qod
