#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace minivectordb {
enum class SearchMode { Exact, Ann };
enum class ColumnType { BigInt, Varchar, Vector };
struct ColumnSchema {
  std::string name;
  ColumnType type;
  std::optional<std::uint64_t> length;
  bool primary_key = false;
};
struct TableSchema {
  std::string name;
  std::vector<ColumnSchema> columns;
  std::size_t primary_key_index = 0;  // Set only after schema validation.
};
using Cell = std::variant<std::int64_t, std::string, std::vector<float>>;
using Row = std::vector<Cell>;
// Integer SQL text is parsed exactly at the binding boundary; strings stay strings.
using Literal = std::variant<std::int64_t, std::string>;
struct ColumnRef { std::string name; };
struct Star {};
struct L2Distance { std::string column; std::string vector; };
struct OrderPosition { std::uint64_t position; bool operator==(const OrderPosition&) const = default; };
using Expression = std::variant<ColumnRef, Star, Literal, L2Distance, OrderPosition>;
struct Projection {
  Expression expression;
  std::optional<std::string> alias;
  std::optional<std::string> display_text;
};
enum class CompareOperator { Equal, Less, LessEqual, Greater, GreaterEqual };
struct Comparison { Expression left; CompareOperator op; Expression right; };
struct OrderItem { Expression expression; bool descending = false; };
struct Assignment { std::string column; Literal value; };
struct CreateTable { std::string table; std::vector<ColumnSchema> columns; };
struct Insert {
  std::string table;
  std::optional<std::vector<std::string>> columns;
  std::vector<std::vector<Literal>> rows;
};
struct Select {
  std::string table;
  std::vector<Projection> projections;
  std::vector<Comparison> where;
  std::vector<OrderItem> order_by;
  std::optional<std::uint64_t> limit;
};
struct Update { std::string table; std::vector<Assignment> assignments; std::vector<Comparison> where; };
struct Delete { std::string table; std::vector<Comparison> where; };
struct CreateVectorIndex { std::string index; std::string table; std::string column; };
struct Begin {};
struct Commit {};
struct Rollback {};
using Statement = std::variant<CreateTable, Insert, Select, Update, Delete, CreateVectorIndex, Begin, Commit, Rollback>;
struct EmptyResult {};
struct AffectedRows { std::uint64_t count; };
// Distances are result-only doubles, never storage Cells.
using ResultCell = std::variant<std::int64_t, std::string, std::vector<float>, double>;
using ResultRow = std::vector<ResultCell>;
struct ResultSet { std::vector<std::string> columns; std::vector<ResultRow> rows; };
using QueryResult = std::variant<EmptyResult, AffectedRows, ResultSet>;
class DbError : public std::runtime_error {
 public:
  DbError(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code(std::move(code)) {}
  const std::string code;
};
// Semantic validation is native and independent of the parser/binding boundary.
std::string normalized_name(std::string_view name);
inline bool same_name(std::string_view a, std::string_view b) {
  return normalized_name(a) == normalized_name(b);
}
std::size_t utf8_codepoints(const std::string& text);
void validate_schema(TableSchema& schema);
std::vector<float> parse_vector(const std::string& text, std::uint64_t dimension);
Cell bind_literal(const Literal& literal, const ColumnSchema& column);
void validate_row(const TableSchema& schema, const Row& row);
}  // namespace minivectordb
