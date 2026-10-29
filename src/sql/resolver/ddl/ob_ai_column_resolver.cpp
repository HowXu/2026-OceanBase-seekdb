/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_service.h"
#include "sql/resolver/ob_schema_checker.h"
#include "query/vector/ob_vector_index_util.h"
#include "common/json_type/ob_json_tree.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace
{
ObString ai_node_text(const ParseNode &node)
{
  return ObString(static_cast<int32_t>(node.str_len_), node.str_value_);
}

int ai_function_arguments(const ParseNode &expr, const ParseNode *&args, bool &embedding)
{
  int ret = OB_SUCCESS;
  args = nullptr;
  if (expr.type_ != T_FUN_SYS || expr.num_child_ != 2 || nullptr == expr.children_
      || nullptr == expr.children_[0] || nullptr == expr.children_[1]) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const ObString name = ai_node_text(*expr.children_[0]);
    embedding = 0 == name.case_compare("AI_EMBED");
    args = expr.children_[1];
    if ((!embedding && 0 != name.case_compare("AI_COMPLETE"))
        || args->type_ != T_EXPR_LIST || args->num_child_ < 2 || args->num_child_ > 3
        || nullptr == args->children_ || nullptr == args->children_[0]
        || args->children_[0]->type_ != T_VARCHAR || ai_node_text(*args->children_[0]).empty()) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int ai_resolve_options(const ParseNode *options, int64_t &max_retries)
{
  int ret = OB_SUCCESS;
  bool has_mode = false;
  bool has_retries = false;
  max_retries = 2;
  if (nullptr != options) {
    if (options->type_ != T_AI_COLUMN_OPTIONS) {
      ret = OB_NOT_SUPPORTED;
    }
    for (int i = 0; OB_SUCC(ret) && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (nullptr == option || option->type_ != T_AI_COLUMN_OPTION || option->num_child_ != 2
          || nullptr == option->children_[0] || nullptr == option->children_[1]) {
        ret = OB_NOT_SUPPORTED;
      } else {
        const ObString name = ai_node_text(*option->children_[0]);
        const ParseNode &value = *option->children_[1];
        if (0 == name.case_compare("sync_mode")) {
          if (has_mode || value.type_ != T_VARCHAR || 0 != ai_node_text(value).case_compare("async")) {
            ret = OB_NOT_SUPPORTED;
          }
          has_mode = true;
        } else if (0 == name.case_compare("max_retries")) {
          if (has_retries || value.type_ != T_INT || value.value_ < 0) {
            ret = OB_NOT_SUPPORTED;
          } else {
            max_retries = value.value_;
          }
          has_retries = true;
        } else {
          ret = OB_NOT_SUPPORTED;
        }
      }
    }
  }
  return ret;
}

int ai_check_input(const ObRawExpr &expr)
{
  int ret = OB_SUCCESS;
  if (expr.get_expr_type() == T_FUN_SYS_AI_COMPLETE || expr.get_expr_type() == T_FUN_SYS_AI_EMBED
      || expr.get_expr_type() == T_FUN_SYS_AI_RERANK) {
    ret = OB_NOT_SUPPORTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.get_param_count(); ++i) {
    if (nullptr == expr.get_param_expr(i)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(SMART_CALL(ai_check_input(*expr.get_param_expr(i))))) {
    }
  }
  return ret;
}

int ai_print_expression(ObIAllocator &allocator, ObRawExpr *expr, ObString &text)
{
  int ret = OB_SUCCESS;
  for (int64_t capacity = 1024; capacity <= OB_MAX_SQL_LENGTH; capacity *= 2) {
    char *buf = static_cast<char *>(allocator.alloc(capacity));
    int64_t pos = 0;
    if (nullptr == buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      break;
    }
    ObRawExprPrinter printer(buf, capacity, &pos, nullptr);
    ret = printer.do_print(expr, T_NONE_SCOPE, true);
    if (OB_SUCC(ret)) {
      text.assign_ptr(buf, static_cast<int32_t>(pos));
      break;
    } else if (ret != OB_SIZE_OVERFLOW && ret != OB_BUF_NOT_ENOUGH) {
      break;
    }
  }
  return ret;
}

int ai_build_normalized_definition(ObIAllocator &allocator, const ObAiModelSchema &model,
    ObRawExpr &expr, const ObColumnSchemaV2 &column, const int64_t dimension, ObString &text)
{
  int ret = OB_SUCCESS;
  ObString input_sql;
  ObString config_sql("JSON_OBJECT()");
  const ObObjType type = column.get_data_type();
  const char *hint = "";
  if (dimension == 0) {
    if (ob_is_json(type)) {
      hint = "Return exactly one valid JSON value with no surrounding text.";
    } else if (ob_is_integer_type(type)) {
      hint = "Return exactly one base-10 integer with no surrounding text.";
    } else if (ob_is_numeric_type(type) || ob_is_decimal_int(type)) {
      hint = "Return exactly one finite numeric value with no surrounding text.";
    } else {
      hint = "Return only the generated text.";
    }
  }
  if (OB_FAIL(ai_print_expression(allocator, expr.get_param_expr(1), input_sql))) {
  } else if (expr.get_param_count() == 3 && dimension == 0
      && OB_FAIL(ai_print_expression(allocator, expr.get_param_expr(2), config_sql))) {
  } else {
    // This is a structured invocation specification, not SQL rewritten by text substitution.
    // The worker evaluates input/config separately and passes the type hint as a system message.
    ObJsonObject spec(&allocator);
    ObJsonString function(ObString(dimension > 0 ? "AI_EMBED" : "AI_COMPLETE"));
    ObJsonString model_name(model.get_name());
    ObJsonString input(input_sql);
    ObJsonString config(config_sql);
    ObJsonString system_prompt{ObString(hint)};
    ObJsonInt dim(dimension);
    ObJsonBuffer buffer(&allocator);
    if (OB_FAIL(spec.add("function", &function))) {
    } else if (OB_FAIL(spec.add("model", &model_name))) {
    } else if (OB_FAIL(spec.add("input_expr", &input))) {
    } else if (OB_FAIL(spec.add("config_expr", &config))) {
    } else if (OB_FAIL(spec.add("system_prompt", &system_prompt))) {
    } else if (OB_FAIL(spec.add("dimension", &dim))) {
    } else if (OB_FAIL(spec.print(buffer, 0))) {
    } else {
      text = buffer.string();
    }
  }
  return ret;
}
} // namespace

int AIColumnResolver::resolve_column_attribute(const ParseNode &node, ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  int64_t max_retries = 2;
  const ParseNode *args = nullptr;
  bool embedding = false;
  AIColumnSchema definition;
  if (node.type_ != T_AI_COLUMN || node.num_child_ != 2 || nullptr == node.children_
      || nullptr == node.children_[0]) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(ai_function_arguments(*node.children_[0], args, embedding))) {
  } else if (OB_FAIL(ai_resolve_options(node.children_[1], max_retries))) {
  } else if (column.is_generated_column() || column.is_autoincrement() || column.is_hidden()) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(definition.set_max_retries(max_retries))) {
  } else if (OB_FAIL(definition.set_column_name(column.get_column_name_str()))) {
  } else if (OB_FAIL(definition.set_expression(ai_node_text(*node.children_[0])))) {
  } else if (OB_FAIL(column.set_ai_column_schema(definition))) {
  }
  if (ret == OB_NOT_SUPPORTED) {
    LOG_WARN("invalid asynchronous AI column definition or options", K(ret));
  }
  return ret;
}

