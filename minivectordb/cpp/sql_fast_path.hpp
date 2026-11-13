#pragma once
#include "types.hpp"
#include <optional>
#include <string_view>

namespace minivectordb {
// Returns nullopt for statements outside the narrow fast-path shape.
std::optional<Insert> try_parse_insert_values(std::string_view sql);
}  // namespace minivectordb
