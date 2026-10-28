/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sql/engine/basic/ob_file_parquet_scan.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/file_sql_file_access.h"
#include "sql/engine/basic/file_sql_memory.h"

namespace oceanbase {
namespace sql {
namespace file_sql {

namespace {

// the magic bytes found at the beginning and at the end of a parquet file
const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};

// thrift compact protocol type ids
enum CompactType {
  COMPACT_STOP = 0x00,
  COMPACT_BOOLEAN_TRUE = 0x01,
  COMPACT_BOOLEAN_FALSE = 0x02,
  COMPACT_BYTE = 0x03,
  COMPACT_I16 = 0x04,
  COMPACT_I32 = 0x05,
  COMPACT_I64 = 0x06,
  COMPACT_DOUBLE = 0x07,
  COMPACT_BINARY = 0x08,
  COMPACT_LIST = 0x09,
  COMPACT_SET = 0x0A,
  COMPACT_MAP = 0x0B,
  COMPACT_STRUCT = 0x0C,
};

// parquet physical types
enum ParquetType {
  PARQUET_BOOLEAN = 0,
  PARQUET_INT32 = 1,
  PARQUET_INT64 = 2,
  PARQUET_INT96 = 3,
  PARQUET_FLOAT = 4,
  PARQUET_DOUBLE = 5,
  PARQUET_BYTE_ARRAY = 6,
  PARQUET_FIXED_LEN_BYTE_ARRAY = 7,
};

// parquet repetition types
enum ParquetRepetition {
  PARQUET_REQUIRED = 0,
  PARQUET_OPTIONAL = 1,
  PARQUET_REPEATED = 2,
};

// parquet encodings
enum ParquetEncoding {
  ENCODING_PLAIN = 0,
  ENCODING_PLAIN_DICTIONARY = 2,
  ENCODING_RLE = 3,
  ENCODING_BIT_PACKED = 4,
  ENCODING_DELTA_BINARY_PACKED = 5,
  ENCODING_RLE_DICTIONARY = 8,
};

// parquet compression codecs
enum ParquetCodec {
  CODEC_UNCOMPRESSED = 0,
  CODEC_SNAPPY = 1,
  CODEC_GZIP = 2,
};

// parquet page types
enum ParquetPageType {
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3,
};

// converted types that refine the physical type
enum ConvertedType {
  CONVERTED_DATE = 6,
  CONVERTED_TIMESTAMP_MILLIS = 9,
  CONVERTED_TIMESTAMP_MICROS = 10,
  CONVERTED_UINT_8 = 11,
  CONVERTED_UINT_16 = 12,
  CONVERTED_UINT_32 = 13,
  CONVERTED_UINT_64 = 14,
};

// logical types that refine the physical type
enum LogicalType {
  LOGICAL_NONE = 0,
  LOGICAL_DATE = 1,
  LOGICAL_TIMESTAMP = 2,
};

// time unit of a logical timestamp
enum TimeUnit {
  TIME_MILLIS = 0,
  TIME_MICROS = 1,
  TIME_NANOS = 2,
};

/**
 * @brief Raised when a buffer ends in the middle of a thrift structure.
 *
 * It tells a truncated buffer apart from a malformed one, the caller may
 * read more bytes and try again.
 */
struct Truncated {};

/**
 * @brief Reader of the thrift compact protocol.
 */
class CompactReader
{
public:
  CompactReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0) {}

  size_t position() const { return pos_; }
  size_t remaining() const { return size_ - pos_; }

  uint8_t read_byte()
  {
    if ((read_calls_++ & 4095) == 0) check_work();
    if (pos_ >= size_) {
      throw Truncated();
    }
    return data_[pos_++];
  }

  /**
   * @brief Read an unsigned variable length integer.
   */
  uint64_t read_varint()
  {
    uint64_t value = 0;
    int shift = 0;
    while (true) {
      uint8_t byte = read_byte();
      if (shift == 63 && (byte & 0xfe) != 0) {
        throw Error("corrupt thrift variable length integer");
      }
      if (shift == 63 && (byte & 0xfe) != 0) return false;
    value |= (uint64_t)(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        throw Error("corrupt thrift variable length integer");
      }
    }
    return value;
  }

  /**
   * @brief Read a zigzag encoded signed variable length integer.
   */
  int64_t read_zigzag()
  {
    uint64_t raw = read_varint();
    return (int64_t)(raw >> 1) ^ -(int64_t)(raw & 1);
  }

  int32_t read_i32()
  {
    const int64_t value = read_zigzag();
    if (value < INT32_MIN || value > INT32_MAX) throw Error("parquet integer overflow");
    return static_cast<int32_t>(value);
  }

  uint32_t read_collection_size(size_t minimum_bytes = 1)
  {
    const uint64_t count = read_varint();
    if (count > UINT32_MAX || count > remaining() / minimum_bytes) {
      throw Error("parquet collection size limit exceeded");
    }
    return static_cast<uint32_t>(count);
  }

  void enter_skip()
  {
    if (skip_depth_ == 64) throw Error("parquet nesting limit exceeded");
    ++skip_depth_;
  }
  void leave_skip() { --skip_depth_; }

  /**
   * @brief Read a length prefixed byte string.
   */
  BudgetString read_binary()
  {
    uint64_t len = read_varint();
    if (len > remaining()) {
      throw Truncated();
    }
    BudgetString out(reinterpret_cast<const char *>(data_ + pos_), (size_t)len);
    pos_ += (size_t)len;
    return out;
  }

  void read_bytes(uint8_t *out, size_t len)
  {
    if (len > remaining()) {
      throw Truncated();
    }
    memcpy(out, data_ + pos_, len);
    pos_ += len;
  }

  void skip_bytes(size_t len)
  {
    if (len > remaining()) {
      throw Truncated();
    }
    pos_ += len;
  }

  double read_double()
  {
    uint8_t buf[8];
    read_bytes(buf, 8);
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
      bits |= (uint64_t)buf[i] << (8 * i);
    }
    double value = 0;
    memcpy(&value, &bits, sizeof(value));
    return value;
  }

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  size_t skip_depth_ = 0;
  size_t read_calls_ = 0;
};

/**
 * @brief Header of one field of a thrift struct.
 */
struct CompactField {
  int id = 0;
  uint8_t type = COMPACT_STOP;
  bool bool_value = false;
};

/**
 * @brief Reader of the fields of one thrift struct.
 */
class CompactStructReader
{
public:
  explicit CompactStructReader(CompactReader &reader) : reader_(reader), last_id_(0) {}

  /**
   * @brief Read the header of the next field.
   * @return false when the end of the struct is reached
   */
  bool next_field(CompactField &field)
  {
    uint8_t header = reader_.read_byte();
    if (header == COMPACT_STOP) {
      return false;
    }
    uint8_t type = header & 0x0F;
    int delta = (header & 0xF0) >> 4;
    field.type = type;
    if (type == COMPACT_BOOLEAN_TRUE) {
      field.bool_value = true;
    } else if (type == COMPACT_BOOLEAN_FALSE) {
      field.bool_value = false;
    }
    if (delta == 0) {
      field.id = reader_.read_i32();
    } else {
      field.id = last_id_ + delta;
    }
    if (field.id <= 0 || field.id > INT16_MAX) throw Error("corrupt parquet field id");
    last_id_ = field.id;
    return true;
  }

private:
  CompactReader &reader_;
  int last_id_;
};

void skip_value(CompactReader &reader, uint8_t type);

void skip_struct(CompactReader &reader)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    skip_value(reader, field.type);
  }
}

