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
#include "sql/parser/ob_parser.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
namespace {

void append_quoted_text(std::string &sql, const common::ObString &value)
{
  sql.append("'");
  for (int64_t i = 0; i < value.length(); ++i) {
    const char item = value.ptr()[i];
    if ('\\' == item || '\'' == item) {
      sql.push_back('\\');
    }
    sql.push_back(item);
  }
  sql.append("'");
}

} // namespace

int ObExplainResolver::resolve_file_table_columns(ObStmt *child_stmt)
{
  int ret = OB_SUCCESS;
  const ObFileTableDef *table_def = NULL;
  if (OB_ISNULL(child_stmt) || !child_stmt->is_select_stmt()) {
    ret = OB_NOT_SUPPORTED;
  } else {
    ObSelectStmt *select_stmt = static_cast<ObSelectStmt *>(child_stmt);
    common::ObIArray<TableItem *> &table_items = select_stmt->get_table_items();
    for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); ++i) {
      TableItem *table_item = table_items.at(i);
      if (OB_NOT_NULL(table_item) && table_item->is_file_table()
          && OB_NOT_NULL(table_item->file_table_def_)) {
        table_def = table_item->file_table_def_;
        break;
      }
    }
    if (OB_ISNULL(table_def)) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_SUCC(ret)) {
    // the columns of the file, in the shape of a describe
    std::string sql;
    for (int64_t i = 0; OB_SUCC(ret) && i < table_def->columns_.count(); ++i) {
      const ObFileTableColumnDef &column = table_def->columns_.at(i);
      if (i > 0) {
        sql.append(" UNION ALL ");
      }
      sql.append("SELECT ");
      append_quoted_text(sql, column.name_);
      sql.append(" AS column_name, ");
      sql.append("'");
      sql.append(file_sql::type_name(static_cast<file_sql::Type>(column.type_)));
      sql.append("'");
      sql.append(" AS column_type, ");
      sql.append(column.nullable_ ? "'YES'" : "'NO'");
      sql.append(" AS `null`, NULL AS `key`, NULL AS `default`, NULL AS `extra` FROM dual");
    }
    ParseResult parse_result;
    ObParser parser(*allocator_, session_info_->get_sql_mode(), session_info_->get_charsets4parser());
    if (OB_FAIL(parser.parse(common::ObString(sql.length(), sql.c_str()), parse_result))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("cannot parse the columns of the file", K(ret));
    } else {
      ObResolver resolver(params_);
      ObStmt *column_stmt = NULL;
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                  *parse_result.result_tree_,
                                  column_stmt))) {
      } else if (OB_ISNULL(column_stmt)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        stmt_ = column_stmt;
      }
      parser.free_result(parse_result);
    }
  }
  if (OB_NOT_SUPPORTED == ret) {
    // the statement does not read a file, the plan is the answer
    ret = OB_SUCCESS;
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
