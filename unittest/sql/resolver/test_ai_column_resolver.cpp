/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <string>
#include "lib/oblog/ob_warning_buffer.h"
#include "sql/parser/ob_parser.h"
#include "sql/ob_sql_init.h"
#include "sql/engine/ob_exec_context.h"
#include "share/schema/ob_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/printer/ob_schema_printer.h"
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "share/system_variable/ob_system_variable_init.h"

namespace oceanbase
{
namespace sql
{
namespace
{
using namespace common;
using namespace share::schema;

const ParseNode *find_ai_node(const ParseNode *node, const ObItemType type)
{
  if (nullptr == node || node->type_ == type) { return node; }
  const ParseNode *found = nullptr;
  for (int i = 0; nullptr == found && i < node->num_child_; ++i) {
    found = find_ai_node(node->children_[i], type);
  }
  return found;
}

class AIColumnResolverTest : public ::testing::Test
{
protected:
  AIColumnResolverTest() : parser_(allocator_, 0), parsed_{}, exec_ctx_(allocator_) {}
  ~AIColumnResolverTest() override { parser_.free_result(parsed_); }
  static void SetUpTestCase()
  {
    ASSERT_EQ(OB_SUCCESS, init_sql_factories());
    ASSERT_EQ(OB_SUCCESS, init_sql_expr_static_var());
    ASSERT_EQ(OB_SUCCESS, share::ObSysVariables::init_default_values());
  }
  void SetUp() override
  {
    ASSERT_EQ(OB_SUCCESS, session_.test_init(0, 1, &allocator_));
    ASSERT_EQ(OB_SUCCESS, session_.load_default_sys_variable(false, false));
    exec_ctx_.set_my_session(&session_);
    ObSQLSessionInfo::ExecCtxSessionRegister register_ctx(session_, &exec_ctx_);
    ASSERT_EQ(OB_SUCCESS, exec_ctx_.create_physical_plan_ctx());
    table_.set_table_id(500001);
    table_.set_table_type(USER_TABLE);
    ASSERT_EQ(OB_SUCCESS, table_.set_table_name("docs"));
    table_.set_charset_type(CHARSET_UTF8MB4);
    table_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ObColumnSchemaV2 source;
    source.set_table_id(table_.get_table_id());
    source.set_column_id(17);
    source.set_data_type(ObVarcharType);
    source.set_data_length(200);
    source.set_charset_type(CHARSET_UTF8MB4);
    source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, source.set_column_name("content"));
    ASSERT_EQ(OB_SUCCESS, table_.add_column(source));
    column_.set_table_id(table_.get_table_id());
    column_.set_column_id(18);
    column_.set_data_type(ObJsonType);
    column_.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    column_.set_schema_version(42);
    ASSERT_EQ(OB_SUCCESS, column_.set_column_name("result"));
    ASSERT_EQ(OB_SUCCESS, model_.set_name("m"));
    model_.set_type(share::EndpointType::COMPLETION);
  }
  const ParseNode *attribute(const char *expression, const char *options = "")
  {
    const std::string sql = std::string("CREATE TABLE docs(result JSON AI COLUMN(")
        + expression + ") " + options + ")";
    const int ret = parser_.parse(ObString(sql.c_str()), parsed_);
    EXPECT_EQ(OB_SUCCESS, ret);
    return ret == OB_SUCCESS ? find_ai_node(parsed_.result_tree_, T_AI_COLUMN) : nullptr;
  }
  int resolve(const char *expression, const char *options = "")
  {
    const ParseNode *node = attribute(expression, options);
    return nullptr == node ? OB_ERR_UNEXPECTED : AIColumnResolver::resolve_column_attribute(*node, column_);
  }
  void vector_column()
  {
    column_.set_data_type(ObCollectionSQLType);
    ObSEArray<ObString, 1> info;
    ASSERT_EQ(OB_SUCCESS, info.push_back(ObString("VECTOR(4)")));
    ASSERT_EQ(OB_SUCCESS, column_.set_extended_type_info(info));
    model_.set_type(share::EndpointType::DENSE_EMBEDDING);
  }
  int validate()
  {
    return AIColumnResolver::resolve_expression(column_, table_, model_, session_);
  }
  std::string normalized() const
  {
    const ObString &text = column_.get_ai_column_schema()->get_normalized_expr();
    return std::string(text.ptr(), text.length());
  }
  ObArenaAllocator allocator_;
  ObParser parser_;
  ParseResult parsed_;
  ObSQLSessionInfo session_;
  ObExecContext exec_ctx_;
  ObTableSchema table_;
  ObColumnSchemaV2 column_;
  ObAiModelSchema model_;
};

TEST_F(AIColumnResolverTest, AsyncOptionsAndExpressionAreOwned)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)", "WITH(SYNC_MODE='async',max_retries=3)"));
  ASSERT_TRUE(column_.is_ai_column());
  EXPECT_EQ(3, column_.get_ai_column_schema()->get_max_retries());
  ASSERT_EQ(OB_SUCCESS, validate());
  ASSERT_EQ(1, column_.get_ai_column_schema()->get_dependency_count());
  EXPECT_EQ(17, column_.get_ai_column_schema()->get_dependency_ids()[0]);
  EXPECT_NE(std::string::npos, normalized().find("Return exactly one valid JSON value"));
  parser_.free_result(parsed_);
  EXPECT_FALSE(column_.get_ai_column_schema()->get_expression().empty());
}

