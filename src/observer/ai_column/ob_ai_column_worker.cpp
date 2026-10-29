/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_column/ob_ai_column_worker.h"
#include "common/json_type/ob_json_tree.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "observer/ai_column/ob_ai_column_result_converter.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_schema_struct.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/ob_sql_utils.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using common::sqlclient::ObMySQLResult;
using share::OB_ALL_AI_COLUMN_TASK_TNAME;
using namespace share::schema;
using namespace sql;

namespace
{
const int64_t AI_COLUMN_POLL_INTERVAL_US = 100L * 1000L;
const int64_t AI_COLUMN_RETRY_DELAY_US = 200L * 1000L;
const int64_t AI_COLUMN_LEASE_US = 60L * 1000L * 1000L;

int copy_result_id(ObMySQLResult &result, const int64_t index, uint64_t &value)
{
  int64_t signed_value = 0;
  int ret = result.get_int(index, signed_value);
  if (OB_SUCC(ret)) {
    if (signed_value < 0) { ret = OB_INVALID_DATA; }
    else { value = static_cast<uint64_t>(signed_value); }
  }
  return ret;
}

int copy_result_string(ObMySQLResult &result, const int64_t index,
    ObIAllocator &allocator, ObString &value)
{
  ObString raw;
  int ret = result.get_varchar(index, raw);
  if (OB_SUCC(ret)) { ret = ob_write_string(allocator, raw, value); }
  return ret;
}

int json_string(ObIJsonBase &root, const char *name, ObString &value)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *node = root.json_type() == ObJsonNodeType::J_OBJECT
      ? static_cast<ObJsonObject &>(root).get_value(name) : nullptr;
  if (nullptr == node || node->json_type() != ObJsonNodeType::J_STRING) {
    ret = OB_INVALID_DATA;
  } else {
    value = static_cast<ObJsonString *>(node)->get_str();
  }
  return ret;
}

const char *task_where()
{
  return "((state IN (0,3) AND eligible_ts <= NOW(6)) "
      "OR (state = 1 AND lease_expire_ts > 0 AND lease_expire_ts <= %ld))";
}
} // namespace

AIColumnWorker::AIColumnWorker()
    : timer_(), proxy_(nullptr), inited_(false), stopped_(true) {}

AIColumnWorker::~AIColumnWorker() { destroy(); }

int AIColumnWorker::start(ObMySQLProxy &proxy)
{
  int ret = OB_SUCCESS;
  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(timer_.init("AIColumnWorker", ObMemAttr("AIColumnTimer")))) {
  } else if (OB_FAIL(timer_.start())) {
  } else {
    proxy_ = &proxy;
    stopped_ = false;
    inited_ = true;
    if (OB_FAIL(timer_.schedule(*this, AI_COLUMN_POLL_INTERVAL_US, true, true))) {
      stop();
    }
  }
  return ret;
}

void AIColumnWorker::stop()
{
  stopped_ = true;
  if (timer_.inited()) {
    timer_.stop();
    timer_.wait();
  }
}

void AIColumnWorker::destroy()
{
  stop();
  if (timer_.inited()) { timer_.destroy(); }
  proxy_ = nullptr;
  inited_ = false;
}

void AIColumnWorker::runTimerTask()
{
  if (!stopped_) {
    int ret = process_one();
    if (OB_FAIL(ret) && REACH_TIME_INTERVAL(10L * 1000L * 1000L)) {
      LOG_WARN("AI column worker poll failed", K(ret));
    }
  }
}

int AIColumnWorker::pick_group(ObIAllocator &allocator, AIColumnWorkerGroup &group)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("SELECT table_id,request_id FROM %s.%s WHERE ",
      OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME))) {
  } else if (OB_FAIL(sql.append_fmt(task_where(), ObTimeUtility::current_time()))) {
  } else if (OB_FAIL(sql.append(" ORDER BY gmt_create,request_id LIMIT 1"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else if (OB_FAIL(copy_result_id(*result, 0, group.table_id_))) {
      } else if (OB_FAIL(copy_result_string(*result, 1, allocator, group.request_id_))) {
      }
    }
  }
  return ret;
}