void skip_value(CompactReader &reader, uint8_t type)
{
  struct DepthGuard {
    CompactReader &reader;
    explicit DepthGuard(CompactReader &value) : reader(value) { reader.enter_skip(); }
    ~DepthGuard() { reader.leave_skip(); }
  } depth(reader);
  switch (type) {
    case COMPACT_BOOLEAN_TRUE:
    case COMPACT_BOOLEAN_FALSE: {
      // the value is carried by the field header
      break;
    }
    case COMPACT_BYTE: {
      reader.read_byte();
      break;
    }
    case COMPACT_I16:
    case COMPACT_I32:
    case COMPACT_I64: {
      reader.read_varint();
      break;
    }
    case COMPACT_DOUBLE: {
      reader.skip_bytes(8);
      break;
    }
    case COMPACT_BINARY: {
      reader.read_binary();
      break;
    }
    case COMPACT_LIST:
    case COMPACT_SET: {
      uint8_t header = reader.read_byte();
      uint32_t size = header >> 4;
      uint8_t element_type = header & 0x0F;
      if (size == 15) {
        size = reader.read_collection_size();
      }
      check_count(size, reader.remaining(), "parquet collection size");
      for (uint32_t i = 0; i < size; ++i) {
        if (element_type == COMPACT_BOOLEAN_TRUE || element_type == COMPACT_BOOLEAN_FALSE) {
          reader.read_byte();
        } else {
          skip_value(reader, element_type);
        }
      }
      break;
    }
    case COMPACT_MAP: {
      uint64_t size = reader.read_collection_size(2);
      if (size > 0) {
        uint8_t types = reader.read_byte();
        uint8_t key_type = types >> 4;
        uint8_t value_type = types & 0x0F;
        for (uint64_t i = 0; i < size; ++i) {
          for (const auto kind : {key_type, value_type}) {
            if (kind == COMPACT_BOOLEAN_TRUE || kind == COMPACT_BOOLEAN_FALSE) {
              const auto value = reader.read_byte();
              if (value != COMPACT_BOOLEAN_TRUE && value != COMPACT_BOOLEAN_FALSE) throw Error("corrupt parquet boolean");
            } else {
              skip_value(reader, kind);
            }
          }
        }
      }
      break;
    }
    case COMPACT_STRUCT: {
      skip_struct(reader);
      break;
    }
    default: {
      throw Error("unknown thrift compact type");
    }
  }
}

/**
 * @brief Read the header of a list.
 */
void read_list_header(CompactReader &reader, uint32_t &size, uint8_t &element_type)
{
  uint8_t header = reader.read_byte();
  size = header >> 4;
  element_type = header & 0x0F;
  if (size == 15) {
    size = reader.read_collection_size();
  }
  check_count(size, reader.remaining(), "parquet collection size");
}

/**
 * @brief Number of bits needed to store the values from 0 to max.
 */
int bit_width(int max)
{
  int width = 0;
  while (max > 0) {
    width++;
    max >>= 1;
  }
  return width;
}

/**
 * @brief Read a little endian integer of the given size.
 */
uint64_t read_le(const uint8_t *data, int size)
{
  uint64_t value = 0;
  for (int i = 0; i < size; ++i) {
    value |= (uint64_t)data[i] << (8 * i);
  }
  return value;
}

/**
 * @brief One element of the schema of a parquet file.
 */
struct SchemaElement {
  BudgetString name;
  int type = -1;
  int type_length = 0;
  int repetition_type = PARQUET_REQUIRED;
  int num_children = 0;
  int converted_type = -1;
  int logical_type = LOGICAL_NONE;
  int time_unit = TIME_MILLIS;
};

/**
 * @brief Metadata of one column chunk.
 */
struct ColumnMetaData {
  int type = -1;
  int codec = CODEC_UNCOMPRESSED;
  int64_t num_values = 0;
  int64_t total_compressed_size = 0;
  int64_t total_uncompressed_size = 0;
  int64_t data_page_offset = 0;
  int64_t dictionary_page_offset = -1;
  BudgetVector<BudgetString> path_in_schema;
};

/**
 * @brief One column chunk of a row group.
 */
struct ColumnChunk {
  BudgetString file_path;
  int64_t file_offset = 0;
  bool has_metadata = false;
  ColumnMetaData metadata;
};

/**
 * @brief One row group of the file.
 */
struct RowGroup {
  int64_t num_rows = 0;
  BudgetVector<ColumnChunk> columns;
};

/**
 * @brief The metadata of the whole file.
 */
struct FileMetaData {
  int64_t num_rows = 0;
  BudgetVector<SchemaElement> schema;
  BudgetVector<RowGroup> row_groups;
};

/**
 * @brief One leaf column of the file.
 */
struct LeafColumn {
  BudgetString name;
  BudgetVector<BudgetString> path;
  Type type = Type::Varchar;
  bool nullable = false;
  int parquet_type = PARQUET_INT64;
  int type_length = 0;
  int converted_type = -1;
  int logical_type = LOGICAL_NONE;
  int time_unit = TIME_MILLIS;
  int max_def_level = 0;
  int max_rep_level = 0;
};

/**
 * @brief Parse the time unit of a logical timestamp.
 */
