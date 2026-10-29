/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_iarray.h"
#include "common/object/ob_object.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_define.h"

namespace oceanbase
{
namespace sql
{
class ObTableModifyOp;
struct ObDMLBaseCtDef;

struct AIColumnRowkey
{
  AIColumnRowkey() : hash_(0) {}
  common::ObSqlString predicate_;
  common::ObSqlString hex_;
  uint64_t hash_;
};

class AIColumnDmlService
{
public:
  static int collect_affected_columns(const share::schema::ObTableSchema &table,
      const common::ObIArray<uint64_t> &changed, bool rowkey_changed,
      common::ObIArray<uint64_t> &affected);
  static int build_rowkey(const share::schema::ObTableSchema &table,
      const common::ObIArray<uint64_t> &column_ids, const common::ObIArray<common::ObObj> &values,
      const common::ObObjPrintParams &params, common::ObIAllocator &allocator, AIColumnRowkey &key);
  static int decode_rowkey_hex(const common::ObString &hex, common::ObIAllocator &allocator,
      common::ObString &bytes);
  static int build_mutations(const share::schema::ObTableSchema &table, const common::ObString &database,
      const AIColumnRowkey *old_key, const AIColumnRowkey *new_key,
      const common::ObIArray<uint64_t> &affected, const common::ObString &request_id,
      common::ObIAllocator &allocator, common::ObIArray<common::ObString> &statements);
  static int maintain(ObTableModifyOp &op, const ObDMLBaseCtDef &ctdef, ObDmlEventType event);
  static int build_retry_statements(const share::schema::ObTableSchema &table,
      const common::ObString &database_name, uint64_t column_id, const common::ObString &predicate,
      common::ObIAllocator &allocator, common::ObIArray<common::ObString> &statements);
};
} // namespace sql
} // namespace oceanbase