int AIColumnWorker::claim_and_load(ObIAllocator &allocator, AIColumnWorkerGroup &group, bool &claimed)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  claimed = false;
  char owner[UuidCommon::LENGTH_UUID + 1] = {};
  ObMySQLTransaction trans;
  bool commit = false;
  ObSqlString sql;
  int64_t affected = 0;
  if (OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(ObExprUuid::gen_server_uuid(owner, UuidCommon::LENGTH_UUID))) {
  } else if (OB_FAIL(ob_write_string(allocator,
      ObString(UuidCommon::LENGTH_UUID, owner), group.owner_))) {
  } else if (OB_FAIL(trans.start(proxy_, false))) {
  } else if (OB_FAIL(sql.append_fmt("UPDATE %s.%s SET lease_owner=",
      OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME))) {
  } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.owner_, allocator))) {
  } else if (OB_FAIL(sql.append_fmt(",lease_expire_ts=%ld,lease_version=lease_version+1,state=1 "
      "WHERE table_id=%lu AND request_id=", ObTimeUtility::current_time() + AI_COLUMN_LEASE_US,
      group.table_id_))) {
  } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.request_id_, allocator))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(sql.append_fmt(task_where(), ObTimeUtility::current_time()))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
  } else if (0 == affected) {
    commit = true;
  } else if (OB_FAIL(load_tasks(trans, allocator, group))) {
  } else if (group.tasks_.empty()) {
    commit = true;
  } else {
    claimed = true;
    commit = true;
  }
  if (trans.is_started() && OB_SUCCESS != (tmp_ret = trans.end(commit && OB_SUCC(ret)))) {
    LOG_WARN("failed to end AI column claim transaction", K(tmp_ret));
    if (OB_SUCC(ret)) { ret = tmp_ret; }
  }
  if (OB_FAIL(ret)) {
    claimed = false;
  }
  return ret;
}

int AIColumnWorker::load_tasks(ObISQLClient &client, ObIAllocator &allocator,
    AIColumnWorkerGroup &group)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("SELECT table_id,column_id,request_id,schema_version,retry_count,"
      "rowkey_predicate,input_payload,result_spec FROM %s.%s WHERE table_id=%lu AND request_id=",
      OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME, group.table_id_))) {
  } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.request_id_, allocator))) {
  } else if (OB_FAIL(sql.append(" AND lease_owner="))) {
  } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.owner_, allocator))) {
  } else if (OB_FAIL(sql.append(" ORDER BY column_id"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(client.read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        AIColumnWorkerTask task;
        if (OB_FAIL(copy_result_id(*result, 0, task.table_id_))) {
        } else if (OB_FAIL(copy_result_id(*result, 1, task.column_id_))) {
        } else if (OB_FAIL(copy_result_string(*result, 2, allocator, task.request_id_))) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(3), task.schema_version_))) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(4), task.retry_count_))) {
        } else if (OB_FAIL(copy_result_string(*result, 5, allocator, task.rowkey_predicate_))) {
        } else if (OB_FAIL(copy_result_string(*result, 6, allocator, task.input_payload_))) {
        } else if (OB_FAIL(copy_result_string(*result, 7, allocator, task.result_spec_))) {
        } else {
          ret = group.tasks_.push_back(task);
        }
      }
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
    }
  }
  if (OB_FAIL(ret)) {
  }
  return ret;
}