void parse_time_unit(CompactReader &reader, int &unit)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        unit = TIME_MILLIS;
        skip_value(reader, field.type);
        break;
      }
      case 2: {
        unit = TIME_MICROS;
        skip_value(reader, field.type);
        break;
      }
      case 3: {
        unit = TIME_NANOS;
        skip_value(reader, field.type);
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse the logical type of a schema element.
 */
void parse_logical_type(CompactReader &reader, SchemaElement &element)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 6: {
        // DateType, the only field is a boolean carried by the header
        element.logical_type = LOGICAL_DATE;
        break;
      }
      case 8: {
        // TimestampType, the second field holds the time unit
        element.logical_type = LOGICAL_TIMESTAMP;
        CompactStructReader members(reader);
        CompactField member;
        while (members.next_field(member)) {
          if (member.id == 2) {
            parse_time_unit(reader, element.time_unit);
          } else {
            skip_value(reader, member.type);
          }
        }
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse one element of the schema.
 */
void parse_schema_element(CompactReader &reader, SchemaElement &element)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        element.type = reader.read_i32();
        break;
      }
      case 2: {
        element.type_length = reader.read_i32();
        break;
      }
      case 3: {
        element.repetition_type = reader.read_i32();
        break;
      }
      case 4: {
        element.name = reader.read_binary();
        break;
      }
      case 5: {
        element.num_children = reader.read_i32();
        break;
      }
      case 6: {
        element.converted_type = reader.read_i32();
        break;
      }
      case 7: {
        skip_value(reader, field.type);
        break;
      }
      case 8: {
        skip_value(reader, field.type);
        break;
      }
      case 10: {
        parse_logical_type(reader, element);
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
  if (element.converted_type == CONVERTED_TIMESTAMP_MICROS) {
    element.time_unit = TIME_MICROS;
  } else if (element.converted_type == CONVERTED_TIMESTAMP_MILLIS) {
    element.time_unit = TIME_MILLIS;
  }
}

/**
 * @brief Parse the metadata of one column chunk.
 */
void parse_column_metadata(CompactReader &reader, ColumnMetaData &metadata)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        metadata.type = reader.read_i32();
        break;
      }
      case 2: {
        uint32_t size = 0;
        uint8_t element_type = COMPACT_STOP;
        read_list_header(reader, size, element_type);
        for (uint32_t i = 0; i < size; ++i) {
          skip_value(reader, element_type);
        }
        break;
      }
      case 3: {
        uint32_t size = 0;
        uint8_t element_type = COMPACT_STOP;
        read_list_header(reader, size, element_type);
        for (uint32_t i = 0; i < size; ++i) {
          if (element_type == COMPACT_BINARY) {
            metadata.path_in_schema.push_back(reader.read_binary());
          } else {
            skip_value(reader, element_type);
          }
        }
        break;
      }
      case 4: {
        metadata.codec = reader.read_i32();
        break;
      }
      case 5: {
        metadata.num_values = reader.read_zigzag();
        break;
      }
      case 6: {
        metadata.total_uncompressed_size = reader.read_zigzag();
        break;
      }
      case 7: {
        metadata.total_compressed_size = reader.read_zigzag();
        break;
      }
      case 9: {
        metadata.data_page_offset = reader.read_zigzag();
        break;
      }
      case 10: {
        skip_value(reader, field.type);
        break;
      }
      case 11: {
        metadata.dictionary_page_offset = reader.read_zigzag();
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse one column chunk.
 */
void parse_column_chunk(CompactReader &reader, ColumnChunk &chunk)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        chunk.file_path = reader.read_binary();
        break;
      }
      case 2: {
        chunk.file_offset = reader.read_zigzag();
        break;
      }
      case 3: {
        parse_column_metadata(reader, chunk.metadata);
        chunk.has_metadata = true;
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse one row group.
 */
void parse_row_group(CompactReader &reader, RowGroup &row_group)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        uint32_t size = 0;
        uint8_t element_type = COMPACT_STOP;
        read_list_header(reader, size, element_type);
        check_count(size, MAX_COLUMNS, "parquet row group column");
        for (uint32_t i = 0; i < size; ++i) {
          ColumnChunk chunk;
          if (element_type == COMPACT_STRUCT) {
            parse_column_chunk(reader, chunk);
          } else {
            skip_value(reader, element_type);
          }
          row_group.columns.push_back(chunk);
        }
        break;
      }
      case 3: {
        row_group.num_rows = reader.read_zigzag();
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse the metadata of the file.
 */
void parse_file_metadata(CompactReader &reader, FileMetaData &metadata)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 2: {
        uint32_t size = 0;
        uint8_t element_type = COMPACT_STOP;
        read_list_header(reader, size, element_type);
        check_count(size, MAX_COLUMNS + 1, "parquet schema column");
        for (uint32_t i = 0; i < size; ++i) {
          SchemaElement element;
          if (element_type == COMPACT_STRUCT) {
            parse_schema_element(reader, element);
          } else {
            skip_value(reader, element_type);
          }
          metadata.schema.push_back(element);
        }
        break;
      }
      case 3: {
        metadata.num_rows = reader.read_zigzag();
        break;
      }
      case 4: {
        uint32_t size = 0;
        uint8_t element_type = COMPACT_STOP;
        read_list_header(reader, size, element_type);
        check_count(size, reader.remaining(), "parquet row group count");
        for (uint32_t i = 0; i < size; ++i) {
          RowGroup row_group;
          if (element_type == COMPACT_STRUCT) {
            parse_row_group(reader, row_group);
          } else {
            skip_value(reader, element_type);
          }
          metadata.row_groups.push_back(row_group);
        }
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Number of elements of the subtree rooted at every element.
 *
 * The schema is stored as a flat list in preorder, the children of an
 * element are the elements that follow it.
 */
BudgetVector<int> compute_subtree_sizes(const BudgetVector<SchemaElement> &elements)
{
  BudgetVector<int> sizes(elements.size(), 1);
  for (int i = (int)elements.size() - 1; i >= 0; --i) {
    sizes[i] = 1;
    int child = i + 1;
    for (int c = 0; c < elements[i].num_children; ++c) {
      if (child >= (int)elements.size()) {
        break;
      }
      sizes[i] += sizes[child];
      child += sizes[child];
    }
  }
  return sizes;
}

/**
 * @brief Split a dotted name into its parts.
 */
BudgetVector<BudgetString> split_path(const BudgetString &name)
{
  BudgetVector<BudgetString> parts;
  size_t start = 0;
  while (start <= name.size()) {
    size_t pos = name.find('.', start);
    if (pos == BudgetString::npos) {
      parts.push_back(name.substr(start));
      break;
    }
    parts.push_back(name.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

/**
 * @brief Map the physical type of a column to the type of the scanner.
 */
void map_column_type(LeafColumn &column)
{
  switch (column.parquet_type) {
    case PARQUET_BOOLEAN: {
      column.type = Type::Boolean;
      break;
    }
    case PARQUET_INT32: {
      if (column.converted_type == CONVERTED_DATE || column.logical_type == LOGICAL_DATE) {
        column.type = Type::Date;
      } else {
        column.type = Type::Bigint;
      }
      break;
    }
    case PARQUET_INT64: {
      if (column.converted_type == CONVERTED_TIMESTAMP_MILLIS || column.converted_type == CONVERTED_TIMESTAMP_MICROS
          || column.logical_type == LOGICAL_TIMESTAMP) {
        column.type = Type::Datetime;
      } else {
        column.type = Type::Bigint;
      }
      break;
    }
    case PARQUET_FLOAT:
    case PARQUET_DOUBLE: {
      column.type = Type::Double;
      break;
    }
    default: {
      column.type = Type::Varchar;
      break;
    }
  }
}

/**
 * @brief Collect the leaf columns of the schema, in leaf order.
 */
void collect_leaf_columns(const BudgetVector<SchemaElement> &elements,
                           const BudgetVector<int> &sizes,
                           int index,
                           const BudgetString &prefix,
                           int def_level,
                           int rep_level,
                           bool nullable,
                           BudgetVector<LeafColumn> &columns)
{
  const SchemaElement &element = elements[index];
  BudgetString name = prefix.empty() ? element.name : prefix + "." + element.name;
  int def = def_level;
  int rep = rep_level;
  bool is_nullable = nullable;
  if (element.repetition_type == PARQUET_OPTIONAL) {
    def++;
    is_nullable = true;
  } else if (element.repetition_type == PARQUET_REPEATED) {
    def++;
    rep++;
  }
  if (element.num_children == 0) {
    LeafColumn column;
    column.name = name;
    column.path = split_path(name);
    column.nullable = is_nullable;
    column.parquet_type = element.type;
    column.type_length = element.type_length;
    column.converted_type = element.converted_type;
    column.logical_type = element.logical_type;
    column.time_unit = element.time_unit;
    column.max_def_level = def;
    column.max_rep_level = rep;
    map_column_type(column);
    columns.push_back(column);
    return;
  }
  int child = index + 1;
  for (int c = 0; c < element.num_children; ++c) {
    if (child >= (int)elements.size()) {
      break;
    }
    collect_leaf_columns(elements, sizes, child, name, def, rep, is_nullable, columns);
    child += sizes[child];
  }
}

/**
 * @brief Decompress a snappy block.
 *
 * The block starts with the length of the uncompressed data, encoded as a
 * variable length integer, and continues with literal and copy elements.
 *
 * @return false when the block is malformed
 */
bool snappy_decompress(const uint8_t *src, size_t src_size, size_t expected_size, BudgetVector<uint8_t> &out)
{
  size_t pos = 0;
  uint64_t length = 0;
  int shift = 0;
  while (true) {
    if (pos >= src_size) {
      return false;
    }
    uint8_t byte = src[pos++];
    if (shift == 63 && (byte & 0xfe) != 0) return false;
    length |= (uint64_t)(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      break;
    }
    shift += 7;
    if (shift > 63) {
      return false;
    }
  }
  check_count(length, MAX_PARQUET_BYTES, "Snappy decompressed page");
  if (length != expected_size) throw Error("parquet decompressed page size mismatch");
  out.assign((size_t)length, 0);
  size_t written = 0;
  size_t checked = 0;
  while (pos < src_size) {
    if (written - checked >= 65536) {
      check_work();
      checked = written;
    }
    uint8_t tag = src[pos++];
    uint32_t type = tag & 0x03;
    if (type == 0) {
      uint64_t len = tag >> 2;
      if (len >= 60) {
        int extra = (int)(len - 59);
        if (pos + extra > src_size) {
          return false;
        }
        uint64_t value = 0;
        for (int i = 0; i < extra; ++i) {
          value |= (uint64_t)src[pos + i] << (8 * i);
        }
        pos += extra;
        len = value;
      }
      len += 1;
      if (len > src_size - pos || written + len > out.size()) {
        return false;
      }
      memcpy(out.data() + written, src + pos, (size_t)len);
      pos += (size_t)len;
      written += (size_t)len;
    } else {
      uint32_t len = 0;
      uint32_t offset = 0;
      if (type == 1) {
        len = 4 + ((tag >> 2) & 0x07);
        if (pos >= src_size) {
          return false;
        }
        offset = ((uint32_t)(tag >> 5) << 8) | src[pos++];
      } else if (type == 2) {
        len = (tag >> 2) + 1;
        if (pos + 2 > src_size) {
          return false;
        }
        offset = (uint32_t)src[pos] | ((uint32_t)src[pos + 1] << 8);
        pos += 2;
      } else {
        len = (tag >> 2) + 1;
        if (pos + 4 > src_size) {
          return false;
        }
        offset = (uint32_t)read_le(src + pos, 4);
        pos += 4;
      }
      if (offset == 0 || offset > written) {
        return false;
      }
      size_t start = written - offset;
      for (uint32_t i = 0; i < len; ++i) {
        if (written >= out.size()) {
          return false;
        }
        out[written++] = out[start + i];
      }
    }
  }
  return written == out.size();
}

/**
 * @brief Decompress the content of a page.
 */
BudgetVector<uint8_t> decompress_page(const uint8_t *data, size_t size, int codec, size_t expected_size)
{
  check_count(expected_size, MAX_PARQUET_BYTES, "decompressed parquet page");
  if (codec == CODEC_UNCOMPRESSED) {
    if (size != expected_size) throw Error("parquet decompressed page size mismatch");
    return BudgetVector<uint8_t>(data, data + size);
  }
  if (codec == CODEC_SNAPPY) {
    BudgetVector<uint8_t> out;
    if (!snappy_decompress(data, size, expected_size, out)) {
      throw Error("corrupt snappy compressed page");
    }
    return out;
  }
  if (codec == CODEC_GZIP) {
    throw Error("gzip compressed parquet files are not supported");
  }
  throw Error("unsupported parquet compression codec");
}

/**
 * @brief Reader of the bit packed values of the RLE / bit packing hybrid.
 */
class BitReader
{
public:
  BitReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0), buffer_(0), bits_(0) {}

  bool read_bits(int width, uint32_t &value)
  {
    if (width == 0) {
      value = 0;
      return true;
    }
    while (bits_ < width) {
      if (pos_ >= size_) {
        return false;
      }
      buffer_ |= (uint64_t)data_[pos_++] << bits_;
      bits_ += 8;
    }
    value = (uint32_t)(buffer_ & ((1ULL << width) - 1));
    buffer_ >>= width;
    bits_ -= width;
    return true;
  }

  size_t position() const { return pos_; }

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  uint64_t buffer_;
  int bits_;
};

/**
 * @brief Decode the RLE / bit packing hybrid encoding.
 *
 * @param data     the encoded bytes
 * @param size     the number of encoded bytes
 * @param width    the number of bits of a value
 * @param count    the number of values to decode
 * @param values   receives the decoded values
 * @param consumed receives the number of bytes that were read
 * @return false when the encoded bytes are malformed
 */
bool decode_hybrid(const uint8_t *data,
                   size_t size,
                   int width,
                   int count,
                   BudgetVector<int32_t> &values,
                   size_t &consumed)
{
  if (width < 0 || width > 32 || count < 0) {
    throw Error("invalid parquet hybrid width or value count");
  }
  values.clear();
  size_t pos = 0;
  BitReader reader(data, size);
  while ((int)values.size() < count) {
    if (pos >= size) {
      return false;
    }
    uint64_t header = 0;
    int shift = 0;
    while (true) {
      if (pos >= size) {
        return false;
      }
      uint8_t byte = data[pos++];
      if (shift == 63 && (byte & 0xfe) != 0) return false;
      header |= (uint64_t)(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        break;
      }
      shift += 7;
      if (shift > 63) {
        return false;
      }
    }
    if ((header & 1) == 0) {
      // a run of the same value
      uint64_t run = header >> 1;
      int bytes = (width + 7) / 8;
      if (pos + bytes > size) {
        return false;
      }
      uint32_t value = (uint32_t)read_le(data + pos, bytes);
      pos += bytes;
      for (uint64_t i = 0; i < run && (int)values.size() < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back((int32_t)value);
      }
    } else {
      // a run of bit packed values
      uint64_t groups = header >> 1;
      if (groups > UINT64_MAX / 8 || (width != 0 && groups > (size - pos) / width)) {
        return false;
      }
      uint64_t total = groups * 8;
      size_t bytes = static_cast<size_t>(groups * width);
      if (bytes > size - pos) {
        return false;
      }
      BitReader packed(data + pos, bytes);
      for (uint64_t i = 0; i < total && (int)values.size() < count; ++i) {
        if ((i & 4095) == 0) check_work();
        uint32_t value = 0;
        if (!packed.read_bits(width, value)) {
          return false;
        }
        values.push_back((int32_t)value);
      }
      pos += bytes;
    }
  }
  consumed = pos;
  return true;
}

/**
 * @brief Check that the decoded levels are within the range of the schema.
 */
bool levels_are_valid(const BudgetVector<int32_t> &levels, int max_level)
{
  for (size_t i = 0; i < levels.size(); ++i) {
    if (levels[i] < 0 || levels[i] > max_level) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Decode the definition or repetition levels of a page.
 *
 * The levels of a version 1 page are prefixed by their length, the levels of
 * a version 2 page are not. Both layouts are accepted, the prefixed one is
 * tried first.
 *
 * @return the number of bytes that were read
 */
size_t decode_levels(const uint8_t *data,
                     size_t size,
                     int width,
                     int max_level,
                     int count,
                     bool prefixed,
                     BudgetVector<int32_t> &levels)
{
  if (prefixed && size >= 4) {
    uint32_t len = (uint32_t)read_le(data, 4);
    if (len <= size - 4) {
      BudgetVector<int32_t> candidate;
      size_t consumed = 0;
      if (decode_hybrid(data + 4, len, width, count, candidate, consumed) && consumed <= len
          && levels_are_valid(candidate, max_level)) {
        levels.swap(candidate);
        return 4 + consumed;
      }
    }
  }
  size_t consumed = 0;
  if (!decode_hybrid(data, size, width, count, levels, consumed) || !levels_are_valid(levels, max_level)) {
    throw Error("corrupt parquet levels");
  }
  return consumed;
}

/**
 * @brief Format a double with the shortest representation that reads back
 *        to the same value.
 */
BudgetString format_double(double value)
{
  if (std::isnan(value)) {
    return "NaN";
  }
  if (std::isinf(value)) {
    return value > 0 ? "Infinity" : "-Infinity";
  }
  char buf[64];
  for (int precision = 1; precision <= 17; ++precision) {
    snprintf(buf, sizeof(buf), "%.*g", precision, value);
    if (strtod(buf, NULL) == value) {
      break;
    }
  }
  return BudgetString(buf);
}

/**
 * @brief Convert a number of days since the epoch into a calendar date.
 */
void civil_from_days(int64_t days, int64_t &year, int64_t &month, int64_t &day)
{
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = y + (month <= 2 ? 1 : 0);
}

/**
 * @brief Format a date as year-month-day.
 */
BudgetString format_date(int64_t days)
{
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  civil_from_days(days, year, month, day);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld", (long long)year, (long long)month, (long long)day);
  return BudgetString(buf);
}

/**
 * @brief Format a timestamp as year-month-day hour:minute:second.
 *
 * The fractional part is appended when it is not zero.
 */
BudgetString format_timestamp(int64_t value, int unit)
{
  int64_t divisor = 1000;
  int digits = 3;
  if (unit == TIME_MICROS) {
    divisor = 1000000;
    digits = 6;
  } else if (unit == TIME_NANOS) {
    divisor = 1000000000;
    digits = 9;
  }
  int64_t seconds = value / divisor;
  int64_t fraction = value % divisor;
  if (fraction < 0) {
    fraction += divisor;
    seconds -= 1;
  }
  int64_t days = seconds / 86400;
  int64_t seconds_of_day = seconds % 86400;
  if (seconds_of_day < 0) {
    seconds_of_day += 86400;
    days -= 1;
  }
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  civil_from_days(days, year, month, day);
  char buf[64];
  snprintf(buf,
           sizeof(buf),
           "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
           (long long)year,
           (long long)month,
           (long long)day,
           (long long)(seconds_of_day / 3600),
           (long long)((seconds_of_day % 3600) / 60),
           (long long)(seconds_of_day % 60));
  BudgetString result(buf);
  if (fraction != 0) {
    char frac[32];
    snprintf(frac, sizeof(frac), ".%0*lld", digits, (long long)fraction);
    result += frac;
  }
  return result;
}

/**
 * @brief Format the nanosecond timestamp of an int96 value.
 */
BudgetString format_int96(const uint8_t *data)
{
  uint64_t nanoseconds = read_le(data, 8);
  int32_t julian_day = (int32_t)read_le(data + 8, 4);
  int64_t days = (int64_t)julian_day - 2440588;
  int64_t seconds = (int64_t)(nanoseconds / 1000000000ULL);
  int64_t fraction = (int64_t)(nanoseconds % 1000000000ULL);
  int64_t seconds_of_day = seconds % 86400;
  if (seconds_of_day < 0) {
    seconds_of_day += 86400;
    days -= 1;
  }
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  civil_from_days(days, year, month, day);
  char buf[64];
  snprintf(buf,
           sizeof(buf),
           "%04lld-%02lld-%02lld %02lld:%02lld:%02lld",
           (long long)year,
           (long long)month,
           (long long)day,
           (long long)(seconds_of_day / 3600),
           (long long)((seconds_of_day % 3600) / 60),
           (long long)(seconds_of_day % 60));
  BudgetString result(buf);
  if (fraction != 0) {
    char frac[32];
    snprintf(frac, sizeof(frac), ".%09lld", (long long)fraction);
    result += frac;
  }
  return result;
}

/**
 * @brief Convert one raw value into its textual form.
 */
BudgetString format_value(const LeafColumn &column, const uint8_t *data, size_t size)
{
  switch (column.parquet_type) {
    case PARQUET_BOOLEAN: {
      return (data[0] & 1) ? "true" : "false";
    }
    case PARQUET_INT32: {
      int32_t value = (int32_t)read_le(data, 4);
      if (column.converted_type == CONVERTED_DATE || column.logical_type == LOGICAL_DATE) {
        return format_date(value);
      }
      if (column.converted_type == CONVERTED_UINT_8 || column.converted_type == CONVERTED_UINT_16
          || column.converted_type == CONVERTED_UINT_32) {
        return budget_to_string((uint32_t)value);
      }
      return budget_to_string((int64_t)value);
    }
    case PARQUET_INT64: {
      int64_t value = (int64_t)read_le(data, 8);
      if (column.converted_type == CONVERTED_TIMESTAMP_MILLIS
          || column.converted_type == CONVERTED_TIMESTAMP_MICROS || column.logical_type == LOGICAL_TIMESTAMP) {
        return format_timestamp(value, column.time_unit);
      }
      if (column.converted_type == CONVERTED_UINT_64) {
        return budget_to_string((uint64_t)value);
      }
      return budget_to_string(value);
    }
    case PARQUET_INT96: {
      return format_int96(data);
    }
    case PARQUET_FLOAT: {
      uint32_t bits = (uint32_t)read_le(data, 4);
      float value = 0;
      memcpy(&value, &bits, sizeof(value));
      return format_double((double)value);
    }
    case PARQUET_DOUBLE: {
      uint64_t bits = read_le(data, 8);
      double value = 0;
      memcpy(&value, &bits, sizeof(value));
      return format_double(value);
    }
    case PARQUET_BYTE_ARRAY:
    case PARQUET_FIXED_LEN_BYTE_ARRAY: {
      return BudgetString(reinterpret_cast<const char *>(data), size);
    }
    default: {
      return BudgetString(reinterpret_cast<const char *>(data), size);
    }
  }
}

/**
 * @brief Decode the values of a page that use the plain encoding.
 */
void decode_plain_values(const uint8_t *data,
                          size_t size,
                          const LeafColumn &column,
                          int count,
                          BudgetVector<BudgetString> &values)
{
  if (count < 0) throw Error("negative parquet value count");
  values.reserve(static_cast<size_t>(count));
  size_t pos = 0;
  switch (column.parquet_type) {
    case PARQUET_BOOLEAN: {
      if ((static_cast<size_t>(count) + 7) / 8 > size) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        uint8_t byte = data[i / 8];
        values.push_back((byte >> (i % 8)) & 1 ? "true" : "false");
      }
      break;
    }
    case PARQUET_INT32: {
      if (size < (size_t)count * 4) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back(format_value(column, data + pos, 4));
        pos += 4;
      }
      break;
    }
    case PARQUET_INT64: {
      if (size < (size_t)count * 8) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back(format_value(column, data + pos, 8));
        pos += 8;
      }
      break;
    }
    case PARQUET_INT96: {
      if (size < (size_t)count * 12) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back(format_value(column, data + pos, 12));
        pos += 12;
      }
      break;
    }
    case PARQUET_FLOAT: {
      if (size < (size_t)count * 4) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back(format_value(column, data + pos, 4));
        pos += 4;
      }
      break;
    }
    case PARQUET_DOUBLE: {
      if (size < (size_t)count * 8) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back(format_value(column, data + pos, 8));
        pos += 8;
      }
      break;
    }
    case PARQUET_BYTE_ARRAY: {
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        if (pos + 4 > size) {
          throw Error("corrupt parquet page");
        }
        uint32_t len = (uint32_t)read_le(data + pos, 4);
        pos += 4;
        if (pos + len > size) {
          throw Error("corrupt parquet page");
        }
        values.push_back(format_value(column, data + pos, len));
        pos += len;
      }
      break;
    }
    case PARQUET_FIXED_LEN_BYTE_ARRAY: {
      int length = column.type_length;
      if (length <= 0 || static_cast<size_t>(count) > size / static_cast<size_t>(length)) {
        throw Error("corrupt parquet page");
      }
      for (int i = 0; i < count; ++i) {
        if ((i & 4095) == 0) check_work();
        values.push_back(format_value(column, data + pos, length));
        pos += length;
      }
      break;
    }
    default: {
      throw Error("unsupported parquet type");
    }
  }
}

