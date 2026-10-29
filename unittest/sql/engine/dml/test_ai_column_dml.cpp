/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/dml/ob_ai_column_dml_service.h"
#include "sql/parser/ob_parser.h"
#include "common/rowkey/ob_rowkey.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;
using namespace share::schema;

class AIColumnDmlTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    table_.set_table_id(500001);
    table_.set_table_type(USER_TABLE);
    table_.set_schema_version(100);
    table_.set_charset_type(CHARSET_UTF8MB4);
    table_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, table_.set_table_name("docs"));
    add_column(17, "id", ObIntType, true);
    add_column(18, "content", ObVarcharType);
    add_column(19, "title", ObVarcharType);
    add_column(20, "tags", ObJsonType);
    add_column(21, "score", ObIntType);
    add_column(22, "unrelated", ObVarcharType);
    for (uint64_t i = 23; i <= 28; ++i) {
      const std::string name = "__ai_hidden_" + std::to_string(i);
      add_column(i, name.c_str(), ObIntType);
      table_.get_column_schema(i)->set_is_hidden(true);
    }
    define_ai(20, 18, 23);
    define_ai(21, 19, 26);
    params_.need_cast_expr_ = true;
  }
  void add_column(uint64_t id, const char *name, ObObjType type, bool rowkey = false)
  {
    ObColumnSchemaV2 column;
    column.set_table_id(table_.get_table_id());
    column.set_column_id(id);
    column.set_schema_version(100);
    column.set_data_type(type);
    column.set_charset_type(CHARSET_UTF8MB4);
    column.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    column.set_data_length(200);
    if (rowkey) { column.set_rowkey_position(1); column.set_nullable(false); }
    ASSERT_EQ(OB_SUCCESS, column.set_column_name(name));
    ASSERT_EQ(OB_SUCCESS, table_.add_column(column));
  }
  void define_ai(uint64_t id, uint64_t dependency, uint64_t hidden)
  {
    AIColumnSchema definition;
    ObSEArray<uint64_t, 1> dependencies;
    ASSERT_EQ(OB_SUCCESS, dependencies.push_back(dependency));
    ASSERT_EQ(OB_SUCCESS, definition.set_column_name(table_.get_column_schema(id)->get_column_name_str()));
    ASSERT_EQ(OB_SUCCESS, definition.set_expression("AI_COMPLETE('m',content)"));
    ASSERT_EQ(OB_SUCCESS, definition.set_normalized_expr(
        "{\"function\":\"AI_COMPLETE\",\"model\":\"m\",\"input_expr\":\"concat('Classify: ',content)\","
        "\"config_expr\":\"JSON_OBJECT()\",\"system_prompt\":\"Return exactly one valid JSON value\",\"dimension\":0}"));
    ASSERT_EQ(OB_SUCCESS, definition.set_dependency_ids(dependencies));
    definition.set_state_column_id(hidden);
    definition.set_request_column_id(hidden + 1);
    definition.set_result_spec_column_id(hidden + 2);
    ASSERT_EQ(OB_SUCCESS, table_.get_column_schema(id)->set_ai_column_schema(definition));
  }
  void key(int64_t value, AIColumnRowkey &out)
  {
    ObSEArray<uint64_t, 1> ids;
    ObSEArray<ObObj, 1> values;
    ASSERT_EQ(OB_SUCCESS, ids.push_back(17));
    ObObj object; object.set_int(value);
    ASSERT_EQ(OB_SUCCESS, values.push_back(object));
    ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_rowkey(table_, ids, values, params_, allocator_, out));
  }
  void expect_parseable(const ObString &sql)
  {
    ObParser parser(allocator_, 0);
    ParseResult result{};
    EXPECT_EQ(OB_SUCCESS, parser.parse(sql, result)) << sql.ptr();
    parser.free_result(result);
  }
  ObArenaAllocator allocator_;
  ObTableSchema table_;
  ObObjPrintParams params_;
};

TEST_F(AIColumnDmlTest, UnrelatedChangesProduceNoWork)
{
  ObSEArray<uint64_t, 1> changed;
  ObSEArray<uint64_t, 2> affected;
  ASSERT_EQ(OB_SUCCESS, changed.push_back(22));
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::collect_affected_columns(table_, changed, false, affected));
  EXPECT_EQ(0, affected.count());
}

TEST_F(AIColumnDmlTest, OnlyDependentColumnsAreInvalidated)
{
  ObSEArray<uint64_t, 1> changed;
  ObSEArray<uint64_t, 2> affected;
  ASSERT_EQ(OB_SUCCESS, changed.push_back(18));
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::collect_affected_columns(table_, changed, false, affected));
  ASSERT_EQ(1, affected.count());
  EXPECT_EQ(20, affected.at(0));
}