int AIColumnWorker::execute_task(ObIAllocator &allocator, AIColumnWorkerTask &task)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  ObIJsonBase *spec = nullptr;
  ObIJsonBase *payload = nullptr;
  ObString function, model_name, system_prompt, input;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      share::server_service<query::ObIAiEndpointResolver>();
  if (OB_FAIL(GCTX.schema_service_->get_full_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(task.table_id_, table)) || nullptr == table) {
    ret = OB_SCHEMA_ERROR;
  } else if (OB_ISNULL(column = table->get_column_schema(task.column_id_))
      || !column->is_ai_column()) {
    ret = OB_SCHEMA_ERROR;
  } else {
    const AIColumnSchema &definition = *column->get_ai_column_schema();
    task.max_retries_ = definition.get_max_retries();
    if (task.schema_version_ != definition.get_schema_version()) {
      ret = OB_SCHEMA_EAGAIN;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, task.result_spec_,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, spec)) || nullptr == spec) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, task.input_payload_,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, payload)) || nullptr == payload) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(json_string(*spec, "function", function))
      || OB_FAIL(json_string(*spec, "model", model_name))
      || OB_FAIL(json_string(*spec, "system_prompt", system_prompt))) {
  } else if (payload->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    ObIJsonBase *input_node = static_cast<ObJsonObject *>(payload)->get_value("input");
    if (nullptr == input_node || input_node->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_DATA;
    } else {
      input = static_cast<ObJsonString *>(input_node)->get_str();
    }
  }
  ObJsonObject *config = nullptr;
  if (OB_SUCC(ret) && spec->json_type() == ObJsonNodeType::J_OBJECT) {
    ObIJsonBase *config_node = static_cast<ObJsonObject *>(spec)->get_value("config");
    if (nullptr != config_node && config_node->json_type() == ObJsonNodeType::J_OBJECT) {
      config = static_cast<ObJsonObject *>(config_node);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (input.empty() || OB_ISNULL(endpoint_resolver)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_name, allocator, endpoint))) {
  } else if (function == "AI_EMBED") {
    ObAIFuncModel model(allocator, *info, endpoint);
    ret = model.call_dense_embedding(input, config, task.result_);
  } else if (function == "AI_COMPLETE") {
    ObAIFuncIComplete *provider = nullptr;
    ObAIFuncClient client;
    ObArray<ObString> headers;
    ObJsonObject *body = nullptr;
    ObJsonObject *response = nullptr;
    ObIJsonBase *output = nullptr;
    if (OB_FAIL(ObAIFuncUtils::get_complete_provider(allocator, endpoint.get_provider(), provider))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, *info, endpoint, headers))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_complete_body(allocator, *info, endpoint,
        system_prompt, input, config, body))) {
    } else if (OB_FAIL(client.send_post(allocator, endpoint.get_url(), headers, body, response))) {
    } else if (OB_FAIL(provider->parse_output(allocator, response, output))) {
    } else {
      ret = ObAIFuncJsonUtils::print_json_to_str(allocator, output, task.result_);
    }
  } else {
    ret = OB_INVALID_DATA;
  }
  if (OB_SUCC(ret)) {
    ObSqlString validation;
    ret = AIColumnResultConverter::append_value(validation, *column, task.result_, allocator);
  }
  task.call_ret_ = ret;
  return OB_SUCCESS;
}

int AIColumnWorker::defer_group(AIColumnWorkerGroup &group)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator(ObMemAttr("AIColumnRetry"));
  int tmp_ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  bool commit = false;
  if (OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(trans.start(proxy_, false))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.tasks_.count(); ++i) {
      AIColumnWorkerTask &task = group.tasks_.at(i);
      const bool failed = OB_SUCCESS != task.call_ret_;
      const bool retry = failed && AIColumnTaskAction::CALL_MODEL
          == AIColumnTaskDecision(task.max_retries_).after_failure(task.retry_count_);
      ObSqlString sql;
      int64_t affected = 0;
      char error[64] = {};
      snprintf(error, sizeof(error), "model call failed: %d", task.call_ret_);
      if (OB_FAIL(sql.append_fmt("UPDATE %s.%s SET state=%d,retry_count=retry_count+%d,"
          "eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld MICROSECOND),lease_owner='',"
          "lease_expire_ts=0,error_message=", OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME,
          retry ? 3 : 0, retry ? 1 : 0, AI_COLUMN_RETRY_DELAY_US))) {
      } else if (failed && OB_FAIL(AIColumnResultConverter::append_literal(sql,
          ObString(error), allocator))) {
      } else if (!failed && OB_FAIL(sql.append("NULL"))) {
      } else if (OB_FAIL(sql.append_fmt(" WHERE table_id=%lu AND column_id=%lu AND request_id=",
          task.table_id_, task.column_id_))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, task.request_id_, allocator))) {
      } else if (OB_FAIL(sql.append(" AND lease_owner="))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.owner_, allocator))) {
      } else {
        ret = trans.write(sql.ptr(), affected);
      }
    }
    commit = OB_SUCC(ret);
  }
  if (trans.is_started() && OB_SUCCESS != (tmp_ret = trans.end(commit))) {
    if (OB_SUCC(ret)) { ret = tmp_ret; }
  }
  return ret;
}

