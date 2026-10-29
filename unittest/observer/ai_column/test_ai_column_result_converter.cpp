/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include "observer/ai_column/ob_ai_column_result_converter.h"

namespace oceanbase
{
namespace observer
{
namespace
{
using namespace common;
using namespace share::schema;

TEST(AIColumnRetryTest, TwoRetriesAllowThreeAttempts)
{
  AIColumnTaskDecision decision(2);
  EXPECT_EQ(AIColumnTaskAction::CALL_MODEL, decision.after_failure(0));
  EXPECT_EQ(AIColumnTaskAction::CALL_MODEL, decision.after_failure(1));
  EXPECT_EQ(AIColumnTaskAction::MARK_FAILED, decision.after_failure(2));
}

class AIColumnResultConverterTest : public ::testing::Test
{
protected:
  void configure(ObObjType type)
  {
    column_.set_data_type(type);
    column_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  }
  ObArenaAllocator allocator_;
  ObColumnSchemaV2 column_;
};

TEST_F(AIColumnResultConverterTest, JsonUsesAnExplicitJSONCast)
{
  configure(ObJsonType);
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, AIColumnResultConverter::append_value(sql, column_, "{\"a\":1}", allocator_));
  EXPECT_NE(std::string::npos, std::string(sql.ptr()).find("CAST("));
  EXPECT_NE(std::string::npos, std::string(sql.ptr()).find("AS JSON"));
}

TEST_F(AIColumnResultConverterTest, StringsAreQuotedWithoutSQLInjection)
{
  configure(ObVarcharType);
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, AIColumnResultConverter::append_value(sql, column_, "x' OR 1=1 -- \\", allocator_));
  const std::string text(sql.ptr());
  EXPECT_NE(std::string::npos, text.find("CONVERT(x'"));
  EXPECT_EQ(std::string::npos, text.find("x' OR 1=1"));
}

TEST_F(AIColumnResultConverterTest, IntegersRequireOneCompleteBase10Value)
{
  configure(ObIntType);
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, AIColumnResultConverter::append_value(sql, column_, "-7", allocator_));
  EXPECT_STREQ("-7", sql.ptr());
  EXPECT_EQ(OB_INVALID_DATA, AIColumnResultConverter::append_value(sql, column_, "7x", allocator_));
  EXPECT_EQ(OB_INVALID_DATA, AIColumnResultConverter::append_value(sql, column_, "1.5", allocator_));
  EXPECT_EQ(OB_INVALID_DATA, AIColumnResultConverter::append_value(sql, column_, "9223372036854775808", allocator_));
}

TEST_F(AIColumnResultConverterTest, VectorMustMatchDeclaredDimensionAndFiniteValues)
{
  configure(ObCollectionSQLType);
  ObSEArray<ObString, 1> info;
  ASSERT_EQ(OB_SUCCESS, info.push_back("VECTOR(4)"));
  ASSERT_EQ(OB_SUCCESS, column_.set_extended_type_info(info));
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, AIColumnResultConverter::append_value(sql, column_, "[0,1,2,3]", allocator_));
  EXPECT_EQ(OB_INVALID_DATA, AIColumnResultConverter::append_value(sql, column_, "[0,1,2]", allocator_));
  EXPECT_EQ(OB_INVALID_DATA, AIColumnResultConverter::append_value(sql, column_, "[0,1,2,null]", allocator_));
}

TEST_F(AIColumnResultConverterTest, IdentifiersEscapeBackticks)
{
  ObSqlString sql;
  ASSERT_EQ(OB_SUCCESS, AIColumnResultConverter::append_identifier(sql, "odd`table"));
  EXPECT_STREQ("`odd``table`", sql.ptr());
}
} // namespace
} // namespace observer
} // namespace oceanbase