TEST_F(AIColumnResolverTest, DefaultsPermitAnExpressionWithNoRowDependencies)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m','constant')"));
  EXPECT_EQ(2, column_.get_ai_column_schema()->get_max_retries());
  ASSERT_EQ(OB_SUCCESS, validate());
  EXPECT_EQ(0, column_.get_ai_column_schema()->get_dependency_count());
}

TEST_F(AIColumnResolverTest, RejectSyncMode)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(sync_mode='sync')")); }
TEST_F(AIColumnResolverTest, RejectNegativeRetries)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(max_retries=-1)")); }
TEST_F(AIColumnResolverTest, RejectDecimalRetries)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(max_retries=1.5)")); }
TEST_F(AIColumnResolverTest, RejectStringRetries)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(max_retries='2')")); }
TEST_F(AIColumnResolverTest, RejectOverflowRetries)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(max_retries=18446744073709551615)")); }
TEST_F(AIColumnResolverTest, RejectUnknownOptions)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(unknown_option=1)")); }
TEST_F(AIColumnResolverTest, RejectDuplicateOptionsIgnoringCase)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE('m',content)", "WITH(max_retries=1,MAX_RETRIES=2)")); }

TEST_F(AIColumnResolverTest, RejectWrongModelCategory)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  model_.set_type(share::EndpointType::DENSE_EMBEDDING);
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}
TEST_F(AIColumnResolverTest, RejectNonLiteralModel)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("AI_COMPLETE(content,content)")); }
TEST_F(AIColumnResolverTest, RejectNonAIFunction)
{ EXPECT_EQ(OB_NOT_SUPPORTED, resolve("UPPER(content)")); }
TEST_F(AIColumnResolverTest, RejectSelfDependency)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',result)"));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}
TEST_F(AIColumnResolverTest, RejectAIColumnDependency)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  AIColumnSchema definition;
  ASSERT_EQ(OB_SUCCESS, table_.get_column_schema(17)->set_ai_column_schema(definition));
  ObWarningBufferIgnoreScope warning_scope;
  const bool was_enabled = ObWarningBuffer::is_warn_log_on();
  ObWarningBuffer::set_warn_log_on(true);
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
  // Let the SQL layer use the standard 1235 message, not a custom "... not supported".
  EXPECT_TRUE(ob_get_tsi_err_msg(OB_NOT_SUPPORTED).empty());
  ObWarningBuffer::set_warn_log_on(was_enabled);
}
TEST_F(AIColumnResolverTest, RejectMissingDependency)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',missing_column)"));
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}
TEST_F(AIColumnResolverTest, NativeExpressionsDeduplicateDependencies)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',concat(content,upper(content)))"));
  ASSERT_EQ(OB_SUCCESS, validate());
  EXPECT_EQ(1, column_.get_ai_column_schema()->get_dependency_count());
}
TEST_F(AIColumnResolverTest, IntegerHintDoesNotChangeUserConfigOrInput)
{
  column_.set_data_type(ObIntType);
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content,'{\"temperature\":0.2}')"));
  ASSERT_EQ(OB_SUCCESS, validate());
  EXPECT_NE(std::string::npos, normalized().find("base-10 integer"));
  EXPECT_NE(std::string::npos, normalized().find("temperature"));
  EXPECT_NE(std::string::npos, normalized().find("input_expr"));
  EXPECT_NE(std::string::npos, normalized().find("config_expr"));
}
TEST_F(AIColumnResolverTest, RejectNestedAIRequests)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',AI_COMPLETE('m',content))"));
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}
TEST_F(AIColumnResolverTest, RejectSubqueryInput)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',(SELECT content FROM other_table))"));
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}
TEST_F(AIColumnResolverTest, RejectExplicitEmbeddingDimensionMismatch)
{
  vector_column();
  ASSERT_EQ(OB_SUCCESS, resolve("AI_EMBED('m',content,3)"));
  EXPECT_EQ(OB_ERR_INVALID_VECTOR_DIM, validate());
}
TEST_F(AIColumnResolverTest, ImplicitEmbeddingDimensionUsesTargetVector)
{
  vector_column();
  ASSERT_EQ(OB_SUCCESS, resolve("AI_EMBED('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  EXPECT_NE(std::string::npos, normalized().find("dimension"));
}
TEST_F(AIColumnResolverTest, RejectEmbeddingIntoNonVector)
{
  model_.set_type(share::EndpointType::DENSE_EMBEDDING);
  ASSERT_EQ(OB_SUCCESS, resolve("AI_EMBED('m',content)"));
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}
TEST_F(AIColumnResolverTest, RejectCompletionIntoVector)
{
  vector_column();
  model_.set_type(share::EndpointType::COMPLETION);
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  EXPECT_EQ(OB_NOT_SUPPORTED, validate());
}

TEST_F(AIColumnResolverTest, OrdinaryAlterDoesNotRequireAnAIModelOrAllocateHiddenColumns)
{
  AlterTableSchema changes;
  AlterColumnSchema action;
  ASSERT_EQ(OB_SUCCESS, action.assign(*table_.get_column_schema(ObString("content"))));
  ASSERT_EQ(OB_SUCCESS, action.set_column_name("ordinary"));
  action.set_column_id(OB_INVALID_ID);
  action.alter_type_ = OB_DDL_ADD_COLUMN;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::append_alter_column(
      table_, changes, action, session_, nullptr));
  ASSERT_EQ(1, changes.get_column_count());
  EXPECT_FALSE(changes.get_column_schema_by_idx(0)->is_ai_column());
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::finish_alter_table(table_, changes));
  EXPECT_EQ(1, changes.get_column_count());
  EXPECT_EQ(1, table_.get_column_count());
}