/**
 * @brief Decode the values of a page that use a dictionary encoding.
 */
void decode_dictionary_values(const uint8_t *data,
                              size_t size,
                              int count,
                              const BudgetVector<BudgetString> &dictionary,
                              BudgetVector<BudgetString> &values)
{
  if (count <= 0) {
    // the chunk holds no value at all, its dictionary is empty as well
    return;
  }
  if (dictionary.empty()) {
    throw Error("parquet page uses a dictionary that was not read");
  }
  if (size < 1) {
    throw Error("corrupt parquet page");
  }
  int width = data[0];
  BudgetVector<int32_t> indices;
  size_t consumed = 0;
  if (!decode_hybrid(data + 1, size - 1, width, count, indices, consumed)) {
    throw Error("corrupt parquet page");
  }
  for (size_t i = 0; i < indices.size(); ++i) {
    if (indices[i] < 0 || (size_t)indices[i] >= dictionary.size()) {
      throw Error("corrupt parquet page");
    }
    values.push_back(dictionary[indices[i]]);
  }
}

/**
 * @brief Header of one page of a column chunk.
 */
struct PageHeader {
  int type = PAGE_DATA;
  int32_t uncompressed_page_size = 0;
  int32_t compressed_page_size = 0;
  int32_t num_values = 0;
  int encoding = ENCODING_PLAIN;
  int definition_level_encoding = ENCODING_RLE;
  int repetition_level_encoding = ENCODING_RLE;
  int32_t num_nulls = 0;
  int32_t num_rows = 0;
  int32_t definition_levels_byte_length = 0;
  int32_t repetition_levels_byte_length = 0;
  bool is_compressed = true;
  int32_t dictionary_num_values = 0;
  int dictionary_encoding = ENCODING_PLAIN;
};

