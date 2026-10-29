/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX OBSERVER
#include "observer/ai_column/ob_ai_column_result_converter.h"
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include "common/json_type/ob_json_tree.h"
#include "query/api/query/vector/ob_vector_index_util.h"
#include "share/json/ob_json_helper.h"

namespace oceanbase
{
namespace observer
{
using namespace common;
using namespace share::schema;

int AIColumnResultConverter::append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    ret = name.ptr()[i] == '`' ? sql.append("``") : sql.append(name.ptr() + i, 1);
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}

int AIColumnResultConverter::append_literal(ObSqlString &sql, const ObString &text,
    ObIAllocator &allocator)
{
  ObObj value;
  value.set_varchar(text);
  value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  const int64_t length = text.length() * 2L + 256;
  char *buffer = static_cast<char *>(allocator.alloc(length));
  int64_t pos = 0;
  int ret = OB_SUCCESS;
  if (nullptr == buffer) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObObjCharacterUtil::print_safe_hex_represent_mysql(value, buffer, length, pos))) {
  } else {
    ret = sql.append(buffer, pos);
  }
  return ret;
}

int AIColumnResultConverter::append_value(ObSqlString &sql, const ObColumnSchemaV2 &column,
    const ObString &result, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  const ObObjType type = column.get_data_type();
  if (ob_is_integer_type(type)) {
    int64_t value = 0;
    const char *first = result.ptr();
    const char *last = result.ptr() + result.length();
    const auto parsed = std::from_chars(first, last, value, 10);
    if (result.empty() || parsed.ec != std::errc() || parsed.ptr != last) {
      ret = OB_INVALID_DATA;
    } else {
      ret = sql.append(result);
    }
  } else if (ob_is_numeric_type(type) || ob_is_decimal_int(type)) {
    ObString copy;
    char *end = nullptr;
    errno = 0;
    if (OB_FAIL(ob_write_string(allocator, result, copy, true))) {
    } else {
      const double value = std::strtod(copy.ptr(), &end);
      if (copy.empty() || end != copy.ptr() + copy.length() || errno == ERANGE || !std::isfinite(value)) {
        ret = OB_INVALID_DATA;
      } else {
        ret = sql.append(result);
      }
    }
  } else if (column.is_collection()) {
    int64_t dimension = 0;
    ObIJsonBase *root = nullptr;
    if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
        column.get_extended_type_info(), dimension)) || dimension <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, result,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root))) {
      ret = OB_INVALID_DATA;
    } else if (nullptr == root || root->json_type() != ObJsonNodeType::J_ARRAY
        || root->element_count() != static_cast<uint64_t>(dimension)) {
      ret = OB_INVALID_DATA;
    } else {
      share::ObJsonReaderHelper reader(allocator);
      for (int64_t i = 0; OB_SUCC(ret) && i < dimension; ++i) {
        ObIJsonBase *element = nullptr;
        float value = 0;
        if (OB_FAIL(root->get_array_element(i, element))) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(reader.get_float_value(element, value)) || !std::isfinite(value)) {
          ret = OB_INVALID_DATA;
        }
      }
    }
    if (OB_SUCC(ret)) { ret = append_literal(sql, result, allocator); }
  } else if (ob_is_json(type)) {
    ObIJsonBase *root = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, result,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root)) || nullptr == root) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(sql.append("CAST("))) {
    } else if (OB_FAIL(append_literal(sql, result, allocator))) {
    } else {
      ret = sql.append(" AS JSON)");
    }
  } else if (ob_is_string_tc(type)) {
    ret = append_literal(sql, result, allocator);
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}
} // namespace observer
} // namespace oceanbase
