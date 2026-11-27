#pragma once

// Standalone stand-in for QoD core's function_classes.hpp: without QoD there is no function allowlist, so
// every function is allowed (the extension still excludes getvariable and current_setting by name).
#include <optional>
#include <string>

namespace qod {

enum class FunctionClass { Allowed, Listing, File, Superuser };

std::optional<FunctionClass> FunctionClassOf(const std::string &name, const std::string &type);

} // namespace qod
