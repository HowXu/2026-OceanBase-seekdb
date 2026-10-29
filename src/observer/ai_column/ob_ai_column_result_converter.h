/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace observer
{
enum class AIColumnTaskAction
{
  CALL_MODEL,
  MARK_FAILED
};

struct AIColumnTaskDecision
{
  explicit AIColumnTaskDecision(int64_t max_retries) : max_retries_(max_retries) {}
  AIColumnTaskAction after_failure(int64_t retry_count) const
  {
    return retry_count < max_retries_ ? AIColumnTaskAction::CALL_MODEL
                                     : AIColumnTaskAction::MARK_FAILED;
  }
  int64_t max_retries_;
};

class AIColumnResultConverter
{
public:
  static int append_identifier(common::ObSqlString &sql, const common::ObString &name);
  static int append_literal(common::ObSqlString &sql, const common::ObString &text,
      common::ObIAllocator &allocator);
  static int append_value(common::ObSqlString &sql, const share::schema::ObColumnSchemaV2 &column,
      const common::ObString &result, common::ObIAllocator &allocator);
};
} // namespace observer
} // namespace oceanbase
