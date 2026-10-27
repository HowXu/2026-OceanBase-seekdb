/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase { namespace sql {
namespace {
const ParseNode *find_node(const ParseNode *node, ObItemType type)
{
  if (node == nullptr || node->type_ == type) return node;
  for (int i = 0; i < node->num_child_; ++i) {
    if (const ParseNode *found = find_node(node->children_[i], type)) return found;
  }
  return nullptr;
}

TEST(FileSqlParser, table_functions_aliases_options_and_native_join)
{
  const char *queries[] = {
    "SELECT * FROM read_csv('/tmp/input.csv', header=true, delim=';') AS c",
    "SELECT * FROM read_json('/tmp/input.jsonl') j",
    "SELECT * FROM read_ndjson('/tmp/input.jsonl')",
    "SELECT * FROM read_parquet('/tmp/input.parquet')",
    "SELECT file FROM glob('/tmp/*.csv') ORDER BY file",
    "SELECT c.id, j.name FROM read_csv('/tmp/a.csv') c LEFT JOIN read_json('/tmp/b.jsonl') j ON c.id=j.id",
    "SELECT id, COUNT(*) n FROM read_csv('/tmp/a.csv') GROUP BY id HAVING n>1 ORDER BY n LIMIT 20"
  };
  for (const char *query : queries) {
    common::ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result{};
    ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString(query), result)) << query;
    const ParseNode *file = find_node(result.result_tree_, T_FILE_TABLE_EXPRESSION);
    ASSERT_NE(nullptr, file) << query;
    EXPECT_EQ(3, file->num_child_);
    EXPECT_EQ(T_VARCHAR, file->children_[1]->children_[0]->type_);
    EXPECT_TRUE(file->is_tree_not_param_);
    parser.free_result(result);
  }
}

TEST(FileSqlParser, describe_and_copy)
{
  common::ObArenaAllocator allocator;
  ObParser parser(allocator, 0);
  ParseResult result{};
  ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString("DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')"), result));
  const ParseNode *explain = find_node(result.result_tree_, T_EXPLAIN);
  ASSERT_NE(nullptr, explain);
  EXPECT_TRUE(explain->is_tree_not_param_);
  parser.free_result(result);
  const char *queries[] = {
    "COPY (SELECT * FROM read_csv('/tmp/a.csv')) TO '/tmp/out.csv' (format csv, header, nullstr 'NULL')",
    "COPY ordinary_table TO '/tmp/out.csv' (delimiter ';', quote '\"', escape '\"', header false)"
  };
  for (const char *query : queries) {
    result = ParseResult{};
    ASSERT_EQ(common::OB_SUCCESS, parser.parse(common::ObString(query), result)) << query;
    const ParseNode *into = find_node(result.result_tree_, T_INTO_OUTFILE);
    ASSERT_NE(nullptr, into);
    EXPECT_EQ(1, into->value_);
    EXPECT_TRUE(into->is_tree_not_param_);
    parser.free_result(result);
  }
}
} // namespace
}} // namespace oceanbase::sql
