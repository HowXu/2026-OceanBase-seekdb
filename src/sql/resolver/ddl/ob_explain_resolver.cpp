/*
 * Copyright (c) 2025 OceanBase.
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
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/optimizer/ob_optimizer_util.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
namespace {
const char *describe_type(ObObjType type)
{
  switch (type) {
    case ObNullType: return "NULL";
    case ObTinyIntType: return "TINYINT";
    case ObIntType: return "BIGINT";
    case ObDoubleType: return "DOUBLE";
    case ObFloatType: return "FLOAT";
    case ObDateType: return "DATE";
    case ObDateTimeType: return "DATETIME";
    case ObNumberType: return "DECIMAL";
    default: return ob_is_string_type(type) ? "VARCHAR" : ob_obj_type_str(type);
  }
}
} // namespace

int ObExplainResolver::resolve_file_table_columns(ObStmt *child_stmt)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(child_stmt) || !child_stmt->is_select_stmt()) {
    return ret; // Preserve ordinary EXPLAIN/DESCRIBE semantics.
  }
  ObSelectStmt *source = static_cast<ObSelectStmt *>(child_stmt);
  ObSEArray<ObSelectStmt *, 8> pending;
  bool reads_file = false;
  OZ (pending.push_back(source));
  for (int64_t i = 0; OB_SUCC(ret) && !reads_file && i < pending.count(); ++i) {
    for (int64_t j = 0; !reads_file && j < pending.at(i)->get_table_items().count(); ++j) {
      reads_file = pending.at(i)->get_table_items().at(j)->is_file_table();
    }
    ObSEArray<ObSelectStmt *, 8> children;
    OZ (pending.at(i)->get_child_stmts(children));
    OZ (append_array_no_dup(pending, children));
  }
  if (OB_FAIL(ret) || !reads_file) {
    return ret;
  }
  const char *names[] = {"column_name", "column_type", "null", "key", "default", "extra"};
  ObSEArray<ObSelectStmt *, 8> rows;
  for (int64_t i = 0; OB_SUCC(ret) && i < source->get_select_item_size(); ++i) {
    const SelectItem &item = source->get_select_item(i);
    const char *type = describe_type(item.expr_->get_result_type().get_type());
    bool nullable = !item.expr_->is_not_null_for_read();
    if (item.expr_->is_column_ref_expr()) {
      const ObColumnRefRawExpr &column = *static_cast<ObColumnRefRawExpr *>(item.expr_);
      const TableItem *table = source->get_table_item_by_id(column.get_table_id());
      if (table != nullptr && table->is_file_table() && table->file_table_def_ != nullptr) {
        const int64_t index = column.get_column_id() - OB_APP_MIN_COLUMN_ID;
        CK (index >= 0 && index < table->file_table_def_->columns_.count());
        if (OB_SUCC(ret)) {
          const ObFileTableColumnDef &definition = table->file_table_def_->columns_.at(index);
          bool null_side = false;
          OZ (ObOptimizerUtil::is_table_on_null_side(source, table->table_id_, null_side));
          nullable = definition.nullable_ || null_side;
          // BOOLEAN is represented by a native TINYINT expression.
          if (definition.type_ == static_cast<int32_t>(file_sql::Type::Boolean)) {
            type = "BOOLEAN";
          }
        }
      }
    }
    ObSelectStmt *row = nullptr;
    CK (OB_NOT_NULL(row = create_stmt<ObSelectStmt>()));
    ObString values[] = {item.alias_name_.empty() ? item.expr_name_ : item.alias_name_,
                         ObString(type), ObString(nullable ? "YES" : "NO")};
    for (int64_t j = 0; OB_SUCC(ret) && j < 6; ++j) {
      SelectItem output;
      output.alias_name_ = ObString(names[j]);
      output.expr_name_ = output.alias_name_;
      output.is_real_alias_ = true;
      if (j < 3) {
        ObString value;
        ObConstRawExpr *constant = nullptr;
        OZ (ob_write_string(*allocator_, values[j], value));
        OZ (ObRawExprUtils::build_const_string_expr(*params_.expr_factory_, ObVarcharType,
                                                   value, session_info_->get_nls_collation(), constant));
        output.expr_ = constant;
      } else {
        OZ (ObRawExprUtils::build_null_expr(*params_.expr_factory_, output.expr_));
      }
      OZ (row->add_select_item(output));
    }
    OZ (row->formalize_stmt(session_info_));
    OZ (rows.push_back(row));
  }
  if (OB_SUCC(ret) && rows.count() == 1) {
    stmt_ = rows.at(0);
  } else if (OB_SUCC(ret) && rows.count() > 1) {
    ObSelectStmt *result = create_stmt<ObSelectStmt>();
    CK (OB_NOT_NULL(result));
    OZ (result->get_set_query().assign(rows));
    OX (result->assign_set_op(ObSelectStmt::UNION));
    OX (result->assign_set_all());
    OZ (result->create_select_list_for_set_stmt(*params_.expr_factory_));
    OZ (result->formalize_stmt(session_info_));
    OX (stmt_ = result);
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        if (OB_SUCC(ret) && parse_tree.is_tree_not_param_) {
          /* a describe asks for the columns, not for a plan */
          ret = resolve_file_table_columns(child_stmt);
        }
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
