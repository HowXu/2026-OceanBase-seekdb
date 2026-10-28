/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include "sql/engine/basic/file_sql_memory.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/executor/ob_memory_tracker.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/worker.h"

namespace oceanbase { namespace sql {

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
