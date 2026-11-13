#include "test_storage_support.hpp"
#include "exact_search.hpp"
#include <cmath>
#include <limits>
#include <random>
int main() {
  auto s = schema();
  validate_schema(s);
  check(s.primary_key_index == 1);
  check(std::get<std::int64_t>(
            bind_literal(Literal{INT64_C(9007199254740993)}, s.columns[1])) ==
        INT64_C(9007199254740993));
  check(utf8_codepoints("中😀a") == 3);
  check(std::get<std::string>(bind_literal(Literal{std::string("中😀a")},
                                           s.columns[0])) == "中😀a");
  rejects([&] { bind_literal(Literal{std::string("abcd")}, s.columns[0]); });
  for (auto text : {std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
                    std::string("\xf4\x90\x80\x80"), std::string("\xe4\xb8")})
    rejects([&] { utf8_codepoints(text); });
  auto v = std::get<std::vector<float>>(
      bind_literal(Literal{std::string("[0.1, -2e1]")}, s.columns[2]));
  check(v[0] == 0.1f && v[1] == -20.f);
  for (const auto* text : {"[+1,01]", "[.5,1.]", "[1e-46,-1e-50]", "[1e-999,0]"}) {
    auto value = parse_vector(text, 2);
    check(value.size() == 2);
    for (float x : value) check(std::isfinite(x));
  }
  check(std::bit_cast<std::uint32_t>(parse_vector("[1e-46,0]",2)[0]) == 0);
  check(std::bit_cast<std::uint32_t>(parse_vector("[-1e-50,0]",2)[0]) == 0x80000000u);
  for (auto text : {"[NaN,0]", "[1e100,0]", "[1e39,0]", "[1e309,0]", "[Inf,0]",
                    "[+.,0]", "[1e,0]", "[1e+,0]", "[1,]", "[1 2,0]",
                    "[1,2]x", "[1]", "[1 2]"})
    rejects([&] { parse_vector(text, 2); });
  rejects([&] { bind_literal(Literal{std::string("1")}, s.columns[1]); });
  auto bad = s;
  bad.columns[0].name = "id";
  rejects([&] { validate_schema(bad); });
  bad = s;
  bad.columns[1].primary_key = false;
  rejects([&] { validate_schema(bad); });
  bad = s;
  bad.columns[0].primary_key = true;
  rejects([&] { validate_schema(bad); });
  bad = s;
  bad.columns[0].length = 0;
  rejects([&] { validate_schema(bad); });
  bad = s;
  bad.columns.push_back({"v2", ColumnType::Vector, 1, false});
  rejects([&] { validate_schema(bad); });
  rejects([&] {
    validate_row(
        s, Row{std::string("a"), INT64_C(1), std::vector<float>{INFINITY, 0}});
  });
  rejects([&] { validate_row(s, Row{}); });
  // Candidate distance unrolling keeps the exact sequential double additions.
  auto unrolled = [](const std::vector<float>& stored,
                     const std::vector<float>& query) {
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
    return sum;
  };
  std::mt19937_64 random(20261113);
  std::uniform_real_distribution<double> distribution(-1000.0, 1000.0);
  for (int trial = 0; trial < 1000; ++trial) {
    std::vector<float> stored(128), query(128);
    for (std::size_t i = 0; i < stored.size(); ++i) {
      stored[i] = static_cast<float>(distribution(random));
      query[i] = static_cast<float>(distribution(random));
    }
    auto expected = std::bit_cast<std::uint64_t>(l2_distance_squared(stored, query));
    auto actual = std::bit_cast<std::uint64_t>(unrolled(stored, query));
    check(actual == expected);
  }
}
