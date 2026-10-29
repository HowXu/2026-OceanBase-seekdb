/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_dml_service.h"
#include "common/json_type/ob_json_tree.h"
#include "common/rowkey/ob_rowkey.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/ob_sql_context.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;
using share::OB_ALL_AI_COLUMN_TASK_TNAME;
namespace
{
int ai_append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') {
      ret = sql.append("``");
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int ai_append_object(ObSqlString &sql, const ObObj &value, const ObObjPrintParams &params,
    ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  char *buffer = nullptr;
  int64_t length = 0;
  int64_t pos = 0;
  if (ob_is_string_type(value.get_type())) {
    // Preserve bytes and collation, independently of NO_BACKSLASH_ESCAPES.
    length = value.get_string_len() * 2L + 256;
    buffer = static_cast<char *>(allocator.alloc(length));
    if (nullptr == buffer) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = ObObjCharacterUtil::print_safe_hex_represent_mysql(value, buffer, length, pos); }
  } else {
    ret = value.print_sql_literal(buffer, length, pos, allocator, params);
  }
  if (OB_SUCC(ret)) { ret = sql.append(buffer, pos); }
  return ret;
}

int ai_append_literal(ObSqlString &sql, const ObString &text, ObIAllocator &allocator)
{
  ObObj value;
  value.set_varchar(text);
  value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  return ai_append_object(sql, value, ObObjPrintParams(), allocator);
}

int ai_save_statement(const ObSqlString &sql, ObIAllocator &allocator, ObIArray<ObString> &statements)
{
  int ret = OB_SUCCESS;
  ObString saved;
  if (OB_FAIL(ob_write_string(allocator, sql.string(), saved, true))) {
  } else { ret = statements.push_back(saved); }
  return ret;
}

int ai_append_key_filter(ObSqlString &sql, const uint64_t table_id,
    const AIColumnRowkey &key, ObIAllocator &allocator)
{
  int ret = sql.append_fmt("table_id = %lu AND pk_hash = '%016lx' AND pk_blob = ", table_id, key.hash_);
  if (OB_SUCC(ret)) { ret = ai_append_literal(sql, key.hex_.string(), allocator); }
  return ret;
}

int ai_json_string(ObIJsonBase &root, const char *name, ObString &text)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *value = nullptr;
  if (OB_FAIL(root.get_object_value(ObString(name), value))) {
  } else if (nullptr == value || value->json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    text.assign_ptr(value->get_data(), value->get_data_length());
  }
  return ret;
}
} // namespace

int AIColumnDmlService::collect_affected_columns(const ObTableSchema &table,
    const ObIArray<uint64_t> &changed, bool rowkey_changed, ObIArray<uint64_t> &affected)
{
  int ret = OB_SUCCESS;
  affected.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < table.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (column->is_ai_column() && !column->is_unused()) {
      const AIColumnSchema &definition = *column->get_ai_column_schema();
      bool hit = rowkey_changed;
      for (int64_t j = 0; !hit && j < definition.get_dependency_count(); ++j) {
        for (int64_t k = 0; !hit && k < changed.count(); ++k) {
          hit = definition.get_dependency_ids()[j] == changed.at(k);
        }
      }
      if (hit) { ret = affected.push_back(column->get_column_id()); }
    }
  }
  return ret;
}

int AIColumnDmlService::build_rowkey(const ObTableSchema &table, const ObIArray<uint64_t> &column_ids,
    const ObIArray<ObObj> &values, const ObObjPrintParams &params, ObIAllocator &allocator,
    AIColumnRowkey &key)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 8> objects;
  key.predicate_.reset();
  key.hex_.reset();
  const ObRowkeyInfo &info = table.get_rowkey_info();
  if (column_ids.count() != values.count() || info.get_size() <= 0) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < info.get_size(); ++i) {
    const uint64_t id = info.get_column(i)->column_id_;
    const ObColumnSchemaV2 *column = table.get_column_schema(id);
    int64_t index = -1;
    for (int64_t j = 0; index < 0 && j < column_ids.count(); ++j) {
      if (column_ids.at(j) == id) { index = j; }
    }
    if (nullptr == column || index < 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (values.at(index).is_null()) {
      ret = OB_ERR_NULL_VALUE;
    } else if (i > 0 && OB_FAIL(key.predicate_.append(" AND "))) {
    } else if (OB_FAIL(ai_append_identifier(key.predicate_, column->get_column_name_str()))) {
    } else if (OB_FAIL(key.predicate_.append(" = "))) {
    } else if (OB_FAIL(ai_append_object(key.predicate_, values.at(index), params, allocator))) {
    } else { ret = objects.push_back(values.at(index)); }
  }
  if (OB_SUCC(ret)) {
    ObRowkey rowkey(objects.get_data(), objects.count());
    const int64_t size = rowkey.get_serialize_size();
    char *bytes = static_cast<char *>(allocator.alloc(size));
    int64_t pos = 0;
    if (nullptr == bytes) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(rowkey.serialize(bytes, size, pos))) {
    } else {
      key.hash_ = rowkey.hash();
      for (int64_t i = 0; OB_SUCC(ret) && i < pos; ++i) {
        ret = key.hex_.append_fmt("%02x", static_cast<unsigned char>(bytes[i]));
      }
    }
  }
  return ret;
}

