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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr.h"
#include "sql/session/ob_basic_session_info.h"
#include "lib/utility/utility.h"
#include "common/timezone/ob_time_convert.h"

#include <cstdlib>
#include <cerrno>

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER_INHERIT(ObFileTableSpec,
                            ObOpSpec,
                            column_exprs_,
                            definition_,
                            root_,
                            pattern_,
                            is_glob_);

int ObFileTableBlob::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  return serialization::encode_vstr(buf, buf_len, pos, data_.ptr(), data_.length());
}

int ObFileTableBlob::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t length = 0;
  const char *source = serialization::decode_vstr(buf, data_len, pos, &length);
  if (OB_ISNULL(source) || length < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    data_ = ObString(static_cast<int32_t>(length), source);
  }
  return ret;
}

int64_t ObFileTableBlob::get_serialize_size() const
{
  return serialization::encoded_length_vstr(data_.length());
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_glob_) {
    ret = open_glob();
  } else {
    ret = open_file_scan();
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    glob_index_ = 0;
    cells_.clear();
    if (OB_NOT_NULL(scan_)) {
      try {
        scan_->rescan();
      } catch (const file_sql::Error &error) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", error.what());
      }
    }
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(scan_)) {
    delete scan_;
    scan_ = NULL;
  }
  glob_files_.clear();
  glob_index_ = 0;
  cells_.clear();
  return ret;
}

int ObFileTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  try {
    const std::string blob = MY_SPEC.definition_.str();
    file_sql::Definition definition = file_sql::Definition::deserialize(blob);
    std::vector<int> projection;
    for (size_t index = 0; index < definition.columns.size(); ++index) {
      projection.push_back(static_cast<int>(index));
    }
    scan_ = new file_sql::Scan(definition, MY_SPEC.root_.str(), projection);
  } catch (const file_sql::Error &error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", error.what());
  } catch (const std::exception &exception) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", exception.what());
  }
  return ret;
}

int ObFileTableOp::open_glob()
{
  int ret = OB_SUCCESS;
  try {
    glob_files_ = file_sql::discover(MY_SPEC.pattern_.str(), MY_SPEC.root_.str());
  } catch (const file_sql::Error &error) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", error.what());
  } catch (const std::exception &exception) {
    ret = OB_ERR_UNEXPECTED;
    LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", exception.what());
  }
  glob_index_ = 0;
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(try_check_status())) {
  } else {
    clear_evaluated_flag();
    if (MY_SPEC.is_glob_) {
      if (glob_index_ >= static_cast<int64_t>(glob_files_.size())) {
        ret = OB_ITER_END;
      } else {
        const std::string &file = glob_files_[glob_index_++];
        file_sql::Cell cell;
        cell.text = file;
        cell.is_null = false;
        cells_.clear();
        cells_.push_back(cell);
        if (OB_SUCC(ret) && OB_FAIL(set_cell_datum(0, cell))) {
        }
      }
    } else if (OB_ISNULL(scan_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      bool has_row = false;
      try {
        has_row = scan_->next(cells_);
      } catch (const file_sql::Error &error) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", error.what());
      }
      if (OB_SUCC(ret)) {
        if (!has_row) {
          ret = OB_ITER_END;
        } else {
          for (int64_t index = 0; OB_SUCC(ret) && index < static_cast<int64_t>(cells_.size()); ++index) {
            if (OB_FAIL(set_cell_datum(index, cells_[index]))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObFileTableOp::set_cell_datum(const int64_t index, const file_sql::Cell &cell)
{
  int ret = OB_SUCCESS;
  if (index < 0 || index >= MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObExpr *expr = MY_SPEC.column_exprs_.at(index);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (cell.is_null) {
      datum.set_null();
    } else if (OB_FAIL(set_text_datum(expr, cell.text.data(), static_cast<int64_t>(cell.text.size())))) {
    }
    if (OB_SUCC(ret)) {
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFileTableOp::set_text_datum(ObExpr *expr, const char *text, const int64_t length)
{
  int ret = OB_SUCCESS;
  const ObObjType type = expr->datum_meta_.type_;
  ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
  if (ObTinyIntType == type) {
    // a boolean column keeps the value true or false
    int value = 0;
    if (4 == length && 0 == ::strncasecmp(text, "true", 4)) {
      value = 1;
    } else if (5 == length && 0 == ::strncasecmp(text, "false", 5)) {
      value = 0;
    } else if (1 == length && ('0' == text[0] || '1' == text[0])) {
      value = text[0] - '0';
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the value '%.*s' is not a boolean", length, text);
    }
    if (OB_SUCC(ret)) {
      datum.set_int(value);
    }
  } else if (ObIntType == type || ObInt32Type == type ||
             ObUInt64Type == type || ObUInt32Type == type) {
    errno = 0;
    char *end = NULL;
    const long long value = ::strtoll(text, &end, 10);
    if (OB_UNLIKELY(0 != errno || end != text + length)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the value '%.*s' is not an integer", length, text);
    } else {
      datum.set_int(value);
    }
  } else if (ObDoubleType == type || ObFloatType == type) {
    errno = 0;
    char *end = NULL;
    const double value = ::strtod(text, &end);
    if (OB_UNLIKELY(0 != errno || end != text + length)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the value '%.*s' is not a number", length, text);
    } else {
      datum.set_double(value);
    }
  } else if (ObDateType == type) {
    int32_t value = 0;
    if (OB_FAIL(ObTimeConverter::str_to_date(ObString(length, text), value))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the value '%.*s' is not a date", length, text);
    } else {
      datum.set_date(value);
    }
  } else if (ObDateTimeType == type) {
    int64_t value = 0;
    ObTimeConvertCtx cvrt_ctx(TZ_INFO(GET_MY_SESSION(ctx_)), false);
    if (OB_FAIL(ObTimeConverter::str_to_datetime(ObString(length, text), cvrt_ctx, value))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_USER_ERROR(OB_ERR_UNEXPECTED, "the value '%.*s' is not a datetime", length, text);
    } else {
      datum.set_datetime(value);
    }
  } else {
    ObString source(length, text);
    ObString target;
    ObExprStrResAlloc res_alloc(*expr, eval_ctx_);
    if (OB_FAIL(deep_copy_ob_string(res_alloc, source, target))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      datum.set_string(target);
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
