/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "share/schema/ob_table_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace share { namespace schema {
class ObAiModelSchema;
struct AlterColumnSchema;
struct AlterTableSchema;
} }
namespace sql
{
class ObSQLSessionInfo;
class ObSchemaChecker;

class AIColumnResolver
{
public:
  static int resolve_column_attribute(const ParseNode &node, share::schema::ObColumnSchemaV2 &column);
  static int resolve_expression(share::schema::ObColumnSchemaV2 &column,
      const share::schema::ObTableSchema &table, const share::schema::ObAiModelSchema &model,
      ObSQLSessionInfo &session, const ObSchemaChecker *checker = nullptr);
  static int prepare_alter_column(const share::schema::ObTableSchema &base,
      share::schema::AlterTableSchema &changes, share::schema::AlterColumnSchema &column,
      share::schema::ObTableSchema &working);
  static int finish_alter_table(const share::schema::ObTableSchema &base,
      share::schema::AlterTableSchema &changes);
  static int append_alter_column(const share::schema::ObTableSchema &base,
      share::schema::AlterTableSchema &changes, share::schema::AlterColumnSchema &column,
      ObSQLSessionInfo &session, ObSchemaChecker *checker);
  static int resolve_definition(share::schema::ObColumnSchemaV2 &column,
      const share::schema::ObTableSchema &table, ObSQLSessionInfo &session, ObSchemaChecker *checker);
  static int resolve_table(share::schema::ObTableSchema &table, ObSQLSessionInfo &session,
      ObSchemaChecker *checker, uint64_t &last_column_id);
  static int add_hidden_columns(share::schema::ObTableSchema &table,
      share::schema::ObColumnSchemaV2 &column, uint64_t &last_column_id);
};
} // namespace sql
} // namespace oceanbase