int AIColumnResolver::resolve_expression(ObColumnSchemaV2 &column, const ObTableSchema &table,
    const ObAiModelSchema &model, ObSQLSessionInfo &session, const ObSchemaChecker *checker)
{
  int ret = OB_SUCCESS;
  AIColumnSchema *definition = column.get_ai_column_schema();
  ObArenaAllocator allocator("AIColResolve");
  ObRawExprFactory factory(allocator);
  const ParseNode *node = nullptr;
  const ParseNode *args = nullptr;
  bool embedding = false;
  ObRawExpr *expr = nullptr;
  ObSEArray<ObQualifiedName, 8> columns;
  ObSEArray<uint64_t, 8> dependencies;
  int64_t dimension = 0;
  ObString normalized;
  if (nullptr == definition) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObRawExprUtils::parse_expr_node_from_str(definition->get_expression(),
      session.get_charsets4parser(), allocator, node, session.get_sql_mode()))) {
  } else if (nullptr == node) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ai_function_arguments(*node, args, embedding))) {
  } else if ((embedding && model.get_type() != EndpointType::DENSE_EMBEDDING)
      || (!embedding && model.get_type() != EndpointType::COMPLETION)
      || model.get_name().case_compare(ai_node_text(*args->children_[0])) != 0) {
    ret = OB_NOT_SUPPORTED;
  } else if (embedding) {
    if (!column.is_collection() || OB_SUCCESS != ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
        column.get_extended_type_info(), dimension) || dimension <= 0) {
      ret = OB_NOT_SUPPORTED;
    } else if (args->num_child_ == 3 && (nullptr == args->children_[2]
        || args->children_[2]->type_ != T_INT || args->children_[2]->value_ <= 0)) {
      ret = OB_NOT_SUPPORTED;
    } else if (args->num_child_ == 3 && args->children_[2]->value_ != dimension) {
      ret = OB_ERR_INVALID_VECTOR_DIM;
      LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(dimension),
          static_cast<unsigned>(args->children_[2]->value_));
    }
  } else if (!ob_is_json(column.get_data_type()) && !ob_is_string_tc(column.get_data_type())
      && !ob_is_integer_type(column.get_data_type()) && !ob_is_numeric_type(column.get_data_type())
      && !ob_is_decimal_int(column.get_data_type())) {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(factory, session, *node,
      expr, columns, nullptr, nullptr, checker))) {
  } else if (nullptr == expr) {
    ret = OB_ERR_UNEXPECTED;
  }
  ObSEArray<ObRawExpr *, 8> resolved;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObQualifiedName &name = columns.at(i);
    ObRawExpr *real = nullptr;
    if (name.is_sys_func()) {
      if (OB_FAIL(name.access_idents_.at(0).check_param_num())) {
      } else {
        real = name.access_idents_.at(0).sys_func_expr_;
      }
    } else {
      const ObColumnSchemaV2 *source = table.get_column_schema(name.col_name_);
      if (!name.database_name_.empty() || (!name.tbl_name_.empty()
          && name.tbl_name_.case_compare(table.get_table_name_str()) != 0)
          || nullptr == source || source->is_ai_column() || source->is_hidden()
          || source->get_column_id() == column.get_column_id() || nullptr == name.ref_expr_) {
        ret = OB_NOT_SUPPORTED;
      } else if (OB_FAIL(ObRawExprUtils::init_column_expr(*source, &session, *name.ref_expr_))) {
      } else {
        name.ref_expr_->set_ref_id(table.get_table_id(), source->get_column_id());
        real = name.ref_expr_;
        bool found = false;
        for (int64_t j = 0; !found && j < dependencies.count(); ++j) {
          found = dependencies.at(j) == source->get_column_id();
        }
        if (!found && OB_FAIL(dependencies.push_back(source->get_column_id()))) {
        }
      }
    }
    if (OB_SUCC(ret) && nullptr == real) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < resolved.count(); ++j) {
      if (OB_FAIL(ObRawExprUtils::replace_ref_column(real, columns.at(j).ref_expr_, resolved.at(j)))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(resolved.push_back(real))) {
    } else if (OB_FAIL(ObRawExprUtils::replace_ref_column(expr, name.ref_expr_, real))) {
    }
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
    if (OB_FAIL(ai_check_input(*expr->get_param_expr(i)))) {
    } else if (OB_FAIL(ObRawExprUtils::check_deterministic(expr->get_param_expr(i), allocator,
        ObResolverUtils::CHECK_FOR_GENERATED_COLUMN))) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr->formalize(&session))) {
  } else if (OB_FAIL(ai_build_normalized_definition(allocator, model, *expr, column, dimension, normalized))) {
  } else if (OB_FAIL(definition->set_dependency_ids(dependencies))) {
  } else if (OB_FAIL(definition->set_normalized_expr(normalized))) {
  }
  if (ret == OB_NOT_SUPPORTED) {
    LOG_WARN("invalid AI column expression, model, result type or dependency", K(ret));
  }
  return ret;
}

