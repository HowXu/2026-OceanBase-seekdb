#include "table_segments.hpp"
#include <bit>
#include <queue>
#include <utility>

namespace minivectordb {
namespace {
const std::string magic = "MVDBD001";
std::uint64_t row_bytes(const Row& row) {
  std::uint64_t total = 0;
  auto add = [&](std::uint64_t n) {
    if (n > UINT64_MAX - total) storage::fail("row too large");
    total += n;
  };
  for (const auto& cell : row) {
    if (std::holds_alternative<std::int64_t>(cell)) add(8);
    else if (auto text = std::get_if<std::string>(&cell)) {
      add(8);
      add(text->size());
    } else {
      auto n = std::get<std::vector<float>>(cell).size();
      if (n > UINT64_MAX / 4) storage::fail("vector too large");
      add(n * 4);
    }
  }
  return total;
}
void write_row(storage::Output& out, const Row& row) {
  for (const auto& cell : row) {
    if (auto number = std::get_if<std::int64_t>(&cell))
      out.u64(std::bit_cast<std::uint64_t>(*number));
    else if (auto text = std::get_if<std::string>(&cell))
      out.text(*text);
    else
      for (float value : std::get<std::vector<float>>(cell))
        out.u32(std::bit_cast<std::uint32_t>(value));
  }
}
Row read_row(storage::Input& in, const TableSchema& schema) {
  Row row;
  for (const auto& column : schema.columns) {
    if (column.type == ColumnType::BigInt)
      row.emplace_back(std::bit_cast<std::int64_t>(in.u64()));
    else if (column.type == ColumnType::Varchar)
      row.emplace_back(in.text());
    else {
      if (*column.length > in.remaining() / 4) storage::fail("truncated vector");
      std::vector<float> vector(static_cast<std::size_t>(*column.length));
      in.floats(vector.data(), *column.length);
      row.emplace_back(std::move(vector));
    }
  }
  return row;
}
std::int64_t row_key(const Row& row, const TableSchema& schema) {
  return std::get<std::int64_t>(row.at(schema.primary_key_index));
}
}  // namespace

DeltaFileWriter::DeltaFileWriter(const std::filesystem::path& path, TableSchema schema)
    : schema_(std::move(schema)), out_(path) {
  validate_schema(schema_);
  out_.bytes(magic.data(), magic.size());
  storage::write_schema(out_, schema_);
}
void DeltaFileWriter::append(const DeltaRecord& record) {
  if (finished_) storage::fail("writer already finished");
  if (record.deleted) {
    if (!record.row.empty()) storage::fail("tombstone cannot contain a row");
  } else {
    validate_row(schema_, record.row);
    if (row_key(record.row, schema_) != record.key) storage::fail("delta key differs from row");
  }
  if (last_key_ && record.key <= *last_key_)
    throw DbError("duplicate_key", "delta keys must be strictly increasing");
  out_.u8(record.deleted ? 2 : 1);
  out_.u64(std::bit_cast<std::uint64_t>(record.key));
  out_.u64(record.deleted ? 0 : row_bytes(record.row));
  if (!record.deleted) write_row(out_, record.row);
  last_key_ = record.key;
}
void DeltaFileWriter::finish() {
  if (finished_) return;
  out_.u8(0);
  out_.finish();
  finished_ = true;
}

DeltaFileReader::DeltaFileReader(const std::filesystem::path& path,
                                 TableSchema expected_schema)
    : schema_(std::move(expected_schema)), in_(path) {
  validate_schema(schema_);
  in_.magic(magic);
  auto actual = storage::read_schema(in_);
  if (!storage::same_schema(actual, schema_)) storage::fail("table schema mismatch");
}
bool DeltaFileReader::next(DeltaRecord& record) {
  if (ended_) return false;
  auto marker = in_.u8();
  if (marker == 0) {
    in_.end();
    ended_ = true;
    record = {};
    return false;
  }
  if (marker != 1 && marker != 2) storage::fail("invalid delta marker");
  auto key = std::bit_cast<std::int64_t>(in_.u64());
  auto length = in_.u64();
  if (length >= in_.remaining()) storage::fail("invalid delta length or missing footer");
  auto after = in_.remaining() - length;
  DeltaRecord decoded{key, marker == 2, {}};
  if (decoded.deleted) {
    if (length != 0) storage::fail("tombstone has a row payload");
  } else {
    decoded.row = read_row(in_, schema_);
    if (in_.remaining() != after) storage::fail("delta row length mismatch");
    validate_row(schema_, decoded.row);
    if (row_key(decoded.row, schema_) != key) storage::fail("delta key differs from row");
  }
  if (last_key_ && key <= *last_key_) storage::fail("unordered delta keys");
  last_key_ = key;
  record = std::move(decoded);
  return true;
}

struct LogicalTableReader::Impl {
  struct Cursor {
    std::unique_ptr<TableFileReader> base;
    std::unique_ptr<DeltaFileReader> delta;
    DeltaRecord current;
    std::size_t order = 0;
    bool active = false;
  };
  struct NewestAtSameKey {
    Impl* owner;
    bool operator()(std::size_t a, std::size_t b) const {
      const auto& left = owner->cursors[a].current;
      const auto& right = owner->cursors[b].current;
      if (left.key != right.key) return left.key > right.key;
      return owner->cursors[a].order < owner->cursors[b].order;
    }
  };
  TableSchema schema;
  std::vector<Cursor> cursors;
  std::priority_queue<std::size_t, std::vector<std::size_t>, NewestAtSameKey> heap;

