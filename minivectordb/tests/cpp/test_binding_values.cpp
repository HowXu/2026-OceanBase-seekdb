// Compile the binding implementation in this test TU to exercise its private
// conversions without adding a test-only API to the production extension.
#include <pybind11/embed.h>
#include "../../cpp/bindings.cpp"
#include <limits>

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int main() {
  py::scoped_interpreter interpreter;
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  py::dict integer;
  integer["kind"] = "integer";
  for (auto value : {minimum, std::int64_t{9007199254740993LL}, maximum}) {
    integer["text"] = std::to_string(value);
    require(std::get<std::int64_t>(literal(integer)) == value, "BIGINT input lost precision");
    auto output = result_cell(ResultCell{value});
    require(PyLong_CheckExact(output.ptr()), "BIGINT output is not Python int");
    require(py::cast<std::int64_t>(output) == value, "BIGINT output lost precision");
  }
  auto vector = py::cast<py::list>(result_cell(ResultCell{std::vector<float>{16777217.0f, 0.5f}}));
  require(py::cast<double>(vector[0]) == 16777216.0, "vector output is not stored float32");
  require(py::cast<double>(vector[1]) == 0.5, "vector component changed");
  require(py::cast<double>(result_cell(ResultCell{5.0})) == 5.0, "distance output changed");
  auto empty = result_payload(ResultSet{{"id"}, {}});
  require(py::len(empty["columns"]) == 1 && py::len(empty["rows"]) == 0, "empty columns lost");
  require(py::len(result_payload(EmptyResult{})) == 0, "empty success payload changed");
  auto affected = result_payload(AffectedRows{9007199254740993ULL});
  require(py::cast<std::uint64_t>(affected["affected_rows"]) == 9007199254740993ULL,
          "affected_rows lost precision");
  for (const auto& invalid : {ResultCell{std::numeric_limits<double>::infinity()},
                              ResultCell{std::vector<float>{std::numeric_limits<float>::quiet_NaN()}}}) {
    bool rejected = false;
    try { result_cell(invalid); }
    catch (const DbError& error) { rejected = error.code == "numeric_error"; }
    require(rejected, "nonfinite output was accepted");
  }
}
