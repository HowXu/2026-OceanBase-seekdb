/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "lib/task/ob_timer.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace observer
{
struct AIColumnWorkerTask
{
  AIColumnWorkerTask() : table_id_(0), column_id_(0), schema_version_(0), retry_count_(0),
      max_retries_(0), call_ret_(0) {}
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t schema_version_;
  int64_t retry_count_;
  int64_t max_retries_;
  int call_ret_;
  common::ObString request_id_;
  common::ObString rowkey_predicate_;
  common::ObString input_payload_;
  common::ObString result_spec_;
  common::ObString result_;
};

struct AIColumnWorkerGroup
{
  AIColumnWorkerGroup() : table_id_(0), tasks_() {}
  uint64_t table_id_;
  common::ObString request_id_;
  common::ObString owner_;
  common::ObSEArray<AIColumnWorkerTask, 4> tasks_;
};

class AIColumnWorker final : public common::ObTimerTask
{
public:
  AIColumnWorker();
  ~AIColumnWorker() override;
  int start(common::ObMySQLProxy &proxy);
  void stop();
  void destroy();
  void runTimerTask() override;
  int process_one();
private:
  int pick_group(common::ObIAllocator &allocator, AIColumnWorkerGroup &group);
  int claim_and_load(common::ObIAllocator &allocator, AIColumnWorkerGroup &group, bool &claimed);
  int load_tasks(common::ObISQLClient &client, common::ObIAllocator &allocator,
      AIColumnWorkerGroup &group);
  int execute_task(common::ObIAllocator &allocator, AIColumnWorkerTask &task);
  int defer_group(AIColumnWorkerGroup &group);
  int publish_group(common::ObIAllocator &allocator, AIColumnWorkerGroup &group);
private:
  common::ObTimer timer_;
  common::ObMySQLProxy *proxy_;
  bool inited_;
  volatile bool stopped_;
private:
  DISALLOW_COPY_AND_ASSIGN(AIColumnWorker);
};
} // namespace observer
} // namespace oceanbase