TEST_F(AIColumnResolverTest, AlterAddIDsIncludeEarlierOrdinaryColumns)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',title)"));
  AlterTableSchema changes;
  changes.set_table_id(table_.get_table_id());
  AlterColumnSchema title;
  ASSERT_EQ(OB_SUCCESS, title.assign(*table_.get_column_schema(ObString("content"))));
  ASSERT_EQ(OB_SUCCESS, title.set_column_name("title"));
  title.set_column_id(OB_INVALID_ID);
  title.alter_type_ = OB_DDL_ADD_COLUMN;
  ASSERT_EQ(OB_SUCCESS, changes.add_alter_column(title, true));
  AlterColumnSchema action;
  ASSERT_EQ(OB_SUCCESS, action.assign(column_));
  action.set_column_id(OB_INVALID_ID);
  action.alter_type_ = OB_DDL_ADD_COLUMN;
  ObTableSchema working;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::prepare_alter_column(table_, changes, action, working));
  EXPECT_EQ(19, action.get_column_id());
  EXPECT_EQ(20, action.get_ai_column_schema()->get_state_column_id());
  EXPECT_EQ(21, action.get_ai_column_schema()->get_request_column_id());
  EXPECT_EQ(22, action.get_ai_column_schema()->get_result_spec_column_id());
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::resolve_expression(action, working, model_, session_));
  ASSERT_EQ(1, action.get_ai_column_schema()->get_dependency_count());
  EXPECT_EQ(18, action.get_ai_column_schema()->get_dependency_ids()[0]);
  EXPECT_EQ(17, table_.get_max_used_column_id());
  EXPECT_EQ(1, table_.get_column_count());
}

