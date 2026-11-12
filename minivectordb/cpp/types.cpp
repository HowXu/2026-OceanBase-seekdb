#include "types.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string_view>

namespace minivectordb {
namespace {
[[noreturn]] void invalid(const char *message) {
  throw DbError("type_error", message);
}
void name_valid(const std::string &name) {
  if (name.empty() || name.find('\0') != std::string::npos)
    invalid("invalid name");
  utf8_codepoints(name);
}
bool digit(char c) { return c >= '0' && c <= '9'; }
} // namespace
std::string normalized_name(std::string_view name) {
  std::string result(name);
  for (char &c : result)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
std::size_t utf8_codepoints(const std::string &text) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++count) {
    auto lead = static_cast<unsigned char>(text[i++]);
    if (lead < 0x80)
      continue;
    unsigned n;
    std::uint32_t cp, minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
      n = 1;
      cp = lead & 31;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      n = 2;
      cp = lead & 15;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      n = 3;
      cp = lead & 7;
      minimum = 0x10000;
    } else
      invalid("invalid UTF-8");
    if (n > text.size() - i)
      invalid("truncated UTF-8");
    while (n--) {
      auto b = static_cast<unsigned char>(text[i++]);
      if ((b & 0xc0) != 0x80)
        invalid("invalid UTF-8 continuation");
      cp = (cp << 6) | (b & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      invalid("invalid UTF-8 codepoint");
  }
  return count;
}
void validate_schema(TableSchema &schema) {
  name_valid(schema.name);
  std::set<std::string> names;
  std::size_t pk = 0, vectors = 0, pk_index = 0;
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    const auto &c = schema.columns[i];
    name_valid(c.name);
    if (!names.insert(normalized_name(c.name)).second)
      invalid("duplicate column");
    switch (c.type) {
    case ColumnType::BigInt:
      if (c.length)
        invalid("BIGINT has no length");
      break;
    case ColumnType::Varchar:
    case ColumnType::Vector:
      if (!c.length || *c.length == 0)
        invalid("positive length required");
      break;
    default:
      invalid("unknown column type");
    }
    if (c.type == ColumnType::Vector)
      ++vectors;
    if (c.primary_key) {
      if (c.type != ColumnType::BigInt)
        invalid("primary key must be BIGINT");
      ++pk;
      pk_index = i;
    }
  }
  if (pk != 1 || vectors > 1)
    invalid("one BIGINT primary key and at most one vector required");
  schema.primary_key_index = pk_index;
}
std::vector<float> parse_vector(const std::string &text,
                                std::uint64_t dimension) {
  std::size_t p = 0;
  auto ws = [&] {
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' ||
                               text[p] == '\r' || text[p] == '\n'))
      ++p;
  };
  auto take = [&](char c) {
    ws();
    if (p == text.size() || text[p++] != c)
      invalid("invalid vector syntax");
  };
  take('[');
  ws();
  std::vector<float> result;
  if (p < text.size() && text[p] != ']') {
    while (true) {
      ws();
      auto start = p;
      if (p < text.size() && (text[p] == '+' || text[p] == '-'))
        ++p;
      std::size_t digits = 0;
      while (p < text.size() && digit(text[p])) {
        ++p;
        ++digits;
      }
      if (p < text.size() && text[p] == '.') {
        ++p;
        while (p < text.size() && digit(text[p])) {
          ++p;
          ++digits;
        }
      }
      if (digits == 0)
        invalid("invalid vector number");
      if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
        ++p;
        if (p < text.size() && (text[p] == '+' || text[p] == '-'))
          ++p;
        auto begin = p;
        while (p < text.size() && digit(text[p]))
          ++p;
        if (begin == p)
          invalid("invalid vector exponent");
      }
      std::string_view number(text.data() + start, p - start);
      if (number.front() == '+') number.remove_prefix(1);
      double value = 0;
      auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(),
                                       value, std::chars_format::general);
      if (ec == std::errc::result_out_of_range) {
        // from_chars does not distinguish overflow from underflow. strtod
        // provides a finite zero for the latter; both are checked explicitly.
        std::string token(number);
        char* parse_end = nullptr;
        errno = 0;
        value = std::strtod(token.c_str(), &parse_end);
        if (parse_end != token.c_str() + token.size() || !std::isfinite(value))
          invalid("nonfinite or out-of-range float32");
      } else if (ec != std::errc{} || end != number.data() + number.size() ||
                 !std::isfinite(value)) {
        invalid("nonfinite or out-of-range float32");
      }
      float stored = static_cast<float>(value);
      if (!std::isfinite(stored))
        invalid("nonfinite or out-of-range float32");
      result.push_back(stored);
      if (result.size() > dimension)
        invalid("vector dimension mismatch");
      ws();
      if (p == text.size())
        invalid("unterminated vector");
      if (text[p] != ',')
        break;
      ++p;
    }
  }
  take(']');
  ws();
  if (p != text.size() || result.size() != dimension)
    invalid("vector dimension mismatch or trailing text");
  return result;
}
Cell bind_literal(const Literal &literal, const ColumnSchema &column) {
  if (column.type == ColumnType::BigInt) {
    if (auto value = std::get_if<std::int64_t>(&literal))
      return *value;
    invalid("BIGINT requires integer");
  }
  const auto *value = std::get_if<std::string>(&literal);
  if (!value || !column.length || !*column.length)
    invalid("string literal and valid length required");
  if (column.type == ColumnType::Varchar) {
    if (utf8_codepoints(*value) > *column.length)
      invalid("VARCHAR too long");
    return *value;
  }
  if (column.type == ColumnType::Vector)
    return parse_vector(*value, *column.length);
  invalid("unknown column type");
}
void validate_row(const TableSchema &schema, const Row &row) {
  if (row.size() != schema.columns.size())
    invalid("row width mismatch");
  for (std::size_t i = 0; i < row.size(); ++i) {
    const auto &c = schema.columns[i];
    const auto &value = row[i];
    if (c.type == ColumnType::BigInt &&
        std::holds_alternative<std::int64_t>(value))
      continue;
    if (c.type == ColumnType::Varchar &&
        std::holds_alternative<std::string>(value) && c.length &&
        utf8_codepoints(std::get<std::string>(value)) <= *c.length)
      continue;
    if (c.type == ColumnType::Vector &&
        std::holds_alternative<std::vector<float>>(value) && c.length) {
      const auto &v = std::get<std::vector<float>>(value);
      if (v.size() == *c.length) {
        bool finite = true;
        for (float x : v)
          finite = finite && std::isfinite(x);
        if (finite)
          continue;
      }
    }
    invalid("row cell violates column type or length");
  }
}
} // namespace minivectordb