/**
 * @brief Parse the header of a data page.
 */
void parse_data_page_header(CompactReader &reader, PageHeader &header)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        header.num_values = reader.read_i32();
        break;
      }
      case 2: {
        header.encoding = reader.read_i32();
        break;
      }
      case 3: {
        header.definition_level_encoding = reader.read_i32();
        break;
      }
      case 4: {
        header.repetition_level_encoding = reader.read_i32();
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse the header of a dictionary page.
 */
void parse_dictionary_page_header(CompactReader &reader, PageHeader &header)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        header.dictionary_num_values = reader.read_i32();
        break;
      }
      case 2: {
        header.dictionary_encoding = reader.read_i32();
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse the header of a version 2 data page.
 */
void parse_data_page_header_v2(CompactReader &reader, PageHeader &header)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        header.num_values = reader.read_i32();
        break;
      }
      case 2: {
        header.num_nulls = reader.read_i32();
        break;
      }
      case 3: {
        header.num_rows = reader.read_i32();
        break;
      }
      case 4: {
        header.encoding = reader.read_i32();
        break;
      }
      case 5: {
        header.definition_levels_byte_length = reader.read_i32();
        break;
      }
      case 6: {
        header.repetition_levels_byte_length = reader.read_i32();
        break;
      }
      case 7: {
        header.is_compressed = field.bool_value;
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Parse the header of a page.
 */
void parse_page_header(CompactReader &reader, PageHeader &header)
{
  CompactStructReader fields(reader);
  CompactField field;
  while (fields.next_field(field)) {
    switch (field.id) {
      case 1: {
        header.type = reader.read_i32();
        break;
      }
      case 2: {
        header.uncompressed_page_size = reader.read_i32();
        break;
      }
      case 3: {
        header.compressed_page_size = reader.read_i32();
        break;
      }
      case 5: {
        parse_data_page_header(reader, header);
        break;
      }
      case 7: {
        parse_dictionary_page_header(reader, header);
        break;
      }
      case 8: {
        parse_data_page_header_v2(reader, header);
        break;
      }
      default: {
        skip_value(reader, field.type);
        break;
      }
    }
  }
}

/**
 * @brief Random access reader of a file.
 */
class FileReader
{
public:
  void open(const std::shared_ptr<LocalFile> &file)
  {
    file_ = file;
    size_ = file_->identity().size;
    data_end_ = size_;
  }

  uint64_t size() const { return size_; }
  uint64_t data_end() const { return data_end_; }
  void set_data_end(uint64_t offset) { data_end_ = offset; }

  void read_at(uint64_t offset, void *data, size_t length)
  {
    if (offset > size_ || length > size_ - offset) {
      throw Error("corrupt parquet file");
    }
    if (file_->read_at(offset, data, length) != length) {
      throw Error("corrupt parquet file");
    }
  }

private:
  std::shared_ptr<LocalFile> file_;
  uint64_t size_ = 0;
  uint64_t data_end_ = 0;
};

/**
 * @brief Reader of the values of one column chunk.
 *
 * The pages are decoded one at a time, the whole chunk is never held in
 * memory.
 */
class ColumnStream
{
public:
  ColumnStream(FileReader &file, const LeafColumn &column, const ColumnMetaData &metadata)
      : file_(file), column_(column), metadata_(metadata)
  {
    uint64_t start = metadata_.data_page_offset;
    if (metadata_.dictionary_page_offset >= 0 && (uint64_t)metadata_.dictionary_page_offset < start) {
      start = (uint64_t)metadata_.dictionary_page_offset;
    }
    if (start < 4 || start > file_.data_end() || metadata_.total_compressed_size < 0
        || static_cast<uint64_t>(metadata_.total_compressed_size) > file_.data_end() - start) {
      throw Error("corrupt parquet column chunk bounds");
    }
    chunk_end_ = start + static_cast<uint64_t>(metadata_.total_compressed_size);
    next_offset_ = start;
  }

  /**
   * @brief Read the next value of the column.
   * @return false when the column is exhausted
   */
  bool next(BudgetString &text, bool &is_null)
  {
    while (page_position_ >= page_values_.size()) {
      if (!load_page()) {
        return false;
      }
    }
    text = page_values_[page_position_];
    is_null = page_nulls_[page_position_] != 0;
    page_position_++;
    values_read_++;
    return true;
  }

private:
  bool read_page_header(PageHeader &header, uint64_t &header_size)
  {
    size_t capacity = 512;
    while (true) {
      if (next_offset_ >= chunk_end_) {
        throw Error("corrupt parquet file");
      }
      size_t available = (size_t)std::min((uint64_t)capacity, chunk_end_ - next_offset_);
      BudgetVector<uint8_t> buffer(available);
      file_.read_at(next_offset_, buffer.data(), available);
      try {
        CompactReader reader(buffer.data(), buffer.size());
        parse_page_header(reader, header);
        header_size = reader.position();
        return true;
      } catch (Truncated &) {
        if (capacity >= 65536 || available == chunk_end_ - next_offset_) {
          throw Error("corrupt parquet page header");
        }
        capacity *= 4;
      }
    }
  }

  bool load_page()
  {
    while (true) {
      if (values_read_ >= metadata_.num_values) {
        return false;
      }
      PageHeader header;
      uint64_t header_size = 0;
      read_page_header(header, header_size);
      uint64_t data_offset = next_offset_ + header_size;
      if (header.compressed_page_size < 0 || data_offset > chunk_end_
          || static_cast<uint64_t>(header.compressed_page_size) > chunk_end_ - data_offset) {
        throw Error("corrupt parquet file");
      }
      if (header.uncompressed_page_size < 0) {
        throw Error("negative parquet page size");
      }
      check_count(header.compressed_page_size, MAX_PARQUET_BYTES, "compressed parquet page");
      check_count(header.uncompressed_page_size, MAX_PARQUET_BYTES, "decompressed parquet page");
      BudgetVector<uint8_t> raw((size_t)header.compressed_page_size);
      if (!raw.empty()) {
        file_.read_at(data_offset, raw.data(), raw.size());
      }
      next_offset_ = data_offset + (uint64_t)header.compressed_page_size;
      if (header.type == PAGE_DICTIONARY) {
        decode_dictionary_page(header, raw.data(), raw.size());
        continue;
      }
      if (header.type == PAGE_INDEX) {
        continue;
      }
      if (header.type == PAGE_DATA || header.type == PAGE_DATA_V2) {
        decode_data_page(header, raw.data(), raw.size());
        if (!page_values_.empty()) {
          return true;
        }
        continue;
      }
      throw Error("unsupported parquet page type");
    }
  }

  void decode_dictionary_page(const PageHeader &header, const uint8_t *raw, size_t raw_size)
  {
    BudgetVector<uint8_t> plain = decompress_page(raw, raw_size, metadata_.codec, header.uncompressed_page_size);
    int count = header.dictionary_num_values;
    if (count < 0) {
      throw Error("corrupt parquet dictionary");
    }
    dictionary_.clear();
    if (header.dictionary_encoding == ENCODING_PLAIN || header.dictionary_encoding == ENCODING_PLAIN_DICTIONARY
        || header.dictionary_encoding == ENCODING_RLE_DICTIONARY) {
      decode_plain_values(plain.data(), plain.size(), column_, count, dictionary_);
    } else {
      throw Error("unsupported parquet dictionary encoding");
    }
  }

  void decode_data_page(const PageHeader &header, const uint8_t *raw, size_t raw_size)
  {
    page_values_.clear();
    page_nulls_.clear();
    page_position_ = 0;
    int count = header.num_values;
    if (count < 0 || count > metadata_.num_values - values_read_) {
      throw Error("parquet page value count exceeds column chunk");
    }
    page_values_.reserve(static_cast<size_t>(count));
    page_nulls_.reserve(static_cast<size_t>(count));
    BudgetVector<int32_t> def_levels;
    const uint8_t *values_data = NULL;
    size_t values_size = 0;
    BudgetVector<uint8_t> plain;
    if (header.type == PAGE_DATA) {
      plain = decompress_page(raw, raw_size, metadata_.codec, header.uncompressed_page_size);
      size_t pos = 0;
      if (column_.max_rep_level > 0) {
        BudgetVector<int32_t> rep_levels;
        size_t used = decode_levels(plain.data() + pos,
                                    plain.size() - pos,
                                    bit_width(column_.max_rep_level),
                                    column_.max_rep_level,
                                    count,
                                    true,
                                    rep_levels);
        pos += used;
      }
      if (column_.max_def_level > 0) {
        size_t used = decode_levels(plain.data() + pos,
                                    plain.size() - pos,
                                    bit_width(column_.max_def_level),
                                    column_.max_def_level,
                                    count,
                                    true,
                                    def_levels);
        pos += used;
      }
      values_data = plain.data() + pos;
      values_size = plain.size() - pos;
    } else {
      if (header.repetition_levels_byte_length < 0 || header.definition_levels_byte_length < 0
          || header.num_nulls < 0 || header.num_nulls > count || header.num_rows != count) {
        throw Error("corrupt parquet v2 page lengths or counts");
      }
      size_t rep_length = (size_t)header.repetition_levels_byte_length;
      size_t def_length = (size_t)header.definition_levels_byte_length;
      if (rep_length > raw_size || def_length > raw_size - rep_length
          || rep_length + def_length > static_cast<size_t>(header.uncompressed_page_size)) {
        throw Error("corrupt parquet page");
      }
      if (column_.max_rep_level > 0) {
        BudgetVector<int32_t> rep_levels;
        decode_levels(raw,
                      rep_length,
                      bit_width(column_.max_rep_level),
                      column_.max_rep_level,
                      count,
                      false,
                      rep_levels);
      }
      if (column_.max_def_level > 0) {
        decode_levels(raw + rep_length,
                      def_length,
                      bit_width(column_.max_def_level),
                      column_.max_def_level,
                      count,
                      false,
                      def_levels);
      }
      const uint8_t *compressed = raw + rep_length + def_length;
      size_t compressed_size = raw_size - rep_length - def_length;
      size_t expected_values_size = header.uncompressed_page_size - rep_length - def_length;
      if (header.is_compressed) {
        plain = decompress_page(compressed, compressed_size, metadata_.codec, expected_values_size);
        values_data = plain.data();
        values_size = plain.size();
      } else {
        if (compressed_size != expected_values_size) throw Error("parquet decompressed page size mismatch");
        values_data = compressed;
        values_size = compressed_size;
      }
    }
    int num_nulls = 0;
    for (int i = 0; i < count; ++i) {
      if (column_.max_def_level > 0 && def_levels[i] < column_.max_def_level) {
        num_nulls++;
      }
    }
    if (header.type == PAGE_DATA_V2 && num_nulls != header.num_nulls) {
      throw Error("parquet v2 null count mismatch");
    }
    BudgetVector<BudgetString> decoded;
    if (header.encoding == ENCODING_PLAIN) {
      decode_plain_values(values_data, values_size, column_, count - num_nulls, decoded);
    } else if (header.encoding == ENCODING_PLAIN_DICTIONARY || header.encoding == ENCODING_RLE_DICTIONARY) {
      decode_dictionary_values(values_data, values_size, count - num_nulls, dictionary_, decoded);
    } else if (header.encoding == ENCODING_DELTA_BINARY_PACKED) {
      throw Error("delta binary packed parquet files are not supported");
    } else {
      throw Error("unsupported parquet encoding");
    }
    size_t next = 0;
    for (int i = 0; i < count; ++i) {
      bool is_null = (column_.max_def_level > 0 && def_levels[i] < column_.max_def_level);
      page_nulls_.push_back(is_null ? 1 : 0);
      page_values_.push_back(is_null ? BudgetString() : decoded[next++]);
    }
  }

private:
  FileReader &file_;
  const LeafColumn &column_;
  const ColumnMetaData &metadata_;
  uint64_t next_offset_ = 0;
  uint64_t chunk_end_ = 0;
  int64_t values_read_ = 0;
  BudgetVector<BudgetString> dictionary_;
  BudgetVector<BudgetString> page_values_;
  BudgetVector<char> page_nulls_;
  size_t page_position_ = 0;
};

} // namespace