int AIColumnWorker::publish_group(ObIAllocator &allocator, AIColumnWorkerGroup &group)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObMySQLTransaction trans;
  bool commit = false;
  const bool old_worker_flag = ObSQLUtils::is_ai_column_worker_thread();
  ObSQLUtils::set_ai_column_worker_thread(true);
  if (OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(GCTX.schema_service_->get_full_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(trans.start(proxy_, false))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.tasks_.count(); ++i) {
      AIColumnWorkerTask &task = group.tasks_.at(i);
      const ObTableSchema *table = nullptr;
      const ObDatabaseSchema *database = nullptr;
      const ObColumnSchemaV2 *column = nullptr;
      ObSqlString sql;
      int64_t affected = 0;
      if (OB_FAIL(guard.get_table_schema(task.table_id_, table))) {
      } else if (nullptr == table) {
        continue; // The table was dropped after its task committed; remove the orphan below.
      } else if (OB_FAIL(guard.get_database_schema(table->get_database_id(), database))) {
      } else if (nullptr == database) {
        continue;
      } else if (OB_ISNULL(column = table->get_column_schema(task.column_id_))
          || !column->is_ai_column()) {
        continue;
      } else if (OB_FAIL(sql.append("UPDATE "))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_identifier(sql, database->get_database_name_str()))) {
      } else if (OB_FAIL(sql.append("."))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_identifier(sql, table->get_table_name_str()))) {
      } else if (OB_FAIL(sql.append(" SET "))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_identifier(sql, column->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append("="))) {
      } else if (OB_SUCCESS == task.call_ret_) {
        ret = AIColumnResultConverter::append_value(sql, *column, task.result_, allocator);
      } else {
        ret = sql.append("NULL");
      }
      const AIColumnSchema *definition = OB_SUCC(ret) ? column->get_ai_column_schema() : nullptr;
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(definition)) {
        ret = OB_SCHEMA_ERROR;
      } else if (OB_FAIL(sql.append(","))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_identifier(sql,
          table->get_column_schema(definition->get_state_column_id())->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append_fmt("=%d WHERE %.*s AND ", OB_SUCCESS == task.call_ret_ ? 0 : 2,
          static_cast<int>(task.rowkey_predicate_.length()), task.rowkey_predicate_.ptr()))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_identifier(sql,
          table->get_column_schema(definition->get_request_column_id())->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append("="))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, task.request_id_, allocator))) {
      } else if (OB_FAIL(sql.append(" AND "))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_identifier(sql,
          table->get_column_schema(definition->get_result_spec_column_id())->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append_fmt("=%ld", task.schema_version_))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected))) {
      } else if (0 == affected) {
        LOG_INFO("stale AI column result was not published", K(task.table_id_),
            K(task.column_id_), K(task.request_id_));
      }
    }
    if (OB_SUCC(ret)) {
      ObSqlString sql;
      int64_t affected = 0;
      if (OB_FAIL(sql.append_fmt("DELETE FROM %s.%s WHERE table_id=%lu AND request_id=",
          OB_SYS_DATABASE_NAME, OB_ALL_AI_COLUMN_TASK_TNAME, group.table_id_))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.request_id_, allocator))) {
      } else if (OB_FAIL(sql.append(" AND lease_owner="))) {
      } else if (OB_FAIL(AIColumnResultConverter::append_literal(sql, group.owner_, allocator))) {
      } else {
        ret = trans.write(sql.ptr(), affected);
      }
    }
    commit = OB_SUCC(ret);
  }
  if (trans.is_started() && OB_SUCCESS != (tmp_ret = trans.end(commit))) {
    if (OB_SUCC(ret)) { ret = tmp_ret; }
  }
  ObSQLUtils::set_ai_column_worker_thread(old_worker_flag);
  return ret;
}

int AIColumnWorker::process_one()
{
  int ret = OB_SUCCESS;
  if (!inited_ || stopped_ || OB_ISNULL(proxy_)) {
    ret = OB_NOT_INIT;
  } else {
    ObArenaAllocator allocator(ObMemAttr("AIColumnWork"));
    AIColumnWorkerGroup group;
    bool claimed = false;
    if (OB_FAIL(pick_group(allocator, group))) {
      if (OB_ITER_END == ret) { ret = OB_SUCCESS; }

    } else if (OB_FAIL(claim_and_load(allocator, group, claimed))) {
    } else if (!claimed) {
    } else {
      bool need_defer = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < group.tasks_.count(); ++i) {
        AIColumnWorkerTask &task = group.tasks_.at(i);
        if (OB_FAIL(execute_task(allocator, task))) {
        } else if (OB_SUCCESS != task.call_ret_ && AIColumnTaskAction::CALL_MODEL
            == AIColumnTaskDecision(task.max_retries_).after_failure(task.retry_count_)) {
          need_defer = true;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (need_defer) {
        ret = defer_group(group);
      } else {
        ret = publish_group(allocator, group);
      }
    }
  }
  return ret;
}
} // namespace observer
} // namespace oceanbase
