/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <vector>
#include "share/schema/ob_ai_column_schema.h"

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

TEST(AIColumnSchemaTest, DefaultIsInvalidAndResetClearsDefinition)
{
  AIColumnSchema schema;
  EXPECT_FALSE(schema.is_valid());
  fill_schema(schema);
  EXPECT_TRUE(schema.is_valid());
  schema.reset();
  EXPECT_FALSE(schema.is_valid());
  EXPECT_EQ(0, schema.get_dependency_count());
  EXPECT_TRUE(schema.get_normalized_expr().empty());
}

TEST(AIColumnSchemaTest, AssignDeepCopiesStringsAndDependencies)
{
  AIColumnSchema copy;
  {
    AIColumnSchema original;
    fill_schema(original);
    ASSERT_EQ(common::OB_SUCCESS, copy.assign(original));
    EXPECT_NE(original.get_expression().ptr(), copy.get_expression().ptr());
    EXPECT_NE(original.get_dependency_ids(), copy.get_dependency_ids());
    original.reset();
  }
  EXPECT_TRUE(copy.is_valid());
  EXPECT_EQ(common::ObString("AI_COMPLETE('m',content)"), copy.get_expression());
  ASSERT_EQ(1, copy.get_dependency_count());
  EXPECT_EQ(17, copy.get_dependency_ids()[0]);
  ASSERT_EQ(common::OB_SUCCESS, copy.assign(copy));
  EXPECT_TRUE(copy.is_valid());
}

TEST(AIColumnSchemaTest, SerializationOwnsDecodedMemoryAndSizeIsExact)
{
  AIColumnSchema original;
  fill_schema(original);
  AIColumnSchema decoded;
  std::vector<char> bytes(original.get_serialize_size());
  int64_t pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, original.serialize(bytes.data(), bytes.size(), pos));
  EXPECT_EQ(bytes.size(), pos);
  int64_t decoded_pos = 0;
  ASSERT_EQ(common::OB_SUCCESS, decoded.deserialize(bytes.data(), bytes.size(), decoded_pos));
  EXPECT_EQ(pos, decoded_pos);
  std::fill(bytes.begin(), bytes.end(), 'x');
  EXPECT_TRUE(decoded.is_valid());
  EXPECT_EQ(common::ObString("tags"), decoded.get_column_name());
  EXPECT_EQ(original.get_normalized_expr(), decoded.get_normalized_expr());
  EXPECT_EQ(42, decoded.get_schema_version());
  EXPECT_EQ(2, decoded.get_max_retries());
  ASSERT_EQ(1, decoded.get_dependency_count());
  EXPECT_EQ(17, decoded.get_dependency_ids()[0]);
  EXPECT_EQ(19, decoded.get_state_column_id());
  EXPECT_EQ(20, decoded.get_request_column_id());
  EXPECT_EQ(21, decoded.get_result_spec_column_id());
}

TEST(AIColumnSchemaTest, RemapIncludesResultHiddenColumnsAndDependencies)
{
  AIColumnSchema schema;
  fill_schema(schema);
  common::ObSEArray<uint64_t, 8> old_ids;
  common::ObSEArray<uint64_t, 8> new_ids;
  for (uint64_t id = 17; id <= 21; ++id) {
    ASSERT_EQ(common::OB_SUCCESS, old_ids.push_back(id));
    ASSERT_EQ(common::OB_SUCCESS, new_ids.push_back(id + 100));
  }
  ASSERT_EQ(common::OB_SUCCESS, schema.remap_column_ids(old_ids, new_ids));
  EXPECT_EQ(118, schema.get_column_id());
  EXPECT_EQ(119, schema.get_state_column_id());
  EXPECT_EQ(120, schema.get_request_column_id());
  EXPECT_EQ(121, schema.get_result_spec_column_id());
  EXPECT_EQ(117, schema.get_dependency_ids()[0]);
  EXPECT_EQ(50001, schema.get_table_id());
}

