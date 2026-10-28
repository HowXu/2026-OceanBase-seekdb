/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/file_sql_memory.h"

namespace oceanbase {
namespace sql {
namespace file_sql {

class LocalFile;

/**
 * @brief One column of a parquet file.
 */
struct ParquetColumn {
  BudgetString name;
  Type type = Type::Varchar;
  bool nullable = false;
};

/**
 * @brief Streaming reader of a parquet file.
 *
 * The reader decodes the footer of the file to learn its schema and then
 * decodes the requested columns row group by row group, so that the whole
 * file is never held in memory.
 *
 * The implementation is self contained: the thrift compact protocol used by
 * the parquet metadata, the page encodings and the snappy compression are
 * implemented inside the file.
 */
class ParquetScanner {
public:
  ParquetScanner();
  ~ParquetScanner();

  /**
   * @brief Open the file and read its footer.
   * @throw Error when the file is not a valid parquet file
   */
  void open(const std::shared_ptr<LocalFile> &file);

  /**
   * @brief The columns of the file, in file order.
   */
  const BudgetVector<ParquetColumn> &schema() const;

  /**
   * @brief The number of rows of the file.
   */
  uint64_t row_count() const;

  /**
   * @brief Select the columns to decode.
   *
   * The values are returned in the order of the projection. An empty
   * projection only counts the rows.
   */
  void set_projection(const BudgetVector<int> &projection);

  /**
   * @brief Read the next row.
   * @param texts   receives the value of every projected column
   * @param nulls   receives the null flag of every projected column
   * @return false when the end of the file is reached
   */
  bool next_row(BudgetVector<BudgetString> &texts, BudgetVector<bool> &nulls);

  /**
   * @brief Restart the reader at the first row.
   */
  void reset();

private:
  class ParquetScannerImpl;
  std::unique_ptr<ParquetScannerImpl> impl_;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
