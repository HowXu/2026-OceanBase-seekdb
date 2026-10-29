/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include "sql/engine/basic/file_sql_memory.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/executor/ob_memory_tracker.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/worker.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"

namespace oceanbase { namespace sql {

// Session variables are snapshots: a different connection may have revoked the
// global root. Copy under the schema guard so concurrent updates cannot leave a
// borrowed pointer to a retired schema.
inline int get_file_sql_secure_root(common::ObIAllocator &allocator, common::ObString &root)
{
  int ret = common::OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObSysVariableSchema *variables = nullptr;
  const share::schema::ObSysVarSchema *value = nullptr;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = common::OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_sys_variable_schema(variables))) {
  } else if (OB_ISNULL(variables)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(variables->get_sysvar_schema(share::SYS_VAR_SECURE_FILE_PRIV, value))) {
  } else if (OB_ISNULL(value)) {
    ret = common::OB_ERR_UNEXPECTED;
  } else {
    ret = common::ob_write_string(allocator, value->get_value(), root);
  }
  return ret;
}

class FileSqlQueryError : public file_sql::Error {
public:
  explicit FileSqlQueryError(int code) : file_sql::Error("file SQL query resource check failed"), code_(code) {}
  int code() const { return code_; }
private:
  int code_;
};

inline std::shared_ptr<file_sql::MemoryBudget> make_file_sql_query_budget(ObSQLSessionInfo &session)
{
  return std::make_shared<file_sql::MemoryBudget>(std::numeric_limits<size_t>::max(),
      std::pmr::new_delete_resource(), std::shared_ptr<file_sql::MemoryBudget>{},
      [&session](size_t bytes) {
        int ret = common::OB_SUCCESS;
        if (THIS_WORKER.is_timeout()) {
          ret = common::OB_TIMEOUT;
        } else if (session.is_terminate(ret)) {
        } else if (IS_INTERRUPTED()) {
          ret = GET_INTERRUPT_CODE().code_;
        } else {
          ret = lib::ObMemTrackerGuard::check_status(
              bytes > static_cast<size_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(bytes));
        }
        if (ret != common::OB_SUCCESS) throw FileSqlQueryError(ret);
      });
}

inline std::shared_ptr<file_sql::MemoryBudget> make_file_sql_scan_budget(ObExecContext &ctx)
{
  auto &query = ctx.get_file_sql_query_budget();
  if (!query) query = make_file_sql_query_budget(*ctx.get_my_session());
  return std::make_shared<file_sql::MemoryBudget>(file_sql::MAX_WORKSPACE_BYTES,
                                                 std::pmr::new_delete_resource(), query);
}

}} // namespace oceanbase::sql
