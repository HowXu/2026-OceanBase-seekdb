#include "table_file.hpp"

namespace minivectordb {
namespace {
const std::string magic = "MVDBT001";
std::uint64_t row_bytes(const Row &row) {
  std::uint64_t total = 0;
  auto add = [&](std::uint64_t n) {
    if (n > UINT64_MAX - total)
      storage::fail("row too large");
    total += n;
  };
  for (const auto &cell : row) {
    if (std::holds_alternative<std::int64_t>(cell))
      add(8);
    else if (auto s = std::get_if<std::string>(&cell)) {
      add(8);
      add(s->size());
    } else {
      auto n = std::get<std::vector<float>>(cell).size();
      if (n > UINT64_MAX / 4)
        storage::fail("vector too large");
      add(n * 4);
    }
  }
  return total;
}
} // namespace
TableFileWriter::TableFileWriter(const std::filesystem::path &path,
                                 TableSchema schema)
    : schema_(std::move(schema)), out_(path) {
  validate_schema(schema_);
  out_.bytes(magic.data(), magic.size());
  storage::write_schema(out_, schema_);
}
void TableFileWriter::append(const Row &row) {
  if (finished_)
    storage::fail("writer already finished");
  validate_row(schema_, row);
  auto key = std::get<std::int64_t>(row[schema_.primary_key_index]);
  if (last_key_ && key <= *last_key_)
    throw DbError("duplicate_key",
                  "rows must have strictly increasing primary keys");
  out_.u8(1);
  out_.u64(row_bytes(row));
  for (const auto &cell : row) {
    if (auto n = std::get_if<std::int64_t>(&cell))
      out_.u64(std::bit_cast<std::uint64_t>(*n));
    else if (auto s = std::get_if<std::string>(&cell))
      out_.text(*s);
    else
      for (float x : std::get<std::vector<float>>(cell))
        out_.u32(std::bit_cast<std::uint32_t>(x));
  }
  last_key_ = key;
}
void TableFileWriter::finish() {
  if (finished_)
    return;
  out_.u8(0);
  out_.finish();
  finished_ = true;
}
TableFileReader::TableFileReader(const std::filesystem::path &path,
                                 TableSchema expected_schema)
    : schema_(std::move(expected_schema)), in_(path) {
  validate_schema(schema_);
  in_.magic(magic);
  auto actual = storage::read_schema(in_);
  if (!storage::same_schema(actual, schema_))
    storage::fail("table schema mismatch");
}
bool TableFileReader::next(Row &row) {
  if (ended_)
    return false;
  auto marker = in_.u8();
  if (marker == 0) {
    in_.end();
    ended_ = true;
    row.clear();
    return false;
  }
  if (marker != 1)
    storage::fail("invalid row marker");
  auto length = in_.u64();
  if (length >= in_.remaining())
    storage::fail("invalid row length or missing footer");
  auto after = in_.remaining() - length;
  Row decoded;
  for (const auto &c : schema_.columns) {
    if (c.type == ColumnType::BigInt)
      decoded.emplace_back(std::bit_cast<std::int64_t>(in_.u64()));
    else if (c.type == ColumnType::Varchar)
      decoded.emplace_back(in_.text());
    else {
      if (*c.length > in_.remaining() / 4)
        storage::fail("truncated vector");
      std::vector<float> v;
      v.reserve(static_cast<std::size_t>(*c.length));
      for (std::uint64_t i = 0; i < *c.length; ++i)
        v.push_back(std::bit_cast<float>(in_.u32()));
      decoded.emplace_back(std::move(v));
    }
  }
  if (in_.remaining() != after)
    storage::fail("row length mismatch");
  validate_row(schema_, decoded);
  auto key = std::get<std::int64_t>(decoded[schema_.primary_key_index]);
  if (last_key_ && key <= *last_key_)
    storage::fail("unordered table rows");
  last_key_ = key;
  row = std::move(decoded);
  return true;
}
} // namespace minivectordb