/**
 * @brief Implementation of the parquet scanner.
 */
class ParquetScanner::ParquetScannerImpl
{
public:
  ParquetScannerImpl()
      : budget_(active_file_sql_budget ? active_file_sql_budget : std::make_shared<MemoryBudget>()),
        bookkeeping_(budget_, 64 * 1024) {}
  const std::shared_ptr<MemoryBudget> &budget() const { return budget_; }
  void open(const std::shared_ptr<LocalFile> &file);
  const BudgetVector<ParquetColumn> &schema() const { return schema_; }
  uint64_t row_count() const { return num_rows_; }
  void set_projection(const BudgetVector<int> &projection);
  bool next_row(BudgetVector<BudgetString> &texts, BudgetVector<bool> &nulls);
  void reset();

private:
  void start_row_group();

private:
  std::shared_ptr<MemoryBudget> budget_;
  MemoryReservation bookkeeping_;
  FileReader file_;
  BudgetVector<LeafColumn> leaves_;
  BudgetVector<ParquetColumn> schema_;
  BudgetVector<RowGroup> row_groups_;
  uint64_t num_rows_ = 0;
  BudgetVector<int> projection_;
  size_t group_index_ = 0;
  int64_t row_in_group_ = 0;
  bool group_started_ = false;
  BudgetVector<std::shared_ptr<ColumnStream>> streams_;
};