TEST_F(AIColumnResolverTest, AlterModifyReusesExistingHiddenColumnIdentities)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, column_, last_id));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  const int64_t count = table_.get_column_count();
  AlterTableSchema changes;
  AlterColumnSchema action;
  ASSERT_EQ(OB_SUCCESS, action.assign(column_));
  ASSERT_EQ(OB_SUCCESS, action.set_origin_column_name("result"));
  action.reset_ai_column_schema();
  const ParseNode *node = attribute("AI_COMPLETE('m',concat('new:',content))");
  ASSERT_NE(nullptr, node);
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::resolve_column_attribute(*node, action));
  action.alter_type_ = OB_DDL_MODIFY_COLUMN;
  ObTableSchema working;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::prepare_alter_column(table_, changes, action, working));
  EXPECT_EQ(count, working.get_column_count());
  EXPECT_EQ(19, action.get_ai_column_schema()->get_state_column_id());
  EXPECT_EQ(20, action.get_ai_column_schema()->get_request_column_id());
  EXPECT_EQ(21, action.get_ai_column_schema()->get_result_spec_column_id());
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::resolve_expression(action, working, model_, session_));
}

TEST_F(AIColumnResolverTest, AlterWithoutNewDeclarationPreservesExistingAIDefinition)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, column_, last_id));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  AlterTableSchema changes;
  AlterColumnSchema action;
  ASSERT_EQ(OB_SUCCESS, action.assign(column_));
  action.reset_ai_column_schema();
  ASSERT_EQ(OB_SUCCESS, action.set_origin_column_name("result"));
  action.alter_type_ = OB_DDL_MODIFY_COLUMN;
  ObTableSchema working;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::prepare_alter_column(table_, changes, action, working));
  ASSERT_TRUE(action.is_ai_column());
  EXPECT_EQ(column_.get_ai_column_schema()->get_expression(),
      action.get_ai_column_schema()->get_expression());
  EXPECT_EQ(19, action.get_ai_column_schema()->get_state_column_id());
}

TEST_F(AIColumnResolverTest, DroppingAnAIColumnAlsoDropsItsThreeHiddenColumns)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, column_, last_id));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  AlterTableSchema changes;
  AlterColumnSchema drop;
  ASSERT_EQ(OB_SUCCESS, drop.assign(column_));
  ASSERT_EQ(OB_SUCCESS, drop.set_origin_column_name("result"));
  drop.alter_type_ = OB_DDL_DROP_COLUMN;
  ASSERT_EQ(OB_SUCCESS, changes.add_alter_column(drop, true));
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::finish_alter_table(table_, changes));
  ASSERT_EQ(4, changes.get_column_count());
  for (int64_t i = 0; i < 4; ++i) {
    const AlterColumnSchema *action =
        static_cast<const AlterColumnSchema *>(changes.get_column_schema_by_idx(i));
    EXPECT_EQ(18 + i, action->get_column_id());
    EXPECT_EQ(OB_DDL_DROP_COLUMN, action->alter_type_);
  }
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::finish_alter_table(table_, changes));
  EXPECT_EQ(4, changes.get_column_count());
  EXPECT_EQ(5, table_.get_column_count());
}

