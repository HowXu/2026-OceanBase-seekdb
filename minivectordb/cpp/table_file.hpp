#pragma once
#include "storage_codec.hpp"

namespace minivectordb {
// A writer produces one candidate file, never a visible root. finish is
// mandatory; destruction closes but deliberately leaves an unfinished file
// invalid.
class TableFileWriter {
public:
  TableFileWriter(const std::filesystem::path &path, TableSchema schema);
  void append(const Row &row);
  void finish();

private:
  TableSchema schema_;
  storage::Output out_;
  std::optional<std::int64_t> last_key_;
  bool finished_ = false;
};
class TableFileReader {
public:
  TableFileReader(const std::filesystem::path &path,
                  TableSchema expected_schema);
  // Retains only one decoded row; false requires a valid end marker and EOF.
  bool next(Row &row);
  const TableSchema &schema() const { return schema_; }

private:
  TableSchema schema_;
  storage::Input in_;
  std::optional<std::int64_t> last_key_;
  bool ended_ = false;
};
} // namespace minivectordb
