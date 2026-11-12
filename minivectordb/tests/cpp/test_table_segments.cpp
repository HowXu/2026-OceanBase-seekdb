#include "table_segments.hpp"
#include "test_storage_support.hpp"
#include <algorithm>

int main() {
  TestDir d;
  auto s = schema();
  auto row = [](std::string text, std::int64_t id) {
    return Row{std::move(text), id, std::vector<float>{0.f, 1.f}};
  };
  auto base = d.path / "base.tbl";
  {
    TableFileWriter writer(base, s);
    writer.append(row("a", 1));
    writer.append(row("c", 3));
    writer.finish();
  }
  auto first = d.path / "first.tbl";
  {
    DeltaFileWriter writer(first, s);
    writer.append({1, false, row("a1", 1)});
    writer.append({2, false, row("b", 2)});
    writer.append({3, true, {}});
    writer.finish();
  }
  auto second = d.path / "second.tbl";
  {
    DeltaFileWriter writer(second, s);
    writer.append({2, false, row("b2", 2)});
    writer.append({3, false, row("c3", 3)});
    writer.finish();
  }
  std::vector<std::filesystem::path> paths{base, first};
  LogicalTableReader reader(paths, s);
  Row current;
  std::vector<std::int64_t> keys;
  while (reader.next(current)) keys.push_back(std::get<std::int64_t>(current[s.primary_key_index]));
  check(keys == std::vector<std::int64_t>({1, 2}));
  paths.push_back(second);
  LogicalTableReader merged(paths, s);
  keys.clear();
  std::vector<std::string> values;
  while (merged.next(current)) {
    keys.push_back(std::get<std::int64_t>(current[s.primary_key_index]));
    values.push_back(std::get<std::string>(current[0]));
  }
  check(keys == std::vector<std::int64_t>({1, 2, 3}));
  check(values == std::vector<std::string>({"a1", "b2", "c3"}));
  validate_table_objects(paths, s);
  rejects([&] {
    DeltaFileWriter writer(d.path / "bad-order.tbl", s);
    writer.append({2, false, row("b", 2)});
    writer.append({1, false, row("a", 1)});
  });
  auto truncated = d.path / "truncated.tbl";
  std::filesystem::copy_file(first, truncated);
  std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 1);
  rejects([&] { validate_table_objects({base, truncated}, s); });
  auto wrong = s;
  wrong.name = "wrong";
  rejects([&] { validate_table_objects(paths, wrong); });
  rejects([&] { validate_table_objects({}, s); });
}