void ParquetScanner::ParquetScannerImpl::open(const std::shared_ptr<LocalFile> &file)
{
  file_.open(file);
  const auto &path = file->path();
  uint64_t size = file_.size();
  if (size < 12) {
    throw Error("not a parquet file: " + path);
  }
  uint8_t magic[4];
  file_.read_at(0, magic, 4);
  if (memcmp(magic, PARQUET_MAGIC, 4) != 0) {
    throw Error("not a parquet file: " + path);
  }
  uint8_t tail[8];
  file_.read_at(size - 8, tail, 8);
  if (memcmp(tail + 4, PARQUET_MAGIC, 4) != 0) {
    throw Error("not a parquet file: " + path);
  }
  uint32_t footer_length = (uint32_t)read_le(tail, 4);
  if ((uint64_t)footer_length + 8 > size) {
    throw Error("corrupt parquet footer: " + path);
  }
  check_count(footer_length, MAX_PARQUET_BYTES, "parquet footer");
  BudgetVector<uint8_t> footer(footer_length);
  if (footer_length > 0) {
    file_.read_at(size - 8 - footer_length, footer.data(), footer.size());
  }
  file_.set_data_end(size - 8 - footer_length);
  CompactReader reader(footer.data(), footer.size());
  FileMetaData metadata;
  try {
    parse_file_metadata(reader, metadata);
  } catch (const Truncated &) {
    throw Error("truncated parquet footer: " + path);
  }
  if (metadata.schema.empty()) {
    throw Error("corrupt parquet schema: " + path);
  }

  check_count(metadata.schema.size(), MAX_COLUMNS + 1, "parquet schema column");
  if (metadata.schema[0].num_children < 0
      || static_cast<size_t>(metadata.schema[0].num_children) != metadata.schema.size() - 1) {
    throw Error("nested or corrupt parquet schema is not supported");
  }
  for (size_t i = 1; i < metadata.schema.size(); ++i) {
    if (metadata.schema[i].num_children != 0) {
      throw Error("nested parquet columns are not supported");
    }
  }
  leaves_.clear();
  BudgetVector<int> sizes = compute_subtree_sizes(metadata.schema);
  const SchemaElement &root = metadata.schema[0];
  int child = 1;
  for (int c = 0; c < root.num_children; ++c) {
    if (child >= (int)metadata.schema.size()) {
      break;
    }
    collect_leaf_columns(metadata.schema, sizes, child, "", 0, 0, false, leaves_);
    child += sizes[child];
  }

  schema_.clear();
  schema_.reserve(leaves_.size());
  for (size_t i = 0; i < leaves_.size(); ++i) {
    ParquetColumn column;
    column.name = leaves_[i].name;
    column.type = leaves_[i].type;
    column.nullable = leaves_[i].nullable;
    schema_.push_back(column);
  }

  if (metadata.num_rows < 0) {
    throw Error("corrupt parquet row count: " + path);
  }
  uint64_t total_rows = 0;
  for (const auto &group : metadata.row_groups) {
    if (group.num_rows < 0 || (uint64_t)group.num_rows > (uint64_t)metadata.num_rows - total_rows) {
      throw Error("corrupt parquet row group count: " + path);
    }
    total_rows += (uint64_t)group.num_rows;
    for (const auto &chunk : group.columns) {
      // Only flat, non-repeated columns are supported: one value (including
      // NULL) is required for every row, even for columns not projected.
      if (!chunk.has_metadata || chunk.metadata.num_values != group.num_rows) {
        throw Error("parquet column value count differs from row group: " + path);
      }
    }
  }
  if (total_rows != (uint64_t)metadata.num_rows) {
    throw Error("parquet row group counts differ from file row count: " + path);
  }
  row_groups_ = std::move(metadata.row_groups);
  num_rows_ = metadata.num_rows;
  reset();
}

