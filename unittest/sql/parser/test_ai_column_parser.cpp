/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
namespace sql
{
namespace
{
const ParseNode *find_node(const ParseNode *node, const ObItemType type)
{
  if (node == nullptr || node->type_ == type) {
    return node;
  }
  for (int i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_node(node->children_[i], type)) {
      return found;
    }
  }
  return nullptr;
}

class AIColumnParserTreeTest : public ::testing::Test
{
protected:
  AIColumnParserTreeTest() : parser_(allocator_, 0), result_{} {}
  ~AIColumnParserTreeTest() override { parser_.free_result(result_); }
  const ParseNode *parse(const char *query)
  {
    const int ret = parser_.parse(common::ObString(query), result_);
    EXPECT_EQ(common::OB_SUCCESS, ret) << query << ": "
        << (result_.error_msg_ == nullptr ? "" : result_.error_msg_);
    return ret == common::OB_SUCCESS ? result_.result_tree_ : nullptr;
  }
  common::ObArenaAllocator allocator_;
  ObParser parser_;
  ParseResult result_;
};

void expect_parse(const char *query, const int expected_ret)
{
  common::ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result{};
  const int ret = parser.parse(common::ObString(query), result);
  EXPECT_EQ(expected_ret, ret) << query << ": "
      << (result.error_msg_ == nullptr ? "" : result.error_msg_)
      << ", column=" << result.yycolumn_;
  parser.free_result(result);
}

TEST(AIColumnParserTest, CreateTableAIColumn)
{
  expect_parse(
      "CREATE TABLE t(id BIGINT PRIMARY KEY, c VARCHAR(20), "
      "v VARCHAR(30) AI COLUMN(AI_COMPLETE('m',c)) "
      "WITH(sync_mode='async',max_retries=2))",
      common::OB_SUCCESS);
}

TEST(AIColumnParserTest, AddAndModifyAIColumn)
{
  expect_parse(
      "ALTER TABLE t ADD COLUMN v VARCHAR(30) "
      "AI COLUMN(AI_COMPLETE('m',c)) WITH(sync_mode='async')",
      common::OB_SUCCESS);
  expect_parse(
      "ALTER TABLE t MODIFY COLUMN v VARCHAR(30) "
      "AI COLUMN(AI_COMPLETE('m',c)) WITH(sync_mode='async',max_retries=0)",
      common::OB_SUCCESS);
}

TEST(AIColumnParserTest, RetryAIAndShowAIColumns)
{
  expect_parse(
      "ALTER TABLE t RETRY AI COLUMN v WHERE status='FAILED' AND id=1",
      common::OB_SUCCESS);
  expect_parse("ALTER TABLE t RETRY AI COLUMN v", common::OB_SUCCESS);
  expect_parse("SHOW AI COLUMNS FROM t", common::OB_SUCCESS);
}

TEST(AIColumnParserTest, MixedRetryActionsReachResolverForNotSupportedError)
{
  expect_parse("ALTER TABLE t RETRY AI COLUMN v, ADD COLUMN x INT",
               common::OB_SUCCESS);
}

TEST(AIColumnParserTest, NonReservedKeywordsAndResolverOptions)
{
  expect_parse("SELECT ai, retry FROM ordinary_table", common::OB_SUCCESS);
  expect_parse(
      "CREATE TABLE t(c TEXT, v JSON AI COLUMN(AI_COMPLETE('m',c)) "
      "WITH(unknown_option=1,max_retries=-1,max_retries=2))",
      common::OB_SUCCESS);
  expect_parse(
      "CREATE TABLE t(c TEXT, v VECTOR(4) AI COLUMN(AI_EMBED('m',c)))",
      common::OB_SUCCESS);
}

TEST_F(AIColumnParserTreeTest, DefinitionKeepsExpressionAndOptions)
{
  const ParseNode *root = parse(
      "CREATE TABLE t(c TEXT, v JSON AI COLUMN(AI_COMPLETE('m',c)) "
      "WITH(sync_mode='async',max_retries=2))");
  const ParseNode *definition = find_node(root, T_COLUMN_DEFINITION);
  ASSERT_NE(nullptr, definition);
  EXPECT_EQ(4, definition->num_child_);
  const ParseNode *ai = find_node(root, T_AI_COLUMN);
  ASSERT_NE(nullptr, ai);
  ASSERT_EQ(2, ai->num_child_);
  ASSERT_NE(nullptr, ai->children_[0]);
  const ParseNode *options = ai->children_[1];
  ASSERT_NE(nullptr, options);
  EXPECT_EQ(T_AI_COLUMN_OPTIONS, options->type_);
  ASSERT_EQ(2, options->num_child_);
  ASSERT_EQ(T_AI_COLUMN_OPTION, options->children_[0]->type_);
  EXPECT_EQ(common::ObString("sync_mode"),
            common::ObString(options->children_[0]->children_[0]->str_len_,
                             options->children_[0]->children_[0]->str_value_));
  EXPECT_EQ(common::ObString("async"),
            common::ObString(options->children_[0]->children_[1]->str_len_,
                             options->children_[0]->children_[1]->str_value_));
  EXPECT_EQ(2, options->children_[1]->children_[1]->value_);
}

TEST_F(AIColumnParserTreeTest, QuotedStatusSurvivesRetryPredicate)
{
  const ParseNode *root = parse(
      "ALTER TABLE t RETRY AI COLUMN v WHERE `status`='active' AND status='FAILED'");
  const ParseNode *retry = find_node(root, T_RETRY_AI_COLUMN);
  ASSERT_NE(nullptr, retry);
  ASSERT_EQ(2, retry->num_child_);
  ASSERT_NE(nullptr, retry->children_[1]);
  const ParseNode *predicate = retry->children_[1]->children_[0];
  ASSERT_EQ(T_OP_AND, predicate->type_);
  const ParseNode *quoted = find_node(predicate->children_[0], T_IDENT);
  const ParseNode *unquoted = find_node(predicate->children_[1], T_IDENT);
  ASSERT_NE(nullptr, quoted);
  ASSERT_NE(nullptr, unquoted);
  EXPECT_TRUE(quoted->is_input_quoted_);
  EXPECT_FALSE(unquoted->is_input_quoted_);
}

TEST_F(AIColumnParserTreeTest, ShowKeepsQualifiedTableAndCondition)
{
  const ParseNode *root = parse("SHOW AI COLUMNS FROM db.t WHERE Field='v'");
  const ParseNode *show = find_node(root, T_SHOW_AI_COLUMNS);
  ASSERT_NE(nullptr, show);
  ASSERT_EQ(3, show->num_child_);
  ASSERT_NE(nullptr, show->children_[0]);
  EXPECT_EQ(T_RELATION_FACTOR, show->children_[0]->type_);
  EXPECT_NE(nullptr, show->children_[2]);
}
} // namespace
} // namespace sql
} // namespace oceanbase
