#include <charconv>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <pybind11/pybind11.h>
#include "engine.hpp"

namespace py = pybind11;
using namespace minivectordb;
namespace {
[[noreturn]] void invalid(const std::string& message) { throw DbError("invalid_ir", message); }
py::dict object(py::handle value, std::initializer_list<const char*> required,
                std::initializer_list<const char*> optional = {}) {
  if (!py::isinstance<py::dict>(value)) invalid("expected IR object");
  auto dict = py::reinterpret_borrow<py::dict>(value);
  for (auto key : required) if (!dict.contains(key)) invalid(std::string("missing field: ") + key);
  for (auto item : dict) {
    if (!py::isinstance<py::str>(item.first)) invalid("IR keys must be strings");
    auto key = py::cast<std::string>(item.first);
    bool known = false;
    for (auto allowed : required) known |= key == allowed;
    for (auto allowed : optional) known |= key == allowed;
    if (!known) invalid("unexpected field: " + key);
  }
  return dict;
}
std::string text(py::handle value) {
  if (!py::isinstance<py::str>(value)) invalid("expected string");
  return py::cast<std::string>(value);
}
std::string name(py::handle value) {
  auto result = text(value);
  if (result.empty() || result.find('\0') != std::string::npos) invalid("invalid empty or NUL identifier");
  return result;
}
py::list array(py::handle value) {
  if (!py::isinstance<py::list>(value)) invalid("expected list");
  return py::reinterpret_borrow<py::list>(value);
}
bool boolean(py::handle value) {
  if (!py::isinstance<py::bool_>(value)) invalid("expected bool");
  return py::cast<bool>(value);
}
std::uint64_t unsigned_integer(py::handle value) {
  if (!PyLong_CheckExact(value.ptr())) invalid("expected nonnegative integer");
  auto result = PyLong_AsUnsignedLongLong(value.ptr());
  if (PyErr_Occurred()) { PyErr_Clear(); invalid("integer outside uint64 range"); }
  return result;
}
Literal literal(py::handle value) {
  auto d = object(value, {"kind", "text"});
  auto kind = text(d["kind"]);
  auto raw = text(d["text"]);
  if (kind == "string") return raw;
  if (kind != "integer") invalid("unsupported literal kind");
  auto digits = std::string_view(raw);
  if (!digits.empty() && digits.front() == '-') digits.remove_prefix(1);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos)
    invalid("invalid integer text");
  std::int64_t parsed = 0;
  auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
  if (error != std::errc{} || end != raw.data() + raw.size()) invalid("integer outside int64 range");
  return parsed;
}
Expression expression(py::handle value) {
  if (!py::isinstance<py::dict>(value)) invalid("expected expression object");
  auto d = py::reinterpret_borrow<py::dict>(value);
  if (!d.contains("kind")) invalid("missing expression kind");
  auto kind = text(d["kind"]);
  if (kind == "column") { object(d, {"kind", "name"}); return ColumnRef{name(d["name"])}; }
  if (kind == "star") { object(d, {"kind"}); return Star{}; }
  if (kind == "ordinal") {
    object(d, {"kind", "position"});
    return OrderPosition{unsigned_integer(d["position"])};
  }
  if (kind == "integer" || kind == "string") return literal(d);
  if (kind == "l2_distance") {
    object(d, {"kind", "column", "vector"});
    auto vector = literal(d["vector"]);
    if (!std::holds_alternative<std::string>(vector)) invalid("distance vector must be a string literal");
    return L2Distance{name(d["column"]), std::get<std::string>(vector)};
  }
  invalid("unsupported expression kind");
}
std::vector<Comparison> conditions(const py::dict& d) {
  std::vector<Comparison> result;
  if (!d.contains("where")) return result;
  for (auto value : array(d["where"])) {
    auto item = object(value, {"left", "operator", "right"});
    auto op = text(item["operator"]);
    CompareOperator comparison;
    if (op == "=") comparison = CompareOperator::Equal;
    else if (op == "<") comparison = CompareOperator::Less;
    else if (op == "<=") comparison = CompareOperator::LessEqual;
    else if (op == ">") comparison = CompareOperator::Greater;
    else if (op == ">=") comparison = CompareOperator::GreaterEqual;
    else invalid("unsupported comparison operator");
    result.push_back({expression(item["left"]), comparison, expression(item["right"])});
  }
  return result;
}
Statement bind_statement(py::handle value) {
  if (!py::isinstance<py::dict>(value)) invalid("expected statement object");
  auto d = py::reinterpret_borrow<py::dict>(value);
  if (!d.contains("op")) invalid("missing statement op");
  auto op = text(d["op"]);
  if (op == "begin" || op == "commit" || op == "rollback") {
    object(d, {"op"});
    if (op == "begin") return Begin{};
    if (op == "commit") return Commit{};
    return Rollback{};
  }
  if (op == "create_table") {
    object(d, {"op", "table", "columns"});
    CreateTable result{name(d["table"]), {}};
    for (auto value : array(d["columns"])) {
      auto column = object(value, {"name", "type"}, {"length", "primary_key"});
      auto type = text(column["type"]);
      ColumnType column_type;
      if (type == "bigint") column_type = ColumnType::BigInt;
      else if (type == "varchar") column_type = ColumnType::Varchar;
      else if (type == "vector") column_type = ColumnType::Vector;
      else invalid("unsupported column type");
      std::optional<std::uint64_t> length;
      if (column.contains("length")) length = unsigned_integer(column["length"]);
      if ((column_type == ColumnType::BigInt) == length.has_value()) invalid("invalid column length field");
      result.columns.push_back({name(column["name"]), column_type, length,
          column.contains("primary_key") ? boolean(column["primary_key"]) : false});
    }
    return result;
  }
  if (op == "insert") {
    object(d, {"op", "table", "rows"}, {"columns"});
    Insert result{name(d["table"]), std::nullopt, {}};
    if (d.contains("columns")) {
      result.columns.emplace();
      for (auto column : array(d["columns"])) result.columns->push_back(name(column));
    }
    for (auto values : array(d["rows"])) {
      std::vector<Literal> row;
      for (auto cell : array(values)) row.push_back(literal(cell));
      result.rows.push_back(std::move(row));
    }
    return result;
  }
  if (op == "select") {
    object(d, {"op", "table", "projections"}, {"where", "order_by", "limit"});
    Select result{name(d["table"]), {}, conditions(d), {}, std::nullopt};
    for (auto value : array(d["projections"])) {
      auto item = object(value, {"expression"}, {"alias", "display_text"});
      std::optional<std::string> alias;
      std::optional<std::string> display;
      if (item.contains("alias")) alias = name(item["alias"]);
      if (item.contains("display_text")) {
        if (alias) invalid("aliased projection cannot have display_text");
        display = text(item["display_text"]);
        if (display->empty() || display->find('\0') != std::string::npos)
          invalid("invalid projection display text");
        utf8_codepoints(*display);
      }
      result.projections.push_back({expression(item["expression"]), alias, display});
    }
    if (d.contains("order_by")) for (auto value : array(d["order_by"])) {
      auto item = object(value, {"expression"}, {"descending"});
      result.order_by.push_back({expression(item["expression"]),
          item.contains("descending") ? boolean(item["descending"]) : false});
    }
    if (d.contains("limit")) result.limit = unsigned_integer(d["limit"]);
    return result;
  }
  if (op == "update") {
    object(d, {"op", "table", "assignments"}, {"where"});
    Update result{name(d["table"]), {}, conditions(d)};
    for (auto value : array(d["assignments"])) {
      auto item = object(value, {"column", "value"});
      result.assignments.push_back({name(item["column"]), literal(item["value"])});
    }
    return result;
  }
  if (op == "delete") {
    object(d, {"op", "table"}, {"where"});
    return Delete{name(d["table"]), conditions(d)};
  }
  if (op == "create_vector_index") {
    object(d, {"op", "index", "table", "column"});
    return CreateVectorIndex{name(d["index"]), name(d["table"]), name(d["column"])};
  }
  invalid("unsupported statement op");
}
py::object result_cell(const ResultCell& value) {
  return std::visit([](const auto& cell) -> py::object {
    using T = std::decay_t<decltype(cell)>;
    if constexpr (std::is_same_v<T, std::int64_t>) return py::int_(cell);
    else if constexpr (std::is_same_v<T, std::string>) return py::str(cell);
    else if constexpr (std::is_same_v<T, double>) {
      if (!std::isfinite(cell)) throw DbError("numeric_error", "nonfinite result");
      return py::float_(cell);
    } else {
      py::list result;
      for (float component : cell) {
        if (!std::isfinite(component)) throw DbError("numeric_error", "nonfinite vector result");
        result.append(py::float_(component));
      }
      return result;
    }
  }, value);
}
py::dict result_payload(const QueryResult& result) {
  py::dict payload;
  if (auto affected = std::get_if<AffectedRows>(&result)) payload["affected_rows"] = py::int_(affected->count);
  if (auto selected = std::get_if<ResultSet>(&result)) {
    py::list columns, rows;
    for (const auto& column : selected->columns) columns.append(py::str(column));
    for (const auto& row : selected->rows) {
      py::list cells;
      for (const auto& cell : row) cells.append(result_cell(cell));
      rows.append(cells);
    }
    payload["columns"] = columns;
    payload["rows"] = rows;
  }
  return payload;
}
}  // namespace
PYBIND11_MODULE(_minivectordb, module) {
  py::register_local_exception_translator([](std::exception_ptr ptr) {
    try { if (ptr) std::rethrow_exception(ptr); }
    catch (const DbError& error) {
      auto type = py::module_::import("minivectordb.errors").attr("DatabaseError");
      auto instance = type(error.code, error.what());
      PyErr_SetObject(type.ptr(), instance.ptr());
    }
  });
  py::class_<Engine>(module, "Engine")
      .def(py::init([](const std::string& path) { return std::make_unique<Engine>(path); }))
      .def("execute", [](Engine& engine, py::handle ir, const std::string& mode) {
        if (mode != "exact" && mode != "ann") throw DbError("invalid_request", "invalid search_mode");
        auto statement = bind_statement(ir);
        return result_payload(engine.execute(statement, mode == "exact" ? SearchMode::Exact : SearchMode::Ann));
      }, py::arg("statement"), py::arg("search_mode") = "exact")
      .def("try_execute_insert", [](Engine& engine, const std::string& sql,
                                    const std::string& mode) -> py::object {
        if (mode != "exact" && mode != "ann") throw DbError("invalid_request", "invalid search_mode");
        auto result = engine.try_execute_insert(sql, mode == "exact" ? SearchMode::Exact : SearchMode::Ann);
        if (!result) return py::none();
        return result_payload(*result);
      }, py::arg("sql"), py::arg("search_mode") = "exact")
      .def("close", &Engine::close);
}
