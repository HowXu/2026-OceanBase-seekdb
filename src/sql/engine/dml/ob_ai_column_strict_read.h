/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/container/ob_iarray.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase
{
namespace sql
{
class ObDMLStmt;
class ObExecContext;
class ObPhysicalPlan;

struct AIColumnRef
{
  AIColumnRef() : table_id_(0), column_id_(0) {}
  AIColumnRef(uint64_t table_id, uint64_t column_id) : table_id_(table_id), column_id_(column_id) {}
  bool operator==(const AIColumnRef &other) const
  { return table_id_ == other.table_id_ && column_id_ == other.column_id_; }
  uint64_t table_id_;
  uint64_t column_id_;
  TO_STRING_KV(K_(table_id), K_(column_id));
};

class AIColumnStrictRead
{
public:
  static int collect_referenced(const ObDMLStmt &stmt, share::schema::ObSchemaGetterGuard &guard,
      common::ObIArray<AIColumnRef> &refs);
  static int check(ObExecContext &exec_ctx, const ObPhysicalPlan &plan);
};
} // namespace sql
} // namespace oceanbase