TEST_F(AIColumnDmlTest, PrimaryKeyChangesAndInsertsInvalidateEveryAIColumn)
{
  ObSEArray<uint64_t, 1> changed;
  ObSEArray<uint64_t, 2> affected;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::collect_affected_columns(table_, changed, true, affected));
  ASSERT_EQ(2, affected.count());
  EXPECT_EQ(20, affected.at(0));
  EXPECT_EQ(21, affected.at(1));
}

TEST_F(AIColumnDmlTest, RowkeyUsesColumnIDsAndKeepsStructuredValues)
{
  AIColumnRowkey result;
  ObSEArray<uint64_t, 2> ids;
  ObSEArray<ObObj, 2> values;
  ASSERT_EQ(OB_SUCCESS, ids.push_back(18));
  ASSERT_EQ(OB_SUCCESS, ids.push_back(17));
  ObObj input; input.set_varchar("not-a-key"); input.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ASSERT_EQ(OB_SUCCESS, values.push_back(input));
  ObObj id; id.set_int(2);
  ASSERT_EQ(OB_SUCCESS, values.push_back(id));
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_rowkey(table_, ids, values, params_, allocator_, result));
  EXPECT_STREQ("`id` = 2", result.predicate_.ptr());
  ASSERT_GT(result.hex_.length(), 0);
  ObString bytes;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::decode_rowkey_hex(result.hex_.string(), allocator_, bytes));
  ObRowkey decoded;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(allocator_, bytes.ptr(), bytes.length(), pos));
  ASSERT_EQ(1, decoded.get_obj_cnt());
  EXPECT_EQ(2, decoded.get_obj_ptr()[0].get_int());
  EXPECT_EQ(bytes.length(), pos);
}

TEST_F(AIColumnDmlTest, MissingOrNullPrimaryKeyCannotCreateABroadPredicate)
{
  AIColumnRowkey result;
  ObSEArray<uint64_t, 1> ids;
  ObSEArray<ObObj, 1> values;
  ASSERT_EQ(OB_SUCCESS, ids.push_back(18));
  ASSERT_EQ(OB_SUCCESS, values.push_back(ObObj()));
  EXPECT_EQ(OB_INVALID_ARGUMENT, AIColumnDmlService::build_rowkey(table_, ids, values, params_, allocator_, result));
  ids.at(0) = 17;
  EXPECT_EQ(OB_ERR_NULL_VALUE, AIColumnDmlService::build_rowkey(table_, ids, values, params_, allocator_, result));
}

TEST_F(AIColumnDmlTest, DeleteOnlyCancelsTasksForTheExactStructuredKey)
{
  AIColumnRowkey old_key;
  key(2, old_key);
  ObSEArray<uint64_t, 2> affected;
  ObSEArray<ObString, 4> statements;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_mutations(table_, "test", &old_key, nullptr,
      affected, ObString(), allocator_, statements));
  ASSERT_EQ(1, statements.count());
  const std::string sql(statements.at(0).ptr());
  EXPECT_NE(std::string::npos, sql.find("pk_hash"));
  EXPECT_NE(std::string::npos, sql.find("pk_blob"));
  EXPECT_EQ(std::string::npos, sql.find("UPDATE"));
  expect_parseable(statements.at(0));
}

TEST_F(AIColumnDmlTest, OneRowSharesOneRequestAndInvalidatesBeforeEnqueueing)
{
  AIColumnRowkey old_key, new_key;
  key(1, old_key); key(2, new_key);
  ObSEArray<uint64_t, 2> affected;
  ASSERT_EQ(OB_SUCCESS, affected.push_back(20));
  ASSERT_EQ(OB_SUCCESS, affected.push_back(21));
  ObSEArray<ObString, 4> statements;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_mutations(table_, "test", &old_key, &new_key,
      affected, "request-group", allocator_, statements));
  ASSERT_EQ(4, statements.count());
  EXPECT_EQ(0, std::string(statements.at(0).ptr()).find("DELETE"));
  EXPECT_EQ(0, std::string(statements.at(1).ptr()).find("UPDATE"));
  EXPECT_NE(std::string::npos, std::string(statements.at(1).ptr()).find("`tags` = NULL"));
  EXPECT_NE(std::string::npos, std::string(statements.at(1).ptr()).find("`score` = NULL"));
  EXPECT_NE(std::string::npos, std::string(statements.at(1).ptr()).find("WHERE `id` = 2"));
  for (int64_t i = 0; i < statements.count(); ++i) { expect_parseable(statements.at(i)); }
  for (int64_t i = 2; i < 4; ++i) {
    const std::string sql(statements.at(i).ptr());
    EXPECT_EQ(0, sql.find("INSERT"));
    EXPECT_NE(std::string::npos, sql.find("NOW(6)"));
    EXPECT_NE(std::string::npos, sql.find("concat('Classify: ',content)"));
    EXPECT_NE(std::string::npos, sql.find("JSON_SET"));
    EXPECT_NE(std::string::npos, sql.find(i == 2 ? "AND `__ai_hidden_24` = " : "AND `__ai_hidden_27` = "));
    EXPECT_NE(std::string::npos, sql.find("726571756573742D67726F7570"));
  }
}

