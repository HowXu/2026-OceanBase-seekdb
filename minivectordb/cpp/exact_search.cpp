#include "exact_search.hpp"
#include "types.hpp"
#include <cmath>

namespace minivectordb {
double l2_distance_squared(const std::vector<float>& stored, const std::vector<float>& query) {
  if (stored.size() != query.size()) throw DbError("type_error", "vector dimension mismatch");
  double sum = 0;
  for (std::size_t i = 0; i < stored.size(); ++i) {
    const double difference = static_cast<double>(stored[i]) - static_cast<double>(query[i]);
    sum += difference * difference;
  }
  if (!std::isfinite(sum)) throw DbError("numeric_error", "nonfinite vector distance");
  return sum;
}
}  // namespace minivectordb