int AIColumnResolver::prepare_alter_column(const ObTableSchema &base, AlterTableSchema &changes,
    AlterColumnSchema &column, ObTableSchema &working)
{
  int ret = OB_SUCCESS;
  const bool adding = column.alter_type_ == OB_DDL_ADD_COLUMN;
  const ObColumnSchemaV2 *original = adding ? nullptr
      : base.get_column_schema(column.get_origin_column_name());
  if (!adding && nullptr == original) {
    ret = OB_ERR_BAD_FIELD_ERROR;
  } else if (!column.is_ai_column() && nullptr != original && original->is_ai_column()
      && OB_FAIL(column.set_ai_column_schema(*original->get_ai_column_schema()))) {
  } else if (!column.is_ai_column()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(working.assign(base))) {
  }
  uint64_t last_id = base.get_max_used_column_id();
  for (int64_t i = 0; OB_SUCC(ret) && i < changes.get_column_count(); ++i) {
    AlterColumnSchema *previous = static_cast<AlterColumnSchema *>(changes.get_column_schema_by_idx(i));
    if (nullptr == previous) {
      ret = OB_ERR_UNEXPECTED;
    } else if (previous->alter_type_ == OB_DDL_ADD_COLUMN) {
      // The root DDL path allocates IDs in this exact action order. Its based-
      // schema version check rejects a concurrently changed source schema.
      previous->set_column_id(++last_id);
      previous->set_table_id(base.get_table_id());
      ret = working.add_column(static_cast<const ObColumnSchemaV2 &>(*previous));
    } else {
      const ObColumnSchemaV2 *old = working.get_column_schema(previous->get_origin_column_name());
      if (nullptr != old && OB_FAIL(working.delete_column(old->get_column_name_str()))) {
      } else if (previous->alter_type_ != OB_DDL_DROP_COLUMN) {
        ret = working.add_column(static_cast<const ObColumnSchemaV2 &>(*previous));
      }
    }
  }
  if (OB_SUCC(ret)) {
    column.set_table_id(base.get_table_id());
    column.set_column_id(adding ? ++last_id : original->get_column_id());
    AIColumnSchema &definition = *column.get_ai_column_schema();
    if (OB_FAIL(definition.set_column_name(column.get_column_name_str()))) {
    } else if (nullptr != original && original->is_ai_column()) {
      const AIColumnSchema &old = *original->get_ai_column_schema();
      definition.set_state_column_id(old.get_state_column_id());
      definition.set_request_column_id(old.get_request_column_id());
      definition.set_result_spec_column_id(old.get_result_spec_column_id());
    }
    if (OB_SUCC(ret) && !adding) {
      ret = working.delete_column(original->get_column_name_str());
    }
    if (OB_SUCC(ret)) {
      ret = working.add_column(static_cast<const ObColumnSchemaV2 &>(column));
    }
    if (OB_SUCC(ret) && (nullptr == original || !original->is_ai_column())) {
      ret = add_hidden_columns(working, column, last_id);
    }
  }
  return ret;
}

