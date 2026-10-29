/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_strict_read.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ob_server_struct.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/ob_sql_utils.h"
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/expr/ob_raw_expr.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using common::sqlclient::ObMySQLResult;
using namespace share::schema;

namespace
{
int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name.ptr()[i] == '`' ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

bool contains_ref(const ObIArray<AIColumnRef> &refs, const AIColumnRef &ref)
{
  bool found = false;
  for (int64_t i = 0; !found && i < refs.count(); ++i) { found = refs.at(i) == ref; }
  return found;
}
} // namespace

int AIColumnStrictRead::collect_referenced(const ObDMLStmt &stmt, ObSchemaGetterGuard &guard,
    ObIArray<AIColumnRef> &refs)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObColumnRefRawExpr *, 16> columns;
  ObSEArray<ObSelectStmt *, 4> children;
  if (OB_FAIL(stmt.get_column_exprs(columns))) {
  } else if (OB_FAIL(stmt.get_subquery_stmts(children))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObColumnRefRawExpr *expr = columns.at(i);
    const ObTableSchema *table = nullptr;
    const ObColumnSchemaV2 *column = nullptr;
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(guard.get_table_schema(expr->get_table_id(), table))) {
    } else if (OB_NOT_NULL(table)
        && OB_NOT_NULL(column = table->get_column_schema(expr->get_column_id()))
        && column->is_ai_column()) {
      const AIColumnRef ref(table->get_table_id(), column->get_column_id());
      if (!contains_ref(refs, ref)) { ret = refs.push_back(ref); }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < children.count(); ++i) {
    if (OB_ISNULL(children.at(i))) { ret = OB_ERR_UNEXPECTED; }
    else { ret = collect_referenced(*children.at(i), guard, refs); }
  }
  return ret;
}

int AIColumnStrictRead::check(ObExecContext &exec_ctx, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  bool strict = false;
  ObSQLSessionInfo *session = exec_ctx.get_my_session();
  ObSqlCtx *sql_ctx = exec_ctx.get_sql_ctx();
  if (OB_ISNULL(session) || OB_ISNULL(sql_ctx) || OB_ISNULL(sql_ctx->schema_guard_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict))
      || !strict || OB_ISNULL(GCTX.sql_proxy_)) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < plan.get_ai_column_refs().count(); ++i) {
      const AIColumnRef &ref = plan.get_ai_column_refs().at(i);
      const ObTableSchema *table = nullptr;
      const ObDatabaseSchema *database = nullptr;
      const ObColumnSchemaV2 *column = nullptr;
      if (OB_FAIL(sql_ctx->schema_guard_->get_table_schema(ref.table_id_, table)) || nullptr == table) {
        ret = OB_SCHEMA_ERROR;
      } else if (OB_FAIL(sql_ctx->schema_guard_->get_database_schema(table->get_database_id(), database))
          || nullptr == database) {
        ret = OB_SCHEMA_ERROR;
      } else if (OB_ISNULL(column = table->get_column_schema(ref.column_id_))
          || !column->is_ai_column() || OB_ISNULL(column->get_ai_column_schema())) {
        ret = OB_SCHEMA_ERROR;
      } else {
        const AIColumnSchema &definition = *column->get_ai_column_schema();
        const ObColumnSchemaV2 *state = table->get_column_schema(definition.get_state_column_id());
        ObSqlString sql;
        if (OB_ISNULL(state)) {
          ret = OB_SCHEMA_ERROR;
        } else if (OB_FAIL(sql.append("SELECT 1 FROM "))
            || OB_FAIL(append_identifier(sql, database->get_database_name_str()))
            || OB_FAIL(sql.append("."))
            || OB_FAIL(append_identifier(sql, table->get_table_name_str()))
            || OB_FAIL(sql.append(" WHERE "))
            || OB_FAIL(append_identifier(sql, state->get_column_name_str()))
            || OB_FAIL(sql.append(" != 0 LIMIT 1"))) {
        } else {
          const bool old_worker_flag = ObSQLUtils::is_ai_column_worker_thread();
          ObSQLUtils::set_ai_column_worker_thread(true);
          SMART_VAR(ObMySQLProxy::MySQLResult, res) {
            ObMySQLResult *result = nullptr;
            if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
            } else if (OB_ISNULL(result = res.get_result())) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(result->next())) {
              if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
            } else {
              ret = OB_AI_COLUMN_NOT_READY;
              LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, ref.table_id_, ref.column_id_);
            }
          }
          ObSQLUtils::set_ai_column_worker_thread(old_worker_flag);
        }
      }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