int AIColumnDmlService::decode_rowkey_hex(const ObString &hex, ObIAllocator &allocator, ObString &bytes)
{
  int ret = OB_SUCCESS;
  bytes.reset();
  if (hex.empty() || hex.length() % 2 != 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const int64_t size = hex.length() / 2;
    char *buffer = static_cast<char *>(allocator.alloc(size));
    if (nullptr == buffer) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      auto digit = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      };
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        const int high = digit(hex.ptr()[i * 2]);
        const int low = digit(hex.ptr()[i * 2 + 1]);
        if (high < 0 || low < 0) { ret = OB_INVALID_ARGUMENT; }
        else { buffer[i] = static_cast<char>((high << 4) | low); }
      }
      if (OB_SUCC(ret)) { bytes.assign_ptr(buffer, size); }
    }
  }
  return ret;
}

int AIColumnDmlService::build_mutations(const ObTableSchema &table, const ObString &database,
    const AIColumnRowkey *old_key, const AIColumnRowkey *new_key, const ObIArray<uint64_t> &affected,
    const ObString &request_id, ObIAllocator &allocator, ObIArray<ObString> &statements)
{
  int ret = OB_SUCCESS;
  if (nullptr == new_key || affected.count() > 0) {
    if (nullptr == old_key || old_key->hex_.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt("DELETE FROM %s.%s WHERE ", OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME))) {
      } else if (OB_FAIL(ai_append_key_filter(sql, table.get_table_id(), *old_key, allocator))) {
      } else if (nullptr != new_key) {
        if (OB_FAIL(sql.append(" AND column_id IN ("))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
          ret = sql.append_fmt("%s%lu", i == 0 ? "" : ",", affected.at(i));
        }
        if (OB_SUCC(ret)) { ret = sql.append(")"); }
      }
      if (OB_SUCC(ret)) { ret = ai_save_statement(sql, allocator, statements); }
    }
  }
  ObSqlString target;
  if (OB_FAIL(ret) || nullptr == new_key || affected.empty()) {
  } else if (request_id.empty() || request_id.length() > 64 || new_key->predicate_.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_append_identifier(target, database))) {
  } else if (OB_FAIL(target.append("."))) {
  } else if (OB_FAIL(ai_append_identifier(target, table.get_table_name_str()))) {
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt("UPDATE %s SET ", target.ptr()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < affected.count(); ++i) {
      const ObColumnSchemaV2 *column = table.get_column_schema(affected.at(i));
      if (nullptr == column || !column->is_ai_column()) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const AIColumnSchema &definition = *column->get_ai_column_schema();
        const uint64_t ids[] = {column->get_column_id(), definition.get_state_column_id(),
            definition.get_request_column_id(), definition.get_result_spec_column_id()};
        for (int64_t j = 0; OB_SUCC(ret) && j < 4; ++j) {
          const ObColumnSchemaV2 *member = table.get_column_schema(ids[j]);
          if (nullptr == member) { ret = OB_SCHEMA_ERROR; }
          else if ((i > 0 || j > 0) && OB_FAIL(sql.append(", "))) {
          } else if (OB_FAIL(ai_append_identifier(sql, member->get_column_name_str()))) {
          } else if (OB_FAIL(sql.append(" = "))) {
          } else if (j == 0) { ret = sql.append("NULL"); }
          else if (j == 1) { ret = sql.append("1"); }
          else if (j == 2) { ret = ai_append_literal(sql, request_id, allocator); }
          else { ret = sql.append_fmt("%ld", definition.get_schema_version()); }
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append_fmt(" WHERE %s", new_key->predicate_.ptr()))) {
    } else { ret = ai_save_statement(sql, allocator, statements); }
  }
  for (int64_t i = 0; OB_SUCC(ret) && nullptr != new_key && i < affected.count(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(affected.at(i));
    const AIColumnSchema &definition = *column->get_ai_column_schema();
    ObIJsonBase *root = nullptr;
    ObString input, config;
    ObSqlString sql;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, definition.get_normalized_expr(),
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root))) {
    } else if (nullptr == root) { ret = OB_INVALID_ARGUMENT; }
    else if (OB_FAIL(ai_json_string(*root, "input_expr", input))) {
    } else if (OB_FAIL(ai_json_string(*root, "config_expr", config))) {
    } else if (OB_FAIL(sql.append_fmt("INSERT INTO %s.%s (table_id,column_id,request_id,schema_version,"
        "pk_hash,pk_blob,rowkey_predicate,state,retry_count,eligible_ts,input_payload,result_spec) SELECT %lu,%lu,",
        OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME, table.get_table_id(), column->get_column_id()))) {
    } else if (OB_FAIL(ai_append_literal(sql, request_id, allocator))) {
    } else if (OB_FAIL(sql.append_fmt(",%ld,'%016lx',", definition.get_schema_version(), new_key->hash_))) {
    } else if (OB_FAIL(ai_append_literal(sql, new_key->hex_.string(), allocator))) {
    } else if (OB_FAIL(sql.append(","))) {
    } else if (OB_FAIL(ai_append_literal(sql, new_key->predicate_.string(), allocator))) {
    } else if (OB_FAIL(sql.append_fmt(",0,0,NOW(6),JSON_OBJECT('input',(%.*s)),JSON_SET(CAST(", input.length(), input.ptr()))) {
    } else if (OB_FAIL(ai_append_literal(sql, definition.get_normalized_expr(), allocator))) {
    } else if (OB_FAIL(sql.append_fmt(" AS JSON),'$.config',CAST((%.*s) AS JSON)) FROM %s WHERE %s",
        config.length(), config.ptr(), target.ptr(), new_key->predicate_.ptr()))) {
    } else if (OB_FAIL(sql.append(" AND "))) {
    } else if (OB_FAIL(ai_append_identifier(sql,
        table.get_column_schema(definition.get_request_column_id())->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (OB_FAIL(ai_append_literal(sql, request_id, allocator))) {
    } else if (OB_FAIL(sql.append(" AND "))) {
    } else if (OB_FAIL(ai_append_identifier(sql,
        table.get_column_schema(definition.get_result_spec_column_id())->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append_fmt(" = %ld", definition.get_schema_version()))) {
    } else { ret = ai_save_statement(sql, allocator, statements); }
  }
  return ret;
}

int AIColumnDmlService::build_retry_statements(const ObTableSchema &table,
    const ObString &database_name, const uint64_t column_id, const ObString &predicate,
    ObIAllocator &allocator, ObIArray<ObString> &statements)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = table.get_column_schema(column_id);
  const AIColumnSchema *definition = nullptr;
  ObSqlString target;
  if (OB_ISNULL(column) || !column->is_ai_column()
      || OB_ISNULL(definition = column->get_ai_column_schema()) || predicate.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_append_identifier(target, database_name))
      || OB_FAIL(target.append("."))
      || OB_FAIL(ai_append_identifier(target, table.get_table_name_str()))) {
  } else {
    const ObColumnSchemaV2 *state = table.get_column_schema(definition->get_state_column_id());
    const ObColumnSchemaV2 *request = table.get_column_schema(definition->get_request_column_id());
    const ObColumnSchemaV2 *result_spec = table.get_column_schema(definition->get_result_spec_column_id());
    ObIJsonBase *root = nullptr;
    ObString input, config;
    if (OB_ISNULL(state) || OB_ISNULL(request) || OB_ISNULL(result_spec)) {
      ret = OB_SCHEMA_ERROR;
    } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, definition->get_normalized_expr(),
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root)) || nullptr == root) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ai_json_string(*root, "input_expr", input))
        || OB_FAIL(ai_json_string(*root, "config_expr", config))) {
    } else {
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt("DELETE FROM %s.%s WHERE table_id=%lu AND column_id=%lu "
          "AND request_id IN (SELECT ", OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME,
          table.get_table_id(), column_id))) {
      } else if (OB_FAIL(ai_append_identifier(sql, request->get_column_name_str()))
          || OB_FAIL(sql.append_fmt(" FROM %s WHERE %s)", target.ptr(), predicate.ptr()))
          || OB_FAIL(ai_save_statement(sql, allocator, statements))) {
      } else if (OB_FAIL(sql.assign_fmt("UPDATE %s SET ", target.ptr()))
          || OB_FAIL(ai_append_identifier(sql, column->get_column_name_str()))
          || OB_FAIL(sql.append(" = NULL,"))
          || OB_FAIL(ai_append_identifier(sql, state->get_column_name_str()))
          || OB_FAIL(sql.append(" = 1,"))
          || OB_FAIL(ai_append_identifier(sql, request->get_column_name_str()))
          || OB_FAIL(sql.append(" = UUID(),"))
          || OB_FAIL(ai_append_identifier(sql, result_spec->get_column_name_str()))
          || OB_FAIL(sql.append_fmt(" = %ld WHERE %s", definition->get_schema_version(), predicate.ptr()))
          || OB_FAIL(ai_save_statement(sql, allocator, statements))) {
      } else if (OB_FAIL(sql.assign_fmt("INSERT INTO %s.%s (table_id,column_id,request_id,schema_version,"
          "pk_hash,pk_blob,rowkey_predicate,state,retry_count,eligible_ts,input_payload,result_spec) "
          "SELECT %lu,%lu,", OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME,
          table.get_table_id(), column_id))
          || OB_FAIL(ai_append_identifier(sql, request->get_column_name_str()))
          || OB_FAIL(sql.append_fmt(",%ld,'','','1=1',0,0,DATE_ADD(NOW(6),INTERVAL 2 SECOND),JSON_OBJECT('input',(%.*s)),"
              "JSON_SET(CAST(", definition->get_schema_version(), input.length(), input.ptr()))
          || OB_FAIL(ai_append_literal(sql, definition->get_normalized_expr(), allocator))
          || OB_FAIL(sql.append_fmt(" AS JSON),'$.config',CAST((%.*s) AS JSON)) FROM %s WHERE %s AND ",
              config.length(), config.ptr(), target.ptr(), predicate.ptr()))
          || OB_FAIL(ai_append_identifier(sql, request->get_column_name_str()))
          || OB_FAIL(sql.append(" IS NOT NULL AND "))
          || OB_FAIL(ai_append_identifier(sql, result_spec->get_column_name_str()))
          || OB_FAIL(sql.append_fmt(" = %ld", definition->get_schema_version()))
          || OB_FAIL(ai_save_statement(sql, allocator, statements))) {
      }
    }
  }
  return ret;
}

