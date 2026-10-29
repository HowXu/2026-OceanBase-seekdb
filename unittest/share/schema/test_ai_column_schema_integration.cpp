/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <vector>
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
namespace
{
void fill_schema(AIColumnSchema &schema)
{
  schema.set_table_id(50001);
  schema.set_column_id(18);
  schema.set_schema_version(42);
  schema.set_state_column_id(19);
  schema.set_request_column_id(20);
  schema.set_result_spec_column_id(21);
  ASSERT_EQ(common::OB_SUCCESS, schema.set_column_name("tags"));
  ASSERT_EQ(common::OB_SUCCESS, schema.set_expression("AI_COMPLETE('m',content)"));
  ASSERT_EQ(common::OB_SUCCESS, schema.set_normalized_expr("AI_COMPLETE('m',content)"));
  ASSERT_EQ(common::OB_SUCCESS, schema.set_max_retries(2));
  common::ObSEArray<uint64_t, 2> deps;
  ASSERT_EQ(common::OB_SUCCESS, deps.push_back(17));
  ASSERT_EQ(common::OB_SUCCESS, schema.set_dependency_ids(deps));
}

TEST(AIColumnSchemaTest, ColumnAssignOwnsAIColumnDefinition)
{
  AIColumnSchema definition;
  fill_schema(definition);
  ObColumnSchemaV2 column;
  column.set_table_id(50001);
  column.set_column_id(18);
  column.set_schema_version(42);
  ASSERT_EQ(common::OB_SUCCESS, column.set_column_name("tags"));
  ASSERT_EQ(common::OB_SUCCESS, column.set_ai_column_schema(definition));
  ObColumnSchemaV2 copy;
  ASSERT_EQ(common::OB_SUCCESS, copy.assign(column));
  ASSERT_TRUE(copy.is_ai_column());
  ASSERT_NE(nullptr, copy.get_ai_column_schema());
  EXPECT_NE(column.get_ai_column_schema(), copy.get_ai_column_schema());
  column.reset();
  definition.reset();
  EXPECT_FALSE(column.is_ai_column());
  EXPECT_TRUE(copy.get_ai_column_schema()->is_valid());
  EXPECT_EQ(common::ObString("tags"), copy.get_ai_column_schema()->get_column_name());
}

TEST(AIColumnSchemaTest, ColumnIdentityAndNameRemainConsistentWithDefinition)
{
  AIColumnSchema definition;
  fill_schema(definition);
  ObColumnSchemaV2 column;
  ASSERT_EQ(common::OB_SUCCESS, column.set_ai_column_schema(definition));
  column.set_table_id(60001);
  column.set_column_id(118);
  column.set_schema_version(43);
  ASSERT_EQ(common::OB_SUCCESS, column.set_column_name("new_tags"));
  ASSERT_NE(nullptr, column.get_ai_column_schema());
  EXPECT_EQ(60001, column.get_ai_column_schema()->get_table_id());
  EXPECT_EQ(118, column.get_ai_column_schema()->get_column_id());
  EXPECT_EQ(43, column.get_ai_column_schema()->get_schema_version());
  EXPECT_EQ(common::ObString("new_tags"), column.get_ai_column_schema()->get_column_name());
}

TEST(AIColumnSchemaTest, ColumnRoundTripAndReuseDoNotLoseOrRetainAIDefinitions)
{
  AIColumnSchema definition;
  fill_schema(definition);
  ObColumnSchemaV2 source;
  source.set_data_type(common::ObJsonType);
  ASSERT_EQ(common::OB_SUCCESS, source.set_ai_column_schema(definition));
  std::vector<char> bytes(source.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, source.serialize(bytes.data(), bytes.size(), pos));
  EXPECT_EQ(bytes.size(), pos);
  ObColumnSchemaV2 decoded;
  pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, decoded.deserialize(bytes.data(), bytes.size(), pos));
  ASSERT_TRUE(decoded.is_ai_column());
  ASSERT_NE(nullptr, decoded.get_ai_column_schema());
  EXPECT_EQ(19, decoded.get_ai_column_schema()->get_state_column_id());
  source.reset();
  source.set_data_type(common::ObIntType);
  bytes.resize(source.get_serialize_size());
  pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, source.serialize(bytes.data(), bytes.size(), pos));
  pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, decoded.deserialize(bytes.data(), bytes.size(), pos));
  EXPECT_FALSE(decoded.is_ai_column());
}

TEST(AIColumnSchemaTest, ColumnIDConversionAlsoRemapsAIDefinition)
{
  AIColumnSchema definition;
  fill_schema(definition);
  ObColumnSchemaV2 column;
  column.set_column_id(18);
  ASSERT_EQ(common::OB_SUCCESS, column.set_ai_column_schema(definition));
  common::hash::ObHashMap<uint64_t, uint64_t> ids;
  ASSERT_EQ(common::OB_SUCCESS, ids.create(8, "AIColTest"));
  for (uint64_t id = 17; id <= 21; ++id) {
    ASSERT_EQ(common::OB_SUCCESS, ids.set_refactored(id, id + 100));
  }
  ASSERT_EQ(common::OB_SUCCESS, column.convert_column_id(ids));
  ASSERT_NE(nullptr, column.get_ai_column_schema());
  EXPECT_EQ(118, column.get_column_id());
  EXPECT_EQ(118, column.get_ai_column_schema()->get_column_id());
  EXPECT_EQ(119, column.get_ai_column_schema()->get_state_column_id());
  EXPECT_EQ(120, column.get_ai_column_schema()->get_request_column_id());
  EXPECT_EQ(121, column.get_ai_column_schema()->get_result_spec_column_id());
  EXPECT_EQ(117, column.get_ai_column_schema()->get_dependency_ids()[0]);
}

} // namespace
} // namespace schema
} // namespace share
} // namespace oceanbase
