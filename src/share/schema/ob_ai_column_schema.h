/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
// The definition is owned by its schema allocator, never by a parser arena.
class AIColumnSchema : public ObSchema
{
  OB_UNIS_VERSION_V(1);
public:
  explicit AIColumnSchema(common::ObIAllocator *allocator = nullptr);
  AIColumnSchema(const AIColumnSchema &) = delete;
  AIColumnSchema &operator=(const AIColumnSchema &) = delete;
  int assign(const AIColumnSchema &other);
  void reset() override;
  bool is_valid() const override;
  int64_t get_convert_size() const override;
  int serialize_to_hex(common::ObIAllocator &allocator, common::ObString &encoded) const;
  int deserialize_from_hex(const common::ObString &encoded);

  void set_table_id(uint64_t id) { table_id_ = id; }
  void set_column_id(uint64_t id) { column_id_ = id; }
  void set_schema_version(int64_t version) { schema_version_ = version; }
  void set_state_column_id(uint64_t id) { state_column_id_ = id; }
  void set_request_column_id(uint64_t id) { request_column_id_ = id; }
  void set_result_spec_column_id(uint64_t id) { result_spec_column_id_ = id; }
  int set_max_retries(int64_t retries);
  int set_column_name(const common::ObString &name) { return deep_copy_str(name, column_name_); }
  int set_expression(const common::ObString &expr) { return deep_copy_str(expr, expression_); }
  int set_normalized_expr(const common::ObString &expr) { return deep_copy_str(expr, normalized_expr_); }
  int set_dependency_ids(const common::ObIArray<uint64_t> &ids);
  int remap_column_ids(const common::ObIArray<uint64_t> &old_ids,
                       const common::ObIArray<uint64_t> &new_ids);

  uint64_t get_table_id() const { return table_id_; }
  uint64_t get_column_id() const { return column_id_; }
  int64_t get_schema_version() const { return schema_version_; }
  uint64_t get_state_column_id() const { return state_column_id_; }
  uint64_t get_request_column_id() const { return request_column_id_; }
  uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  int64_t get_max_retries() const { return max_retries_; }
  const common::ObString &get_column_name() const { return column_name_; }
  const common::ObString &get_expression() const { return expression_; }
  const common::ObString &get_normalized_expr() const { return normalized_expr_; }
  int64_t get_dependency_count() const { return dependency_count_; }
  const uint64_t *get_dependency_ids() const { return dependency_ids_; }

private:
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t schema_version_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  int64_t max_retries_;
  common::ObString column_name_;
  common::ObString expression_;
  common::ObString normalized_expr_;
  int64_t dependency_count_;
  uint64_t *dependency_ids_;
};
} // namespace schema
} // namespace share
} // namespace oceanbase
