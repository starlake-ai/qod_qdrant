#pragma once

#include "function_classes.hpp"

#include <optional>
#include <string>

namespace qod {

//! No-op standalone: nothing reads linked classes.
void RegisterLinkedFunctionClass(const std::string &lower_name, const std::string &type, FunctionClass cls);

std::optional<FunctionClass> LinkedFunctionClassOf(const std::string &lower_name, const std::string &type);

} // namespace qod