int AIColumnResolver::finish_alter_table(const ObTableSchema &base, AlterTableSchema &changes)
{
  int ret = OB_SUCCESS;
  auto action_for = [&](const ObColumnSchemaV2 &column) -> const AlterColumnSchema * {
    const AlterColumnSchema *found = nullptr;
    for (int64_t i = 0; i < changes.get_column_count(); ++i) {
      const AlterColumnSchema *action =
          static_cast<const AlterColumnSchema *>(changes.get_column_schema_by_idx(i));
      if (action->get_origin_column_name().case_compare(column.get_column_name_str()) == 0
          || (action->get_column_id() != OB_INVALID_ID
              && action->get_column_id() == column.get_column_id())) {
        found = action;
      }
    }
    return found;
  };
  const int64_t count = base.get_column_count() + changes.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    const ObColumnSchemaV2 *column = i < base.get_column_count()
        ? base.get_column_schema_by_idx(i) : changes.get_column_schema_by_idx(i - base.get_column_count());
    if (!column->is_ai_column()) {
      continue;
    }
    const AlterColumnSchema *replacement = action_for(*column);
    if (nullptr != replacement && replacement->alter_type_ == OB_DDL_DROP_COLUMN) {
      continue;
    } else if (nullptr != replacement && !replacement->is_ai_column()) {
      ret = OB_NOT_SUPPORTED;
      break;
    }
    const AIColumnSchema &definition = nullptr == replacement
        ? *column->get_ai_column_schema() : *replacement->get_ai_column_schema();
    for (int64_t j = 0; OB_SUCC(ret) && j < definition.get_dependency_count(); ++j) {
      const uint64_t id = definition.get_dependency_ids()[j];
      const ObColumnSchemaV2 *source = base.get_column_schema(id);
      for (int64_t k = 0; nullptr == source && k < changes.get_column_count(); ++k) {
        const ObColumnSchemaV2 *candidate = changes.get_column_schema_by_idx(k);
        if (candidate->get_column_id() == id) {
          source = candidate;
        }
      }
      if (nullptr == source) {
        ret = OB_NOT_SUPPORTED;
      } else {
        const AlterColumnSchema *action = action_for(*source);
        if (source->is_ai_column() || (nullptr != action
            && (action->alter_type_ == OB_DDL_DROP_COLUMN || action->is_ai_column()
                || action->get_column_name_str().case_compare(source->get_column_name_str()) != 0))) {
          ret = OB_NOT_SUPPORTED;
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < base.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = base.get_column_schema_by_idx(i);
    const AlterColumnSchema *action = action_for(*column);
    if (column->is_ai_column() && nullptr != action && action->alter_type_ == OB_DDL_DROP_COLUMN) {
      const AIColumnSchema &definition = *column->get_ai_column_schema();
      const uint64_t ids[] = {definition.get_state_column_id(), definition.get_request_column_id(),
                             definition.get_result_spec_column_id()};
      for (int64_t j = 0; OB_SUCC(ret) && j < 3; ++j) {
        const ObColumnSchemaV2 *hidden = base.get_column_schema(ids[j]);
        const AlterColumnSchema *existing = nullptr == hidden ? nullptr : action_for(*hidden);
        AlterColumnSchema drop;
        if (nullptr == hidden) {
          ret = OB_SCHEMA_ERROR;
        } else if (nullptr != existing && existing->alter_type_ == OB_DDL_DROP_COLUMN) {
        } else if (OB_FAIL(drop.assign(*hidden))) {
        } else if (OB_FAIL(drop.set_origin_column_name(hidden->get_column_name_str()))) {
        } else {
          drop.alter_type_ = OB_DDL_DROP_COLUMN;
          ret = changes.add_alter_column(drop, true);
        }
      }
    }
  }
  if (ret == OB_NOT_SUPPORTED) {
    LOG_WARN("altering an AI column input dependency", K(ret));
  }
  return ret;
}

int AIColumnResolver::append_alter_column(const ObTableSchema &base, AlterTableSchema &changes,
    AlterColumnSchema &column, ObSQLSessionInfo &session, ObSchemaChecker *checker)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *original = base.get_column_schema(column.get_origin_column_name());
  if (!column.is_ai_column() && (nullptr == original || !original->is_ai_column())) {
    ret = changes.add_alter_column(column, true);
  } else {
    ObTableSchema working;
    if (OB_FAIL(prepare_alter_column(base, changes, column, working))) {
    } else if (OB_FAIL(resolve_definition(column, working, session, checker))) {
    } else if (OB_FAIL(changes.add_alter_column(column, true))) {
    } else {
      const AIColumnSchema &definition = *column.get_ai_column_schema();
      const uint64_t ids[] = {definition.get_state_column_id(), definition.get_request_column_id(),
                             definition.get_result_spec_column_id()};
      for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
        if (nullptr == base.get_column_schema(ids[i])) {
          const ObColumnSchemaV2 *hidden = working.get_column_schema(ids[i]);
          AlterColumnSchema action;
          if (nullptr == hidden) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(action.assign(*hidden))) {
          } else {
            action.alter_type_ = OB_DDL_ADD_COLUMN;
            ret = changes.add_alter_column(action, true);
          }
        }
      }
    }
  }
  if (ret == OB_NOT_SUPPORTED) {
    LOG_WARN("invalid asynchronous AI column alteration", K(ret));
  }
  return ret;
}