TEST_F(AIColumnResolverTest, RejectDroppingInputOfASurvivingAIColumn)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, column_, last_id));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  AlterTableSchema changes;
  AlterColumnSchema drop;
  ASSERT_EQ(OB_SUCCESS, drop.assign(*table_.get_column_schema(ObString("content"))));
  ASSERT_EQ(OB_SUCCESS, drop.set_origin_column_name("content"));
  drop.alter_type_ = OB_DDL_DROP_COLUMN;
  ASSERT_EQ(OB_SUCCESS, changes.add_alter_column(drop, true));
  EXPECT_EQ(OB_NOT_SUPPORTED, AIColumnResolver::finish_alter_table(table_, changes));
}

TEST_F(AIColumnResolverTest, RejectDependencyMadeAIByALaterAlterAction)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, column_, last_id));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  AlterTableSchema changes;
  AlterColumnSchema action;
  ASSERT_EQ(OB_SUCCESS, action.assign(*table_.get_column_schema(ObString("content"))));
  ASSERT_EQ(OB_SUCCESS, action.set_origin_column_name("content"));
  action.alter_type_ = OB_DDL_MODIFY_COLUMN;
  const ParseNode *node = attribute("AI_COMPLETE('m','constant')");
  ASSERT_NE(nullptr, node);
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::resolve_column_attribute(*node, action));
  ObTableSchema working;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::prepare_alter_column(table_, changes, action, working));
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::resolve_expression(action, working, model_, session_));
  ASSERT_EQ(OB_SUCCESS, changes.add_alter_column(action, true));
  EXPECT_EQ(OB_NOT_SUPPORTED, AIColumnResolver::finish_alter_table(table_, changes));
}

TEST_F(AIColumnResolverTest, SchemaPrinterPreservesAIClauseAndHidesInternalColumns)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, column_, last_id));
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  ObSchemaGetterGuard guard;
  ObSchemaPrinter printer(guard, false, true, false);
  char buffer[8192] = {};
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, printer.print_table_definition_columns(
      table_, buffer, sizeof(buffer), pos, nullptr, LS_BYTE));
  const std::string text(buffer, pos);
  EXPECT_NE(std::string::npos, text.find(
      "AI COLUMN (AI_COMPLETE('m',content)) WITH (sync_mode='async', max_retries=2)"));
  EXPECT_EQ(std::string::npos, text.find("__ai_"));
  const size_t result_pos = text.find("`result`");
  ASSERT_NE(std::string::npos, result_pos);
  EXPECT_EQ(std::string::npos, text.substr(result_pos).find("DEFAULT"));
  const std::string sql = "CREATE TABLE copy (" + text + ")";
  ASSERT_EQ(OB_SUCCESS, parser_.parse(ObString(sql.c_str()), parsed_));
  EXPECT_NE(nullptr, find_ai_node(parsed_.result_tree_, T_AI_COLUMN));
}

TEST_F(AIColumnResolverTest, CreatesThreeDistinctPhysicalHiddenColumns)
{
  ASSERT_EQ(OB_SUCCESS, resolve("AI_COMPLETE('m',content)"));
  ASSERT_EQ(OB_SUCCESS, validate());
  ASSERT_EQ(OB_SUCCESS, table_.add_column(column_));
  uint64_t last_id = 18;
  ASSERT_EQ(OB_SUCCESS, AIColumnResolver::add_hidden_columns(table_, *table_.get_column_schema(18), last_id));
  EXPECT_EQ(21, last_id);
  const AIColumnSchema *definition = table_.get_column_schema(18)->get_ai_column_schema();
  const uint64_t ids[] = {definition->get_state_column_id(), definition->get_request_column_id(), definition->get_result_spec_column_id()};
  EXPECT_NE(ids[0], ids[1]);
  EXPECT_NE(ids[1], ids[2]);
  EXPECT_NE(ids[0], ids[2]);
  EXPECT_FALSE(table_.get_column_schema(18)->is_hidden());
  for (const uint64_t id : ids) {
    const ObColumnSchemaV2 *hidden = table_.get_column_schema(id);
    ASSERT_NE(nullptr, hidden);
    EXPECT_TRUE(hidden->is_hidden());
    EXPECT_FALSE(hidden->is_generated_column());
    EXPECT_FALSE(hidden->is_ai_column());
  }
}
} // namespace
} // namespace sql
} // namespace oceanbase