TEST_F(AIColumnDmlTest, IdentifiersAndStringKeysCannotEscapeTheGeneratedSQL)
{
  ASSERT_EQ(OB_SUCCESS, table_.set_table_name("odd`table"));
  ObColumnSchemaV2 *column = table_.get_column_schema(17);
  column->set_data_type(ObVarcharType);
  ASSERT_EQ(OB_SUCCESS, column->set_column_name("odd`key"));
  ObSEArray<uint64_t, 1> ids;
  ObSEArray<ObObj, 1> values;
  ASSERT_EQ(OB_SUCCESS, ids.push_back(17));
  ObObj value; value.set_varchar("x' OR 1=1 -- \\"); value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ASSERT_EQ(OB_SUCCESS, values.push_back(value));
  AIColumnRowkey result;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_rowkey(table_, ids, values, params_, allocator_, result));
  EXPECT_EQ(0, std::string(result.predicate_.ptr()).find("`odd``key` = "));
  ObSEArray<uint64_t, 2> affected;
  ASSERT_EQ(OB_SUCCESS, affected.push_back(20));
  ObSEArray<ObString, 4> statements;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_mutations(table_, "odd`db", &result, &result,
      affected, "request'quoted", allocator_, statements));
  EXPECT_NE(std::string::npos, std::string(statements.at(1).ptr()).find("`odd``db`.`odd``table`"));
  for (int64_t i = 0; i < statements.count(); ++i) { expect_parseable(statements.at(i)); }
}

TEST_F(AIColumnDmlTest, InternalResultAndStateWritesCannotReenqueueThemselves)
{
  ObSEArray<uint64_t, 4> changed;
  ObSEArray<uint64_t, 2> affected;
  for (uint64_t id : {20UL, 23UL, 24UL, 25UL}) {
    ASSERT_EQ(OB_SUCCESS, changed.push_back(id));
  }
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::collect_affected_columns(table_, changed, false, affected));
  EXPECT_EQ(0, affected.count());
}

TEST_F(AIColumnDmlTest, MalformedStructuredKeysAreRejected)
{
  ObString bytes;
  EXPECT_EQ(OB_INVALID_ARGUMENT, AIColumnDmlService::decode_rowkey_hex("0", allocator_, bytes));
  EXPECT_EQ(OB_INVALID_ARGUMENT, AIColumnDmlService::decode_rowkey_hex("xx", allocator_, bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(AIColumnDmlTest, NoAffectedColumnsMeansNoMutation)
{
  AIColumnRowkey result;
  key(2, result);
  ObSEArray<uint64_t, 2> affected;
  ObSEArray<ObString, 4> statements;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_mutations(table_, "test", &result, &result,
      affected, "unused-request", allocator_, statements));
  EXPECT_EQ(0, statements.count());
}
TEST_F(AIColumnDmlTest, RetryBuildsAtomicSingleColumnSetStatements)
{
  ObSEArray<ObString, 3> statements;
  ASSERT_EQ(OB_SUCCESS, AIColumnDmlService::build_retry_statements(table_, "test", 20,
      "(`id` IN (1,3))", allocator_, statements));
  ASSERT_EQ(3, statements.count());
  const std::string erase(statements.at(0).ptr());
  const std::string invalidate(statements.at(1).ptr());
  const std::string enqueue(statements.at(2).ptr());
  EXPECT_NE(std::string::npos, erase.find("DELETE"));
  EXPECT_NE(std::string::npos, invalidate.find("UUID()"));
  EXPECT_NE(std::string::npos, invalidate.find("`tags` = NULL"));
  EXPECT_EQ(std::string::npos, invalidate.find("`score` = NULL"));
  EXPECT_NE(std::string::npos, enqueue.find("JSON_OBJECT('input'"));
  for (int64_t i = 0; i < statements.count(); ++i) {
    EXPECT_NE(std::string::npos, std::string(statements.at(i).ptr()).find("`id` IN (1,3)"));
    expect_parseable(statements.at(i));
  }
  EXPECT_EQ(OB_INVALID_ARGUMENT, AIColumnDmlService::build_retry_statements(table_, "test", 18,
      "1=1", allocator_, statements));
}
} // namespace
} // namespace sql
} // namespace oceanbase