int AIColumnResolver::resolve_definition(ObColumnSchemaV2 &column, const ObTableSchema &table,
    ObSQLSessionInfo &session, ObSchemaChecker *checker)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColResolve");
  const ParseNode *node = nullptr;
  const ParseNode *args = nullptr;
  const ObAiModelSchema *model = nullptr;
  bool embedding = false;
  if (!column.is_ai_column()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (nullptr == checker || nullptr == checker->get_schema_guard()) {
    ret = OB_NOT_INIT;
  } else if (column.is_rowkey_column() || !column.is_nullable() || column.is_autoincrement()) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(ObRawExprUtils::parse_expr_node_from_str(
      column.get_ai_column_schema()->get_expression(), session.get_charsets4parser(),
      allocator, node, session.get_sql_mode()))) {
  } else if (nullptr == node) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ai_function_arguments(*node, args, embedding))) {
  } else if (OB_FAIL(checker->get_schema_guard()->get_ai_model_schema(
      ai_node_text(*args->children_[0]), model))) {
  } else if (nullptr == model) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(resolve_expression(column, table, *model, session, checker))) {
  }
  return ret;
}

int AIColumnResolver::resolve_table(ObTableSchema &table, ObSQLSessionInfo &session,
    ObSchemaChecker *checker, uint64_t &last_column_id)
{
  int ret = OB_SUCCESS;
  const int64_t column_count = table.get_column_count();
  // Validate all visible definitions first, including forward dependencies.
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (nullptr == column) {
      ret = OB_ERR_UNEXPECTED;
    } else if (column->is_ai_column()) {
      ret = resolve_definition(*column, table, session, checker);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (column->is_ai_column()) {
      ret = add_hidden_columns(table, *column, last_column_id);
    }
  }
  if (OB_SUCC(ret)) {
    table.set_max_used_column_id(last_column_id);
  } else if (ret == OB_NOT_SUPPORTED) {
    LOG_WARN("invalid asynchronous AI column definition", K(ret));
  }
  return ret;
}