TEST(AIColumnSchemaTest, IncompleteRemapDoesNotPartiallyModifySchema)
{
  AIColumnSchema schema;
  fill_schema(schema);
  common::ObSEArray<uint64_t, 1> old_ids;
  common::ObSEArray<uint64_t, 1> new_ids;
  ASSERT_EQ(common::OB_SUCCESS, old_ids.push_back(18));
  ASSERT_EQ(common::OB_SUCCESS, new_ids.push_back(118));
  EXPECT_EQ(common::OB_ENTRY_NOT_EXIST, schema.remap_column_ids(old_ids, new_ids));
  EXPECT_EQ(18, schema.get_column_id());
  EXPECT_EQ(19, schema.get_state_column_id());
  EXPECT_EQ(17, schema.get_dependency_ids()[0]);
}

TEST(AIColumnSchemaTest, SQLMetadataRoundTripOwnsHexDecodedBytes)
{
  AIColumnSchema original;
  fill_schema(original);
  const char expression[] = {'a', '\0', static_cast<char>(0xff), 'b'};
  ASSERT_EQ(common::OB_SUCCESS,
      original.set_expression(common::ObString(sizeof(expression), expression)));
  common::ObArenaAllocator allocator;
  common::ObString encoded;
  ASSERT_EQ(common::OB_SUCCESS, original.serialize_to_hex(allocator, encoded));
  EXPECT_EQ(original.get_serialize_size() * 2, encoded.length());
  for (int64_t i = 0; i < encoded.length(); ++i) {
    EXPECT_TRUE((encoded[i] >= '0' && encoded[i] <= '9')
        || (encoded[i] >= 'A' && encoded[i] <= 'F'));
  }
  AIColumnSchema decoded;
  ASSERT_EQ(common::OB_SUCCESS, decoded.deserialize_from_hex(encoded));
  MEMSET(encoded.ptr(), '0', encoded.length());
  EXPECT_TRUE(decoded.is_valid());
  EXPECT_EQ(original.get_expression(), decoded.get_expression());
  EXPECT_EQ(original.get_normalized_expr(), decoded.get_normalized_expr());
  EXPECT_EQ(17, decoded.get_dependency_ids()[0]);
  EXPECT_EQ(20, decoded.get_request_column_id());
}

TEST(AIColumnSchemaTest, MalformedSQLMetadataDoesNotReplaceDefinition)
{
  AIColumnSchema schema;
  fill_schema(schema);
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, schema.deserialize_from_hex(""));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, schema.deserialize_from_hex("ABC"));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, schema.deserialize_from_hex("GG"));
  EXPECT_NE(common::OB_SUCCESS, schema.deserialize_from_hex("00"));
  EXPECT_TRUE(schema.is_valid());
  EXPECT_EQ(18, schema.get_column_id());
  EXPECT_EQ(common::ObString("tags"), schema.get_column_name());
}

TEST(AIColumnSchemaTest, SQLMetadataRejectsTrailingBytes)
{
  AIColumnSchema schema;
  fill_schema(schema);
  common::ObArenaAllocator allocator;
  common::ObString encoded;
  ASSERT_EQ(common::OB_SUCCESS, schema.serialize_to_hex(allocator, encoded));
  std::vector<char> trailing(encoded.ptr(), encoded.ptr() + encoded.length());
  trailing.push_back('0');
  trailing.push_back('0');
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, schema.deserialize_from_hex(
      common::ObString(trailing.size(), trailing.data())));
  EXPECT_TRUE(schema.is_valid());
  EXPECT_EQ(42, schema.get_schema_version());
}

TEST(AIColumnSchemaTest, UnresolvedDefinitionCannotBeStoredAsSQLMetadata)
{
  AIColumnSchema schema;
  common::ObArenaAllocator allocator;
  common::ObString encoded;
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, schema.serialize_to_hex(allocator, encoded));
  EXPECT_TRUE(encoded.empty());
}

TEST(AIColumnSchemaTest, NegativeRetryCountIsRejectedWithoutChangingConfiguration)
{
  AIColumnSchema schema;
  ASSERT_EQ(common::OB_SUCCESS, schema.set_max_retries(2));
  EXPECT_EQ(common::OB_INVALID_ARGUMENT, schema.set_max_retries(-1));
  EXPECT_EQ(2, schema.get_max_retries());
}
} // namespace
} // namespace schema
} // namespace share
} // namespace oceanbase
