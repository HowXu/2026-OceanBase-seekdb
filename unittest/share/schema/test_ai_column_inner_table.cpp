/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/inner_table/ob_inner_table_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
namespace
{
using namespace common;

TEST(AIColumnInnerTableTest, DefinitionKeepsExpressionAndHiddenColumnIdentities)
{
  ObTableSchema table;
  ASSERT_EQ(OB_SUCCESS, ObInnerTableSchema::all_ai_column_schema(table));
  EXPECT_EQ(OB_ALL_AI_COLUMN_TID, table.get_table_id());
  EXPECT_EQ(2, table.get_rowkey_column_num());
  for (const char *name : {"column_name", "expression", "normalized_expr", "schema_version",
      "max_retries", "dependency_ids", "state_column_id", "request_column_id", "result_spec_column_id"}) {
    EXPECT_NE(nullptr, table.get_column_schema(ObString(name))) << name;
  }
  EXPECT_EQ(nullptr, table.get_column_schema(ObString("access_key")));
}

TEST(AIColumnInnerTableTest, SharedRowRequestHasASeparateTaskForEachColumn)
{
  ObTableSchema table;
  ASSERT_EQ(OB_SUCCESS, ObInnerTableSchema::all_ai_column_task_schema(table));
  EXPECT_EQ(3, table.get_rowkey_column_num());
  int64_t position = 1;
  for (const char *name : {"table_id", "column_id", "request_id"}) {
    const ObColumnSchemaV2 *column = table.get_column_schema(ObString(name));
    ASSERT_NE(nullptr, column) << name;
    EXPECT_EQ(position++, column->get_rowkey_position()) << name;
  }
}

TEST(AIColumnInnerTableTest, TaskDiagnosticsAndRetryFieldsMatchTheSQLContract)
{
  ObTableSchema table;
  ASSERT_EQ(OB_SUCCESS, ObInnerTableSchema::all_ai_column_task_schema(table));
  for (const char *name : {"rowkey_predicate", "pk_blob", "input_payload", "result_spec"}) {
    const ObColumnSchemaV2 *column = table.get_column_schema(ObString(name));
    ASSERT_NE(nullptr, column) << name;
    EXPECT_EQ(ObLongTextType, column->get_data_type()) << name;
  }
  const ObColumnSchemaV2 *eligible = table.get_column_schema(ObString("eligible_ts"));
  ASSERT_NE(nullptr, eligible);
  EXPECT_EQ(ObTimestampType, eligible->get_data_type());
  EXPECT_EQ(6, eligible->get_data_scale());
  const ObColumnSchemaV2 *retries = table.get_column_schema(ObString("retry_count"));
  ASSERT_NE(nullptr, retries);
  EXPECT_EQ(0, retries->get_cur_default_value().get_int());
  EXPECT_NE(nullptr, table.get_column_schema(ObString("lease_version")));
}

TEST(AIColumnInnerTableTest, CurrentAndHistoricalColumnsBothPersistTheDefinition)
{
  ObTableSchema current;
  ObTableSchema history;
  ASSERT_EQ(OB_SUCCESS, ObInnerTableSchema::all_column_schema(current));
  ASSERT_EQ(OB_SUCCESS, ObInnerTableSchema::all_column_history_schema(history));
  for (const ObTableSchema *table : {&current, &history}) {
    const ObColumnSchemaV2 *column = table->get_column_schema(ObString("ai_column_info"));
    ASSERT_NE(nullptr, column);
    EXPECT_EQ(ObLongTextType, column->get_data_type());
    EXPECT_TRUE(column->is_nullable());
  }
}
} // namespace
} // namespace schema
} // namespace share
} // namespace oceanbase