int AIColumnResolver::add_hidden_columns(ObTableSchema &table, ObColumnSchemaV2 &column,
    uint64_t &last_column_id)
{
  int ret = OB_SUCCESS;
  AIColumnSchema *definition = column.get_ai_column_schema();
  const char *suffixes[] = {"state", "request", "result_spec"};
  uint64_t ids[3] = {};
  if (nullptr == definition || last_column_id < table.get_max_used_column_id()
      || last_column_id > OB_MAX_TMP_COLUMN_ID - 3) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
    ObColumnSchemaV2 hidden;
    ObSqlString name;
    ObObj value;
    hidden.set_table_id(table.get_table_id());
    hidden.set_column_id(last_column_id + 1);
    hidden.set_schema_version(column.get_schema_version());
    hidden.set_is_hidden(true);
    hidden.set_nullable(false);
    if (i == 1) {
      hidden.set_data_type(ObVarcharType);
      hidden.set_data_length(64);
      hidden.set_charset_type(CHARSET_UTF8MB4);
      hidden.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      value.set_varchar(ObString(""));
      value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else {
      hidden.set_data_type(ObIntType);
      // Existing rows added to an AI column must remain PENDING, never READY.
      value.set_int(i == 0 ? 1 : column.get_schema_version());
    }
    if (OB_FAIL(name.append_fmt("__ai_%lu_%s", column.get_column_id(), suffixes[i]))) {
    } else if (OB_FAIL(hidden.set_column_name(name.string()))) {
    } else if (nullptr != table.get_column_schema(name.string())) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(hidden.set_orig_default_value(value))) {
    } else if (OB_FAIL(hidden.set_cur_default_value(value, false))) {
    } else if (OB_FAIL(table.add_column(hidden))) {
    } else {
      ids[i] = ++last_column_id;
    }
  }
  if (OB_SUCC(ret)) {
    definition->set_state_column_id(ids[0]);
    definition->set_request_column_id(ids[1]);
    definition->set_result_spec_column_id(ids[2]);
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
