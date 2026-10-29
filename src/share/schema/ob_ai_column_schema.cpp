/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "share/schema/ob_ai_column_schema.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
using namespace common;

AIColumnSchema::AIColumnSchema(ObIAllocator *allocator) : ObSchema(allocator)
{
  reset();
}

void AIColumnSchema::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  schema_version_ = 0;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  max_retries_ = 2;
  column_name_.reset();
  expression_.reset();
  normalized_expr_.reset();
  dependency_count_ = 0;
  dependency_ids_ = nullptr;
  ObSchema::reset();
}

bool AIColumnSchema::is_valid() const
{
  return ObSchema::is_valid() && table_id_ != OB_INVALID_ID && column_id_ != OB_INVALID_ID
      && state_column_id_ != OB_INVALID_ID && request_column_id_ != OB_INVALID_ID
      && result_spec_column_id_ != OB_INVALID_ID && max_retries_ >= 0
      && !column_name_.empty() && !expression_.empty() && !normalized_expr_.empty();
}

int64_t AIColumnSchema::get_convert_size() const
{
  return sizeof(*this) + column_name_.length() + expression_.length()
      + normalized_expr_.length() + 3 + dependency_count_ * sizeof(uint64_t);
}

int AIColumnSchema::set_max_retries(const int64_t retries)
{
  const int ret = retries < 0 ? OB_INVALID_ARGUMENT : OB_SUCCESS;
  if (OB_SUCCESS == ret) {
    max_retries_ = retries;
  }
  return ret;
}

int AIColumnSchema::set_dependency_ids(const ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  uint64_t *copy = nullptr;
  if (ids.count() > OB_USER_ROW_MAX_COLUMNS_COUNT) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ids.count() > 0) {
    copy = static_cast<uint64_t *>(alloc(sizeof(uint64_t) * ids.count()));
    if (nullptr == copy) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; i < ids.count(); ++i) {
        copy[i] = ids.at(i);
      }
    }
  }
  if (OB_SUCC(ret)) {
    free(dependency_ids_);
    dependency_ids_ = copy;
    dependency_count_ = ids.count();
  }
  return ret;
}

int AIColumnSchema::assign(const AIColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    reset();
    ObSEArray<uint64_t, 8> ids;
    for (int64_t i = 0; OB_SUCC(ret) && i < other.dependency_count_; ++i) {
      if (OB_FAIL(ids.push_back(other.dependency_ids_[i]))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(set_column_name(other.column_name_))) {
    } else if (OB_FAIL(set_expression(other.expression_))) {
    } else if (OB_FAIL(set_normalized_expr(other.normalized_expr_))) {
    } else if (OB_FAIL(set_dependency_ids(ids))) {
    } else {
      table_id_ = other.table_id_;
      column_id_ = other.column_id_;
      schema_version_ = other.schema_version_;
      state_column_id_ = other.state_column_id_;
      request_column_id_ = other.request_column_id_;
      result_spec_column_id_ = other.result_spec_column_id_;
      max_retries_ = other.max_retries_;
    }
    error_ret_ = OB_SUCC(ret) ? other.error_ret_ : ret;
    ret = error_ret_;
  }
  return ret;
}

int AIColumnSchema::remap_column_ids(const ObIArray<uint64_t> &old_ids,
                                    const ObIArray<uint64_t> &new_ids)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 8> mapped;
  if (old_ids.count() != new_ids.count()) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < old_ids.count(); ++i) {
    if (new_ids.at(i) == OB_INVALID_ID) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (old_ids.at(j) == old_ids.at(i) || new_ids.at(j) == new_ids.at(i)) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
  }
  const uint64_t columns[] = {column_id_, state_column_id_, request_column_id_, result_spec_column_id_};
  for (int64_t i = 0; OB_SUCC(ret) && i < 4 + dependency_count_; ++i) {
    const uint64_t id = i < 4 ? columns[i] : dependency_ids_[i - 4];
    int64_t found = -1;
    for (int64_t j = 0; found < 0 && j < old_ids.count(); ++j) {
      if (old_ids.at(j) == id) {
        found = j;
      }
    }
    if (found < 0) {
      ret = OB_ENTRY_NOT_EXIST;
    } else if (OB_FAIL(mapped.push_back(new_ids.at(found)))) {
    }
  }
  ObSEArray<uint64_t, 8> deps;
  for (int64_t i = 4; OB_SUCC(ret) && i < mapped.count(); ++i) {
    if (OB_FAIL(deps.push_back(mapped.at(i)))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(set_dependency_ids(deps))) {
  } else {
    column_id_ = mapped.at(0);
    state_column_id_ = mapped.at(1);
    request_column_id_ = mapped.at(2);
    result_spec_column_id_ = mapped.at(3);
  }
  return ret;
}

