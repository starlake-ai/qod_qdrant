#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qod {

using Digest = std::array<uint8_t, 32>;

Digest Sha256(const std::string &data);

std::string Hex(const uint8_t *bytes, size_t length);
std::string Hex(const Digest &digest);

} // namespace qod
