#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Record {
  std::int64_t id;
  std::int64_t group;
  std::string payload;
  std::vector<float> vector;
};
struct Query {
  std::int64_t id;
  std::int64_t group;
  std::vector<float> vector;
};
class Input {
 public:
  explicit Input(const char* path) : in_(path, std::ios::binary) {
    if (!in_) throw std::runtime_error("cannot open oracle input");
  }
  template<class T> T number() {
    static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
    T value;
    in_.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!in_) throw std::runtime_error("truncated oracle input");
    return value;
  }
  std::string bytes(std::uint64_t size) {
    if (size > (1 << 20)) throw std::runtime_error("invalid oracle payload size");
    std::string value(static_cast<std::size_t>(size), '\0');
    in_.read(value.data(), value.size());
    if (!in_) throw std::runtime_error("truncated oracle payload");
    return value;
  }
  void end() {
    if (in_.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("trailing oracle input");
  }
 private:
  std::ifstream in_;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: oracle <input>");
    Input in(argv[1]);
    auto magic = in.bytes(9);
    if (magic != "MVDBPERF1") throw std::runtime_error("invalid oracle magic");
    auto record_count = in.number<std::uint64_t>();
    auto query_count = in.number<std::uint64_t>();
    auto dimension = in.number<std::uint64_t>();
    auto k = in.number<std::uint64_t>();
    if (dimension == 0 || dimension > 65535 || record_count > 10000000 ||
        query_count > 10000000) throw std::runtime_error("invalid oracle dimensions");
    std::vector<Record> records;
    records.reserve(record_count);
    for (std::uint64_t i = 0; i < record_count; ++i) {
      Record record{in.number<std::int64_t>(), in.number<std::int64_t>(), "", {}};
      record.payload = in.bytes(in.number<std::uint64_t>());
      record.vector.resize(dimension);
      for (auto& value : record.vector) {
        value = std::bit_cast<float>(in.number<std::uint32_t>());
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite record vector");
      }
      records.push_back(std::move(record));
    }
    std::vector<Query> queries;
    queries.reserve(query_count);
    for (std::uint64_t i = 0; i < query_count; ++i) {
      Query query{in.number<std::int64_t>(), in.number<std::int64_t>(), {}};
      query.vector.resize(dimension);
      for (auto& value : query.vector) {
        value = std::bit_cast<float>(in.number<std::uint32_t>());
        if (!std::isfinite(value)) throw std::runtime_error("nonfinite query vector");
      }
      queries.push_back(std::move(query));
    }
    in.end();
    std::cout << std::setprecision(17) << '[';
    for (std::size_t qi = 0; qi < queries.size(); ++qi) {
      const auto& query = queries[qi];
      std::vector<std::pair<double, std::int64_t>> distances;
      for (const auto& record : records) {
        if (query.group >= 0 && record.group != query.group) continue;
        double sum = 0.0;
        for (std::size_t j = 0; j < dimension; ++j) {
          double difference = double(record.vector[j]) - double(query.vector[j]);
          sum += difference * difference;
        }
        double distance = std::sqrt(sum);
        if (!std::isfinite(distance)) throw std::runtime_error("nonfinite oracle distance");
        distances.emplace_back(distance, record.id);
      }
      std::sort(distances.begin(), distances.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
      });
      if (qi) std::cout << ',';
      std::cout << "{\"id\":" << query.id << ",\"rows\":[";
        auto count = std::min<std::uint64_t>(k, distances.size());
      for (std::uint64_t i = 0; i < count; ++i) {
        if (i) std::cout << ',';
        std::cout << '[' << distances[i].second << ',' << distances[i].first << ']';
      }
      std::cout << "]}";
    }
    std::cout << "]\n";
  } catch (const std::exception& error) {
    std::cerr << "oracle: " << error.what() << '\n';
    return 1;
  }
}
