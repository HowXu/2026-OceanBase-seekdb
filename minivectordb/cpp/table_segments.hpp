#pragma once
#include "storage_codec.hpp"
#include "table_file.hpp"
#include <memory>
#include <optional>

namespace minivectordb {
struct DeltaRecord {
  std::int64_t key;
  bool deleted = false;
  Row row;
};
class DeltaFileWriter {
public:
  DeltaFileWriter(const std::filesystem::path& path, TableSchema schema);
  void append(const DeltaRecord& record);
  void finish();
private:
  TableSchema schema_;
  storage::Output out_;
  std::optional<std::int64_t> last_key_;
  bool finished_ = false;
};
class DeltaFileReader {
public:
  DeltaFileReader(const std::filesystem::path& path, TableSchema expected_schema,
                  RowValidation validation = RowValidation::Full);
  bool next(DeltaRecord& record);
private:
  TableSchema schema_;
  storage::Input in_;
  RowValidation validation_;
  std::optional<std::int64_t> last_key_;
  bool ended_ = false;
};
class LogicalTableReader {
public:
  LogicalTableReader(const std::vector<std::filesystem::path>& paths, TableSchema schema,
                     RowValidation validation = RowValidation::Full);
  ~LogicalTableReader();
  LogicalTableReader(LogicalTableReader&&) noexcept;
  LogicalTableReader& operator=(LogicalTableReader&&) noexcept;
  bool next(Row& row);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void validate_table_segment(const std::filesystem::path& path,
                            const TableSchema& schema, bool delta);
void validate_table_objects(const std::vector<std::filesystem::path>& paths,
                            const TableSchema& schema);
}  // namespace minivectordb