int AIColumnDmlService::maintain(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event)
{
  int ret = OB_SUCCESS;
  if (!ctdef.has_ai_columns_ || !ctdef.is_primary_index_) { return ret; }
  ObExecContext &ctx = op.get_exec_ctx();
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  ObArenaAllocator allocator("AIColDml");
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  ObSchemaGetterGuard *guard = nullptr;
  ObSEArray<uint64_t, 16> ids, changed, affected;
  ObSEArray<ObObj, 16> old_values, new_values;
  AIColumnRowkey old_key, new_key;
  ObSEArray<ObString, 4> statements;
  const bool inserting = event == ObDmlEventType::DE_INSERTING;
  const bool deleting = event == ObDmlEventType::DE_DELETING;
  const bool updating = event == ObDmlEventType::DE_UPDATING;
  bool key_changed = inserting;
  if (!inserting && !deleting && !updating) {
    ret = OB_INVALID_ARGUMENT;
  } else if (nullptr == ctx.get_sql_ctx() || nullptr == (guard = ctx.get_sql_ctx()->schema_guard_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(guard->get_table_schema(ctdef.das_base_ctdef_.index_tid_, table))) {
  } else if (nullptr == table) {
    ret = OB_SCHEMA_ERROR;
  } else if (OB_FAIL(guard->get_database_schema(table->get_database_id(), database))) {
  } else if (nullptr == database) {
    ret = OB_SCHEMA_ERROR;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ctdef.column_ids_.count(); ++i) {
    ObDatum *old_datum = nullptr;
    ObDatum *new_datum = nullptr;
    ObObj old_value, new_value;
    const uint64_t id = ctdef.column_ids_.at(i);
    if (OB_FAIL(ids.push_back(id))) {
    } else if (!inserting && (i >= ctdef.old_row_.count() || nullptr == ctdef.old_row_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!deleting && (i >= ctdef.new_row_.count() || nullptr == ctdef.new_row_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!inserting && OB_FAIL(ctdef.old_row_.at(i)->eval(eval_ctx, old_datum))) {
    } else if (!deleting && OB_FAIL(ctdef.new_row_.at(i)->eval(eval_ctx, new_datum))) {
    } else if (!inserting && OB_FAIL(old_datum->to_obj(old_value, ctdef.old_row_.at(i)->obj_meta_))) {
    } else if (!deleting && OB_FAIL(new_datum->to_obj(new_value, ctdef.new_row_.at(i)->obj_meta_))) {
    } else if (!inserting && OB_FAIL(old_values.push_back(old_value))) {
    } else if (!deleting && OB_FAIL(new_values.push_back(new_value))) {
    } else if (updating) {
      bool different = !ObDatum::binary_equal(*old_datum, *new_datum);
      if (different && !old_datum->is_null() && !new_datum->is_null()
          && ob_is_text_tc(old_value.get_type())) {
        int64_t comparison = 0;
        if (OB_FAIL(ObDMLService::check_lob_column_changed(eval_ctx, *ctdef.old_row_.at(i),
            *old_datum, *ctdef.new_row_.at(i), *new_datum, comparison))) {
        } else { different = comparison != 0; }
      }
      if (OB_SUCC(ret) && different) {
        const ObColumnSchemaV2 *column = table->get_column_schema(id);
        if (nullptr == column) { ret = OB_SCHEMA_ERROR; }
        else if (OB_FAIL(changed.push_back(id))) {
        } else if (column->is_rowkey_column()) { key_changed = true; }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!deleting && OB_FAIL(collect_affected_columns(*table, changed, key_changed, affected))) {
  } else if (!deleting && affected.empty()) {
    // Also prevents recursion for native NULL/state writes and later worker publication:
    // AI results and their hidden columns cannot be input dependencies.
    return OB_SUCCESS;
  } else if (!inserting && OB_FAIL(build_rowkey(*table, ids, old_values,
      op.get_obj_print_params(), allocator, old_key))) {
  } else if (!deleting && OB_FAIL(build_rowkey(*table, ids, new_values,
      op.get_obj_print_params(), allocator, new_key))) {
  } else {
    char uuid[UuidCommon::LENGTH_UUID + 1] = {};
    if (!deleting && OB_FAIL(ObExprUuid::gen_server_uuid(uuid, UuidCommon::LENGTH_UUID))) {
    } else if (OB_FAIL(build_mutations(*table, database->get_database_name_str(),
        inserting ? &new_key : &old_key, deleting ? nullptr : &new_key, affected,
        ObString(deleting ? 0 : UuidCommon::LENGTH_UUID, uuid), allocator, statements))) {
    }
  }
  bool nested = false;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(op.check_stack())) {
  } else if (OB_FAIL(op.open_inner_conn())) {
  } else if (OB_FAIL(op.begin_nested_session(true))) {
  } else {
    nested = true;
    ObSQLSessionInfo *session = ctx.get_my_session();
    const ObSQLMode mode = session->get_sql_mode();
    // Normalized expressions use the native printer's canonical backslash escaping.
    session->set_sql_mode(mode & ~SMO_NO_BACKSLASH_ESCAPES);
    session->set_ai_column_internal_access(true);
    const bool old_nested_flag = ctx.get_das_ctx().is_ai_column_nested_;
    ctx.get_das_ctx().is_ai_column_nested_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < statements.count(); ++i) {
      ret = op.execute_write(statements.at(i).ptr());
    }
    ctx.get_das_ctx().is_ai_column_nested_ = old_nested_flag;
    session->set_ai_column_internal_access(false);
    session->set_sql_mode(mode);
  }
  if (nested) {
    const int end_ret = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
