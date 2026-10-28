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

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief A binary blob that is able to travel inside an operator specification.
 *
 * The file table keeps the schema of the file in a serialized
 * file_sql::Definition, this blob carries it from the resolver to the operator.
 * The memory of the blob belongs to the allocator of the plan, it is not
 * copied when the specification is built.
 */
class ObFileTableBlob
{
public:
  ObFileTableBlob() : data_() {}
  const char *data() const { return data_.ptr(); }
  const common::ObString &ob_string() const { return data_; }
  int64_t length() const { return data_.length(); }
  bool empty() const { return data_.empty(); }
  std::string str() const
  {
    return data_.empty() ? std::string() : std::string(data_.ptr(), data_.length());
  }
  void set_data(const common::ObString &data) { data_ = data; }
  int serialize(char *buf, const int64_t buf_len, int64_t &pos) const;
  int deserialize(const char *buf, const int64_t data_len, int64_t &pos);
  int64_t get_serialize_size() const;
private:
  common::ObString data_;
};

class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      column_exprs_(alloc),
      definition_(),
      root_(),
      pattern_(),
      is_glob_(false)
  {}
  INHERIT_TO_STRING_KV("op_spec", ObOpSpec, K_(is_glob));
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  // the serialized file_sql::Definition, empty for a glob table
  ObFileTableBlob definition_;
  // the only directory the file may live in, the value of secure_file_priv
  ObFileTableBlob root_;
  // the pattern of a glob table
  ObFileTableBlob pattern_;
  bool is_glob_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObFileTableSpec);
};

/**
 * @brief The operator that streams the rows of a file.
 *
 * The operator never materialises the file, it asks the scanner for one row at
 * a time and converts the textual cells into the datums of the columns.
 */
class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
      scan_(NULL),
      glob_files_(),
      glob_index_(0),
      cells_()
  {}
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override { ObOperator::destroy(); }
private:
  int check_file_privilege();
  int open_file_scan();
  int open_glob();
  int set_cell_datum(const int64_t index, const file_sql::Cell &cell);
  int set_text_datum(ObExpr *expr, const char *text, const int64_t length);
private:
  file_sql::Scan *scan_;
  std::vector<std::string> glob_files_;
  int64_t glob_index_;
  std::vector<file_sql::Cell> cells_;
};

} // end namespace sql
} // end namespace oceanbase
