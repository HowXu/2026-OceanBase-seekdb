#include "sql_fast_path.hpp"
#include <charconv>

namespace minivectordb {
namespace {
[[noreturn]] void invalid() {
  throw DbError("unsupported_sql", "invalid fast-path INSERT");
}
bool identifier_character(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}
class Cursor {
 public:
  explicit Cursor(std::string_view text) : text_(text) {}
  void whitespace() {
    while (position_ < text_.size() &&
           (text_[position_] == ' ' || text_[position_] == '\t' ||
            text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
  }
  bool keyword(std::string_view expected) {
    whitespace();
    auto start = position_;
    if (expected.size() > text_.size() - start) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      char c = text_[start + i];
      if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
      if (c != expected[i]) return false;
    }
    auto end = start + expected.size();
    if (end < text_.size() && identifier_character(text_[end])) return false;
    position_ = end;
    return true;
  }
  std::optional<std::string> identifier() {
    whitespace();
    auto start = position_;
    if (start == text_.size() || identifier_character(text_[start]) &&
                                      text_[start] >= '0' && text_[start] <= '9')
      return std::nullopt;
    while (position_ < text_.size() && identifier_character(text_[position_])) ++position_;
    if (position_ == start) return std::nullopt;
    return std::string(text_.substr(start, position_ - start));
  }
  void expect(char expected) {
    whitespace();
    if (position_ == text_.size() || text_[position_] != expected) invalid();
    ++position_;
  }
  bool take(char expected) {
    whitespace();
    if (position_ == text_.size() || text_[position_] != expected) return false;
    ++position_;
    return true;
  }
  std::optional<Literal> literal() {
    whitespace();
    if (position_ == text_.size()) return std::nullopt;
    if (text_[position_] == '\'') return string_literal();
    auto sign_position = position_;
    bool negative = take('-');
    whitespace();
    if (negative && position_ != sign_position + 1) return std::nullopt;
    if (position_ < text_.size() && text_[position_] == '+') return std::nullopt;
    if (position_ == text_.size() || text_[position_] < '0' || text_[position_] > '9')
      return std::nullopt;
    auto start = position_;
    while (position_ < text_.size() && text_[position_] >= '0' &&
           text_[position_] <= '9') ++position_;
    if (position_ == start) invalid();
    std::int64_t value = 0;
    auto begin = text_.data() + (negative ? sign_position : start);
    auto [end, error] = std::from_chars(begin, text_.data() + position_, value);
    if (error != std::errc{} || end != text_.data() + position_)
      throw DbError("invalid_ir", "integer outside int64 range");
    return value;
  }
  bool at_end() {
    whitespace();
    return position_ == text_.size();
  }
  bool comment() {
    whitespace();
    return text_.substr(position_, 2) == "/*";
  }
  bool finished() {
    whitespace();
    if (position_ < text_.size() && text_[position_] == ';') ++position_;
    whitespace();
    return position_ == text_.size();
  }
 private:
  std::optional<Literal> string_literal() {
    ++position_;
    std::string value;
    while (position_ < text_.size()) {
      char c = text_[position_++];
      if (c == '\\') return std::nullopt;  // MySQL backslash rules use fallback.
      if (c != '\'') {
        value.push_back(c);
        continue;
      }
      if (position_ < text_.size() && text_[position_] == '\'') {
        ++position_;
        value.push_back('\'');
        continue;
      }
      return value;
    }
    invalid();
  }
  std::string_view text_;
  std::size_t position_ = 0;
};
}  // namespace

std::optional<Insert> try_parse_insert_values(std::string_view sql) {
  Cursor cursor(sql);
  if (!cursor.keyword("INSERT") || !cursor.keyword("INTO")) return std::nullopt;
  auto table = cursor.identifier();
  if (!table || !cursor.keyword("VALUES")) return std::nullopt;
  Insert result{std::move(*table), std::nullopt, {}};
  while (true) {
    cursor.expect('(');
    std::vector<Literal> row;
    if (!cursor.take(')')) {
      while (true) {
        auto value = cursor.literal();
        if (!value) return std::nullopt;
        row.push_back(std::move(*value));
        if (cursor.take(',')) continue;
        if (cursor.take(')')) break;
        if (cursor.at_end()) invalid();
        return std::nullopt;
      }
    }
    result.rows.push_back(std::move(row));
    if (cursor.take(',')) continue;
    break;
  }
  if (!cursor.finished()) {
    if (cursor.comment()) return std::nullopt;
    invalid();
  }
  return result;
}
}  // namespace minivectordb
