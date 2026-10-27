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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/session/ob_basic_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"

namespace oceanbase
{
namespace sql
{

/**
 * @brief The ids of the file functions.
 *
 * The ids are the ones of the parser, they tell which file is read and which
 * format the file has.
 */
enum ObFileTableFunctionId
{
  FILE_TABLE_READ_CSV = 1,
  FILE_TABLE_READ_JSON = 2,
  FILE_TABLE_READ_NDJSON = 3,
  FILE_TABLE_READ_PARQUET = 4,
  FILE_TABLE_GLOB = 5,
};

static const char *file_table_type_name(const int32_t type)
{
  switch (type) {
    case static_cast<int32_t>(file_sql::Type::Boolean):
      return "BOOLEAN";
    case static_cast<int32_t>(file_sql::Type::Bigint):
      return "BIGINT";
    case static_cast<int32_t>(file_sql::Type::Double):
      return "DOUBLE";
    case static_cast<int32_t>(file_sql::Type::Date):
      return "DATE";
    case static_cast<int32_t>(file_sql::Type::Datetime):
      return "DATETIME";
    default:
      return "VARCHAR";
  }
}

static int file_table_type_to_obj_meta(const int32_t file_type,
                                       ObObjMeta &meta,
                                       ObAccuracy &accuracy)
{
  int ret = OB_SUCCESS;
  switch (static_cast<file_sql::Type>(file_type)) {
    case file_sql::Type::Boolean:
      meta.set_tinyint();
      accuracy.set_precision(PRECISION_UNKNOWN_YET);
      accuracy.set_scale(0);
      break;
    case file_sql::Type::Bigint:
      meta.set_int();
      accuracy.set_precision(PRECISION_UNKNOWN_YET);
      accuracy.set_scale(0);
      break;
    case file_sql::Type::Double:
      meta.set_double();
      accuracy.set_precision(PRECISION_UNKNOWN_YET);
      accuracy.set_scale(0);
      break;
    case file_sql::Type::Date:
      meta.set_date();
      accuracy.set_precision(PRECISION_UNKNOWN_YET);
      accuracy.set_scale(0);
      break;
    case file_sql::Type::Datetime:
      meta.set_datetime();
      accuracy.set_precision(PRECISION_UNKNOWN_YET);
      accuracy.set_scale(SCALE_UNKNOWN_YET);
      break;
    default:
      meta.set_varchar();
      accuracy.set_precision(PRECISION_UNKNOWN_YET);
      accuracy.set_scale(SCALE_UNKNOWN_YET);
      break;
  }
  return ret;
}

/**
 * @brief Read the value of a named argument of a file function.
 */
static int file_table_option_value(const ParseNode *node,
                                   ObString &string_value,
                                   int64_t &int_value,
                                   bool &bool_value,
                                   int &value_type)
{
  int ret = OB_SUCCESS;
  value_type = 0;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_VARCHAR == node->type_) {
    string_value.assign_ptr(node->str_value_, node->str_len_);
    value_type = 1;
  } else if (T_BOOL == node->type_) {
    bool_value = (0 != node->value_);
    value_type = 2;
  } else if (T_INT == node->type_ || T_NUMBER == node->type_ || T_UINT64 == node->type_) {
    int_value = node->value_;
    value_type = 3;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the value of the option of a file function");
  }
  return ret;
}

/**
 * @brief Read the options of a file function.
 */
static int file_table_options(const ParseNode *args_node,
                               file_sql::Options &options,
                               bool &has_format)
{
  int ret = OB_SUCCESS;
  has_format = false;
  if (OB_ISNULL(args_node)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 1; OB_SUCC(ret) && i < args_node->num_child_; ++i) {
      const ParseNode *arg_node = args_node->children_[i];
      const ParseNode *name_node = NULL;
      const ParseNode *value_node = NULL;
      ObString name;
      ObString string_value;
      int64_t int_value = 0;
      bool bool_value = false;
      int value_type = 0;
      if (OB_ISNULL(arg_node) || 2 != arg_node->num_child_) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      name_node = arg_node->children_[0];
      value_node = arg_node->children_[1];
      if (OB_ISNULL(name_node) || T_VARCHAR != name_node->type_) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      name.assign_ptr(name_node->str_value_, name_node->str_len_);
      if (OB_FAIL(file_table_option_value(value_node, string_value, int_value, bool_value, value_type))) {
        break;
      }
      char lower_name[32];
      int64_t name_length = name.length() < 31 ? name.length() : 31;
      for (int64_t index = 0; index < name_length; ++index) {
        lower_name[index] = static_cast<char>(::tolower(name.ptr()[index]));
      }
      lower_name[name_length] = '\0';
      if (0 == ::strcmp(lower_name, "header")) {
        if (2 != value_type) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the header option of a file function");
          break;
        }
        options.header = bool_value;
      } else if (0 == ::strcmp(lower_name, "delim") || 0 == ::strcmp(lower_name, "delimiter")) {
        if (1 != value_type || 1 != string_value.length()) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the delim option of a file function");
          break;
        }
        options.delimiter = string_value.ptr()[0];
      } else if (0 == ::strcmp(lower_name, "quote")) {
        if (1 != value_type || 1 != string_value.length()) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the quote option of a file function");
          break;
        }
        options.quote = string_value.ptr()[0];
      } else if (0 == ::strcmp(lower_name, "escape")) {
        if (1 != value_type || 1 != string_value.length()) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the escape option of a file function");
          break;
        }
        options.escape = string_value.ptr()[0];
      } else if (0 == ::strcmp(lower_name, "null_string") || 0 == ::strcmp(lower_name, "nullstr")) {
        if (1 != value_type) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the null_string option of a file function");
          break;
        }
        options.null_string = std::string(string_value.ptr(), string_value.length());
      } else if (0 == ::strcmp(lower_name, "sample_size")) {
        if (3 != value_type) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the sample_size option of a file function");
          break;
        }
        options.sample_size = int_value;
      } else if (0 == ::strcmp(lower_name, "format")) {
        if (1 != value_type) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the format option of a file function");
          break;
        }
        char lower_format[32];
        int64_t format_length = string_value.length() < 31 ? string_value.length() : 31;
        for (int64_t index = 0; index < format_length; ++index) {
          lower_format[index] = static_cast<char>(::tolower(string_value.ptr()[index]));
        }
        lower_format[format_length] = '\0';
        if (0 == ::strcmp(lower_format, "csv")) {
          options.format = file_sql::Format::Csv;
        } else if (0 == ::strcmp(lower_format, "json") ||
                   0 == ::strcmp(lower_format, "jsonl") ||
                   0 == ::strcmp(lower_format, "ndjson") ||
                   0 == ::strcmp(lower_format, "newline_delimited")) {
          options.format = file_sql::Format::Json;
        } else if (0 == ::strcmp(lower_format, "parquet")) {
          options.format = file_sql::Format::Parquet;
        } else if (0 == ::strcmp(lower_format, "auto")) {
          options.format = file_sql::Format::Auto;
        } else {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "the format of a file function");
          break;
        }
        has_format = true;
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "the option of a file function");
        break;
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_table_item(const ParseNode &parse_tree,
                                            TableItem *&tbl_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  TableItem *item = NULL;
  ObFileTableDef *table_def = NULL;
  ObString alias_name;
  ObString path;
  ObString secure_file_priv;
  file_sql::Options options;
  bool enable_file_sql = false;
  bool has_format = false;
  int64_t function_id = 0;
  file_sql::Definition definition;
  std::vector<std::string> glob_files;

  CK (OB_LIKELY(T_FILE_TABLE_EXPRESSION == parse_tree.type_));
  CK (OB_LIKELY(3 == parse_tree.num_child_));
  if (OB_SUCC(ret) && (OB_ISNULL(stmt) || OB_ISNULL(allocator_))) {
    ret = OB_NOT_INIT;
  }
  // the feature is switched off by default
  if (OB_SUCC(ret) && OB_FAIL(session_info_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL,
                                                              enable_file_sql))) {
  } else if (OB_SUCC(ret) && !enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "the file functions");
  }
  if (OB_SUCC(ret)) {
    const ParseNode *function_node = parse_tree.children_[0];
    const ParseNode *args_node = parse_tree.children_[1];
    const ParseNode *alias_node = parse_tree.children_[2];
    if (OB_ISNULL(function_node) || OB_ISNULL(args_node) || 0 == args_node->num_child_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      function_id = function_node->value_;
      const ParseNode *path_node = args_node->children_[0];
      if (OB_ISNULL(path_node) || T_VARCHAR != path_node->type_) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        path.assign_ptr(path_node->str_value_, path_node->str_len_);
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(file_table_options(args_node, options, has_format))) {
    } else if (OB_SUCC(ret) && OB_FAIL(session_info_->get_secure_file_priv(secure_file_priv))) {
    } else if (OB_SUCC(ret) && OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, path))) {
    }
    // the format of the file is the one of the function when it is not given
    if (OB_SUCC(ret) && !has_format) {
      switch (function_id) {
        case FILE_TABLE_READ_CSV:
          options.format = file_sql::Format::Csv;
          break;
        case FILE_TABLE_READ_JSON:
        case FILE_TABLE_READ_NDJSON:
          options.format = file_sql::Format::Json;
          break;
        case FILE_TABLE_READ_PARQUET:
          options.format = file_sql::Format::Parquet;
          break;
        default:
          break;
      }
    }
    // only the whole file is read, the sample size is not supported
    if (OB_SUCC(ret) && -1 != options.sample_size) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "the sample_size option, only -1 is supported");
    }
    // the file is read to know the columns it holds
    if (OB_SUCC(ret)) {
      try {
        if (FILE_TABLE_GLOB == function_id) {
          glob_files = file_sql::discover(std::string(path.ptr(), path.length()),
                                          std::string(secure_file_priv.ptr(), secure_file_priv.length()));
        } else {
          definition = file_sql::infer(std::string(path.ptr(), path.length()),
                                       std::string(secure_file_priv.ptr(), secure_file_priv.length()),
                                       options);
        }
      } catch (const file_sql::Error &error) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", error.what());
      } catch (const std::exception &exception) {
        ret = OB_ERR_UNEXPECTED;
        LOG_USER_ERROR(OB_ERR_UNEXPECTED, "%s", exception.what());
      }
    }
    if (OB_SUCC(ret) && OB_ISNULL(item = stmt->create_table_item(*allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_SUCC(ret) && OB_ISNULL(table_def = static_cast<ObFileTableDef *>(
                                    allocator_->alloc(sizeof(ObFileTableDef))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_SUCC(ret)) {
      table_def = new (table_def) ObFileTableDef();
      item->file_table_def_ = table_def;
      table_def->is_glob_ = (FILE_TABLE_GLOB == function_id);
      if (OB_FAIL(ob_write_string(*allocator_, path, table_def->path_))) {
      } else if (OB_FAIL(ob_write_string(*allocator_, path, table_def->pattern_))) {
      } else if (OB_FAIL(ob_write_string(*allocator_, secure_file_priv, table_def->root_))) {
      } else if (table_def->is_glob_) {
        // a glob table has one column that holds the name of the file
        ObFileTableColumnDef column;
        column.name_ = ObString("file");
        column.type_ = static_cast<int32_t>(file_sql::Type::Varchar);
        column.nullable_ = false;
        if (OB_FAIL(table_def->columns_.push_back(column))) {
        }
      } else {
        const std::string blob = definition.serialize();
        if (OB_FAIL(ob_write_string(*allocator_,
                                    ObString(static_cast<int32_t>(blob.size()), blob.data()),
                                    table_def->definition_))) {
        } else {
          for (size_t index = 0; index < definition.columns.size(); ++index) {
            const file_sql::Column &source = definition.columns[index];
            ObFileTableColumnDef column;
            column.type_ = static_cast<int32_t>(source.type);
            column.nullable_ = source.nullable;
            column.scale_ = source.scale;
            if (OB_FAIL(ob_write_string(*allocator_,
                                        ObString(static_cast<int32_t>(source.name.size()), source.name.data()),
                                        column.name_))) {
              break;
            } else if (OB_FAIL(table_def->columns_.push_back(column))) {
              break;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_NOT_NULL(alias_node) && alias_node->str_len_ > 0) {
        alias_name.assign_ptr(const_cast<char *>(alias_node->str_value_),
                              static_cast<int32_t>(alias_node->str_len_));
      } else if (OB_FAIL(stmt->generate_func_table_name(*allocator_, alias_name))) {
      }
      item->table_name_ = alias_name;
      item->alias_name_ = alias_name;
      item->table_id_ = generate_table_id();
      item->type_ = TableItem::FILE_TABLE;
      if (OB_SUCC(ret) && OB_FAIL(stmt->add_table_item(session_info_, item))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObSEArray<ColumnItem, 16> col_items;
      if (OB_FAIL(resolve_file_table_column_item(*item, col_items))) {
      }
    }
  }
  OX (tbl_item = item);
  return ret;
}

int ObDMLResolver::resolve_file_table_column_item(const TableItem &table_item,
                                                   ObIArray<ColumnItem> &col_items)
{
  int ret = OB_SUCCESS;
  CK (OB_LIKELY(table_item.is_file_table()));
  CK (OB_NOT_NULL(table_item.file_table_def_));
  const ObFileTableDef &table_def = *table_item.file_table_def_;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_def.columns_.count(); ++i) {
    const ObFileTableColumnDef &column = table_def.columns_.at(i);
    ObObjMeta meta;
    ObAccuracy accuracy;
    ColumnItem *col_item = NULL;
    if (OB_FAIL(file_table_type_to_obj_meta(column.type_, meta, accuracy))) {
    } else if (meta.is_string_type()) {
      meta.set_collation_type(session_info_->get_nls_collation());
    }
    if (OB_SUCC(ret)) {
      ret = resolve_file_table_column_item(table_item,
                                           meta,
                                           accuracy,
                                           column.name_,
                                           OB_APP_MIN_COLUMN_ID + i,
                                           col_item);
    }
    if (OB_SUCC(ret) && OB_NOT_NULL(col_item)) {
      ret = col_items.push_back(*col_item);
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_table_column_item(const TableItem &table_item,
                                                   const common::ObObjMeta &meta_type,
                                                   const common::ObAccuracy &accuracy,
                                                   const ObString &column_name,
                                                   uint64_t column_id,
                                                   ColumnItem *&col_item)
{
  int ret = OB_SUCCESS;
  ObColumnRefRawExpr *col_expr = NULL;
  ColumnItem column_item;
  sql::ObRawExprResType result_type;
  ObDMLStmt *stmt = get_stmt();
  CK (OB_NOT_NULL(stmt));
  CK (OB_NOT_NULL(params_.expr_factory_));
  CK (OB_LIKELY(table_item.is_file_table()));
  OZ (params_.expr_factory_->create_raw_expr(T_REF_COLUMN, col_expr));
  CK (OB_NOT_NULL(col_expr));
  OX (col_expr->set_ref_id(table_item.table_id_, column_id));
  OX (result_type.set_meta(meta_type));
  OX (result_type.set_accuracy(accuracy));
  OX (col_expr->set_result_type(result_type));
  if (table_item.get_object_name().empty()) {
    OX (col_expr->set_column_name(column_name));
  } else {
    OX (col_expr->set_column_attr(table_item.get_object_name(), column_name));
  }
  OX (col_expr->set_database_name(table_item.database_name_));
  OX (column_item.expr_ = col_expr);
  OX (column_item.table_id_ = col_expr->get_table_id());
  OX (column_item.column_id_ = col_expr->get_column_id());
  OX (column_item.column_name_ = col_expr->get_column_name());
  OZ (col_expr->extract_info());
  OZ (stmt->add_column_item(column_item));
  OX (col_item = stmt->get_column_item(stmt->get_column_size() - 1));
  return ret;
}

int ObDMLResolver::resolve_file_table_column_item(const TableItem &table_item,
                                                   const ObString &column_name,
                                                   ColumnItem *&col_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  CK (OB_NOT_NULL(stmt));
  CK (OB_LIKELY(table_item.is_file_table()));
  if (OB_FAIL(ret)) {
    // do nothing ...
  } else if (NULL != (col_item = stmt->get_column_item(table_item.table_id_, column_name))) {
    //exist, ignore resolve...
  } else {
    ObSEArray<ColumnItem, 16> col_items;
    OZ (ObResolverUtils::check_file_table_column_exist(table_item,
                                                       params_,
                                                       column_name));
    OZ (resolve_file_table_column_item(table_item, col_items));
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
      ColumnItem* column_item = stmt->get_column_item(i);
      CK (OB_NOT_NULL(column_item));
      if (OB_SUCC(ret)
          && column_item->table_id_ == table_item.table_id_
          && ObCharset::case_insensitive_equal(column_item->column_name_, column_name)) {
        col_item = column_item;
        break;
      }
    }
    if (OB_SUCC(ret) && OB_ISNULL(col_item)) {
      ret = OB_ERR_BAD_FIELD_ERROR;
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