void ParquetScanner::ParquetScannerImpl::set_projection(const BudgetVector<int> &projection)
{
  projection_.clear();
  for (size_t i = 0; i < projection.size(); ++i) {
    // the projection is only checked against the schema when the file is
    // already open, it may be set before the file is read
    if (!schema_.empty() && (projection[i] < 0 || (size_t)projection[i] >= schema_.size())) {
      throw Error("parquet column index out of range: " + budget_to_string(projection[i]));
    }
    projection_.push_back(projection[i]);
  }
  reset();
}

void ParquetScanner::ParquetScannerImpl::start_row_group()
{
  streams_.clear();
  group_started_ = true;
  if (projection_.empty() || group_index_ >= row_groups_.size()) {
    return;
  }
  const RowGroup &group = row_groups_[group_index_];
  for (size_t i = 0; i < projection_.size(); ++i) {
    size_t column_index = (size_t)projection_[i];
    if (projection_[i] < 0 || column_index >= leaves_.size()) {
      throw Error("parquet column index out of range: " + budget_to_string(projection_[i]));
    }
    const LeafColumn &leaf = leaves_[column_index];
    const ColumnChunk *chunk = NULL;
    for (size_t c = 0; c < group.columns.size(); ++c) {
      if (group.columns[c].has_metadata && group.columns[c].metadata.path_in_schema == leaf.path) {
        chunk = &group.columns[c];
        break;
      }
    }
    if (chunk == NULL && column_index < group.columns.size()) {
      chunk = &group.columns[column_index];
    }
    if (chunk == NULL || !chunk->has_metadata) {
      throw Error("parquet row group is missing a column chunk");
    }
    streams_.push_back(std::allocate_shared<ColumnStream>(BudgetAllocator<ColumnStream>(), file_, leaf, chunk->metadata));
  }
}

bool ParquetScanner::ParquetScannerImpl::next_row(BudgetVector<BudgetString> &texts, BudgetVector<bool> &nulls)
{
  texts.clear();
  nulls.clear();
  while (group_index_ < row_groups_.size()) {
    const RowGroup &group = row_groups_[group_index_];
    if (row_in_group_ >= group.num_rows) {
      group_index_++;
      row_in_group_ = 0;
      group_started_ = false;
      continue;
    }
    if (!group_started_) {
      start_row_group();
    }
    for (size_t i = 0; i < projection_.size(); ++i) {
      BudgetString text;
      bool is_null = false;
      if (!streams_[i]->next(text, is_null)) {
        throw Error("parquet column ended before the declared row count");
      }
      texts.push_back(text);
      nulls.push_back(is_null);
    }
    row_in_group_++;
    return true;
  }
  return false;
}

void ParquetScanner::ParquetScannerImpl::reset()
{
  group_index_ = 0;
  row_in_group_ = 0;
  group_started_ = false;
  streams_.clear();
}

ParquetScanner::ParquetScanner() : impl_(new ParquetScannerImpl())
{
}

ParquetScanner::~ParquetScanner()
{
}

void ParquetScanner::open(const std::shared_ptr<LocalFile> &file)
{
  BudgetScope scope(impl_->budget());
  impl_->open(file);
}

const BudgetVector<ParquetColumn> &ParquetScanner::schema() const
{
  return impl_->schema();
}

uint64_t ParquetScanner::row_count() const
{
  return impl_->row_count();
}

void ParquetScanner::set_projection(const BudgetVector<int> &projection)
{
  BudgetScope scope(impl_->budget());
  impl_->set_projection(projection);
}

bool ParquetScanner::next_row(BudgetVector<BudgetString> &texts, BudgetVector<bool> &nulls)
{
  BudgetScope scope(impl_->budget());
  return impl_->next_row(texts, nulls);
}

void ParquetScanner::reset()
{
  impl_->reset();
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