int AIColumnSchema::serialize_to_hex(ObIAllocator &allocator, ObString &encoded) const
{
  int ret = OB_SUCCESS;
  encoded.reset();
  const int64_t size = get_serialize_size();
  char *buffer = nullptr;
  int64_t pos = 0;
  int64_t hex_pos = 0;
  if (!is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (size > INT32_MAX / 2) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_ISNULL(buffer = static_cast<char *>(allocator.alloc(3 * size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(serialize(buffer, size, pos))) {
  } else if (OB_FAIL(hex_print(buffer, pos, buffer + size, 2 * size, hex_pos))) {
  } else {
    encoded.assign_ptr(buffer + size, static_cast<int32_t>(hex_pos));
  }
  return ret;
}

int AIColumnSchema::deserialize_from_hex(const ObString &encoded)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColMeta");
  AIColumnSchema decoded(&allocator);
  char *buffer = nullptr;
  int64_t pos = 0;
  const int64_t size = encoded.length() / 2;
  if (encoded.empty() || encoded.length() % 2 != 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buffer = static_cast<char *>(allocator.alloc(size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (encoded.length() != str_to_hex(encoded.ptr(), encoded.length(), buffer, size)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(decoded.deserialize(buffer, size, pos))) {
  } else if (pos != size || !decoded.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(assign(decoded))) {
  }
  return ret;
}

OB_DEF_SERIALIZE(AIColumnSchema)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, table_id_, column_id_, schema_version_,
      state_column_id_, request_column_id_, result_spec_column_id_, max_retries_,
      column_name_, expression_, normalized_expr_, dependency_count_);
  for (int64_t i = 0; OB_SUCC(ret) && i < dependency_count_; ++i) {
    OB_UNIS_ENCODE(dependency_ids_[i]);
  }
  return ret;
}

OB_DEF_DESERIALIZE(AIColumnSchema)
{
  int ret = OB_SUCCESS;
  AIColumnSchema decoded;
  ObString name;
  ObString expression;
  ObString normalized;
  int64_t count = 0;
  LST_DO_CODE(OB_UNIS_DECODE, decoded.table_id_, decoded.column_id_, decoded.schema_version_,
      decoded.state_column_id_, decoded.request_column_id_, decoded.result_spec_column_id_,
      decoded.max_retries_, name, expression, normalized, count);
  if (OB_SUCC(ret) && (count < 0 || count > OB_USER_ROW_MAX_COLUMNS_COUNT
      || count > data_len - pos || decoded.max_retries_ < 0)) {
    ret = OB_INVALID_ARGUMENT;
  }
  ObSEArray<uint64_t, 8> deps;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    uint64_t id = OB_INVALID_ID;
    OB_UNIS_DECODE(id);
    if (OB_SUCC(ret) && OB_FAIL(deps.push_back(id))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(decoded.set_column_name(name))) {
  } else if (OB_FAIL(decoded.set_expression(expression))) {
  } else if (OB_FAIL(decoded.set_normalized_expr(normalized))) {
  } else if (OB_FAIL(decoded.set_dependency_ids(deps))) {
  } else if (OB_FAIL(assign(decoded))) {
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(AIColumnSchema)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, table_id_, column_id_, schema_version_,
      state_column_id_, request_column_id_, result_spec_column_id_, max_retries_,
      column_name_, expression_, normalized_expr_, dependency_count_);
  for (int64_t i = 0; i < dependency_count_; ++i) {
    OB_UNIS_ADD_LEN(dependency_ids_[i]);
  }
  return len;
}
} // namespace schema
} // namespace share
} // namespace oceanbase