  Impl(const std::vector<std::filesystem::path>& paths, TableSchema expected)
      : schema(std::move(expected)), heap(NewestAtSameKey{this}) {
    validate_schema(schema);
    if (paths.empty()) storage::fail("table has no objects");
    cursors.resize(paths.size());
    for (std::size_t i = 0; i < paths.size(); ++i) {
      auto& cursor = cursors[i];
      cursor.order = i;
      if (i == 0) cursor.base = std::make_unique<TableFileReader>(paths[i], schema);
      else cursor.delta = std::make_unique<DeltaFileReader>(paths[i], schema);
      advance(i);
    }
  }
  void advance(std::size_t index) {
    auto& cursor = cursors[index];
    cursor.active = false;
    if (cursor.base) {
      Row row;
      if (cursor.base->next(row)) {
        cursor.current = {row_key(row, schema), false, std::move(row)};
        cursor.active = true;
      }
    } else if (cursor.delta->next(cursor.current)) {
      cursor.active = true;
    }
    if (cursor.active) heap.push(index);
  }
};
LogicalTableReader::LogicalTableReader(const std::vector<std::filesystem::path>& paths,
                                       TableSchema schema)
    : impl_(std::make_unique<Impl>(paths, std::move(schema))) {}
LogicalTableReader::~LogicalTableReader() = default;
LogicalTableReader::LogicalTableReader(LogicalTableReader&&) noexcept = default;
LogicalTableReader& LogicalTableReader::operator=(LogicalTableReader&&) noexcept = default;
bool LogicalTableReader::next(Row& row) {
  while (!impl_->heap.empty()) {
    auto key = impl_->cursors[impl_->heap.top()].current.key;
    std::optional<DeltaRecord> selected;
    while (!impl_->heap.empty() && impl_->cursors[impl_->heap.top()].current.key == key) {
      auto index = impl_->heap.top();
      impl_->heap.pop();
      if (!selected) selected = std::move(impl_->cursors[index].current);
      impl_->advance(index);
    }
    if (!selected->deleted) {
      row = std::move(selected->row);
      return true;
    }
  }
  row.clear();
  return false;
}
void validate_table_segment(const std::filesystem::path& path,
                            const TableSchema& schema, bool delta) {
  if (delta) {
    DeltaFileReader reader(path, schema);
    DeltaRecord record;
    while (reader.next(record)) {}
  } else {
    TableFileReader reader(path, schema);
    Row row;
    while (reader.next(row)) {}
  }
}
void validate_table_objects(const std::vector<std::filesystem::path>& paths,
                            const TableSchema& schema) {
  LogicalTableReader reader(paths, schema);
  Row row;
  while (reader.next(row)) {}
}
}  // namespace minivectordb
