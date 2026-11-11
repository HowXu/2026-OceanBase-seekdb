#pragma once
#include <vector>

namespace minivectordb {
// Inputs have already been bound to finite float32 vectors of the same dimension.
// Sorting uses the square to avoid introducing sqrt rounding ties.
double l2_distance_squared(const std::vector<float>& stored, const std::vector<float>& query);
}  // namespace minivectordb
