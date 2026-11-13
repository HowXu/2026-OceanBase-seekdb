#include "exact_search.hpp"
#include "types.hpp"
#include <cmath>

namespace minivectordb {
double l2_distance_squared(const std::vector<float>& stored, const std::vector<float>& query) {
  if (stored.size() != query.size()) throw DbError("type_error", "vector dimension mismatch");
  double sum = 0;
  std::size_t i = 0;
  for (; i + 4 <= stored.size(); i += 4) {
    const double a = static_cast<double>(stored[i]) - static_cast<double>(query[i]);
    sum += a * a;
    const double b = static_cast<double>(stored[i+1]) - static_cast<double>(query[i+1]);
    sum += b * b;
    const double c = static_cast<double>(stored[i+2]) - static_cast<double>(query[i+2]);
    sum += c * c;
    const double d = static_cast<double>(stored[i+3]) - static_cast<double>(query[i+3]);
    sum += d * d;
  }
  for (; i < stored.size(); ++i) {
    const double difference = static_cast<double>(stored[i]) - static_cast<double>(query[i]);
    sum += difference * difference;
  }
  if (!std::isfinite(sum)) throw DbError("numeric_error", "nonfinite vector distance");
  return sum;
}
}  // namespace minivectordb
