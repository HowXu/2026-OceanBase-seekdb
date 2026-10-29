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

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_file_parquet_scan.h"
#include "sql/engine/basic/file_sql_file_access.h"
#include "sql/engine/basic/file_sql_memory.h"

#include <algorithm>
#include <random>
#include <limits.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace oceanbase {
namespace sql {
namespace file_sql {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

BudgetString lowercase(std::string_view value)
{
  BudgetString result(value.data(), value.size());
  for (char &item : result) {
    item = static_cast<char>(::tolower(static_cast<unsigned char>(item)));
  }
  return result;
}

BudgetString trim(std::string_view value)
{
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && ::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  while (end > begin && ::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return BudgetString(value.data() + begin, end - begin);
}

bool is_valid_utf8(const char *data, size_t length)
{
  size_t index = 0;
  while (index < length) {
    const unsigned char lead = static_cast<unsigned char>(data[index]);
    if (lead < 0x80) {
      ++index;
      continue;
    }
    size_t extra = 0;
    unsigned int code = 0;
    if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      code = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      code = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      code = lead & 0x07u;
    } else {
      return false;
    }
    if (index + extra >= length) {
      return false;
    }
    for (size_t offset = 1; offset <= extra; ++offset) {
      const unsigned char continuation = static_cast<unsigned char>(data[index + offset]);
      if ((continuation & 0xC0) != 0x80) {
        return false;
      }
      code = (code << 6) | (continuation & 0x3Fu);
    }
    if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000)) {
      return false;
    }
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

bool is_leap_year(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month)
{
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return days[month - 1];
}

bool is_valid_date(int year, int month, int day)
{
  return year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
}

bool is_valid_time(int hour, int minute, int second)
{
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

bool parse_digits(std::string_view value, size_t offset, size_t count, int &result)
{
  if (offset + count > value.size()) {
    return false;
  }
  int parsed = 0;
  for (size_t index = 0; index < count; ++index) {
    const char digit = value[offset + index];
    if (digit < '0' || digit > '9') {
      return false;
    }
    parsed = parsed * 10 + (digit - '0');
  }
  result = parsed;
  return true;
}

bool looks_like_integer(std::string_view value)
{
  if (value.empty()) {
    return false;
  }
  size_t index = 0;
  if (value[index] == '+' || value[index] == '-') {
    ++index;
  }
  if (index >= value.size()) {
    return false;
  }
  for (; index < value.size(); ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return false;
    }
  }
  return true;
}

bool looks_like_double(std::string_view value)
{
  if (value.empty()) {
    return false;
  }
  size_t index = 0;
  if (value[index] == '+' || value[index] == '-') {
    ++index;
  }
  bool has_digit = false;
  bool has_fraction = false;
  bool has_exponent = false;
  for (; index < value.size(); ++index) {
    const char item = value[index];
    if (item >= '0' && item <= '9') {
      has_digit = true;
    } else if (item == '.') {
      if (has_fraction || has_exponent) {
        return false;
      }
      has_fraction = true;
    } else if (item == 'e' || item == 'E') {
      if (!has_digit || has_exponent) {
        return false;
      }
      has_exponent = true;
      if (index + 1 < value.size() && (value[index + 1] == '+' || value[index + 1] == '-')) {
        ++index;
      }
      if (index + 1 >= value.size()) {
        return false;
      }
    } else {
      return false;
    }
  }
  return has_digit && (has_fraction || has_exponent);
}

/**
 * @brief Classify a temporal literal.
 * @param value the trimmed value
 * @param scale receives the number of fractional digits of a datetime
 * @return Date, Datetime or Null when the value is not a temporal literal
 */
Type classify_temporal(std::string_view value, int &scale)
{
  scale = 0;
  if (value.size() < 10) {
    return Type::Null;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  if (!parse_digits(value, 0, 4, year) || value[4] != '-' || !parse_digits(value, 5, 2, month) || value[7] != '-'
      || !parse_digits(value, 8, 2, day)) {
    return Type::Null;
  }
  if (!is_valid_date(year, month, day)) {
    return Type::Null;
  }
  if (value.size() == 10) {
    return Type::Date;
  }
  const char separator = value[10];
  if (separator != 'T' && separator != ' ') {
    return Type::Null;
  }
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_digits(value, 11, 2, hour) || value.size() < 14 || value[13] != ':' || !parse_digits(value, 14, 2, minute)) {
    return Type::Null;
  }
  size_t index = 16;
  if (index < value.size() && value[index] == ':') {
    if (!parse_digits(value, index + 1, 2, second)) {
      return Type::Null;
    }
    index += 3;
  }
  if (index < value.size() && value[index] == '.') {
    size_t end = index + 1;
    while (end < value.size() && value[end] >= '0' && value[end] <= '9') {
      ++end;
    }
    if (end == index + 1 || end != value.size()) {
      return Type::Null;
    }
    scale = static_cast<int>(end - index - 1);
    index = end;
  }
  if (index != value.size() || !is_valid_time(hour, minute, second)) {
    return Type::Null;
  }
  return Type::Datetime;
}

std::string extension_of(const std::string &path)
{
  const size_t slash = path.find_last_of('/');
  const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= base.size()) {
    return std::string();
  }
  const auto lower = lowercase(base.substr(dot + 1));
  return std::string(lower.data(), lower.size());
}

std::string directory_of(const std::string &path)
{
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

std::string basename_of(const std::string &path)
{
  const size_t slash = path.find_last_of('/');
  return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool stat_path(const std::string &path, struct stat &info)
{
  return ::stat(path.c_str(), &info) == 0;
}

std::string canonical_path(const std::string &path)
{
  if (path.find('\0') != std::string::npos) {
    throw Error("file SQL path contains NUL");
  }
  check_count(path.size(), 4096, "path");
  char buffer[PATH_MAX];
  if (::realpath(path.c_str(), buffer) == nullptr) {
    throw Error("cannot resolve the path " + path);
  }
  return std::string(buffer);
}

bool is_directory(const std::string &path)
{
  struct stat info;
  if (!stat_path(path, info)) {
    return false;
  }
  return S_ISDIR(info.st_mode);
}

bool is_regular_file(const std::string &path)
{
  struct stat info;
  if (!stat_path(path, info)) {
    return false;
  }
  return S_ISREG(info.st_mode);
}

bool is_within(const std::string &path, const std::string &root)
{
  if (root == "/" || path == root) {
    return true;
  }
  if (path.size() <= root.size()) {
    return false;
  }
  if (path.compare(0, root.size(), root) != 0) {
    return false;
  }
  return path[root.size()] == '/';
}

/**
 * @brief Build unique column names, the duplicates get a numeric suffix.
 */
// Private representations keep names, values and nested container storage on
// one budget. Public std::string/vector results are copied only at the boundary.
struct WorkingOptions {
  Format format = Format::Auto;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  BudgetString null_string;
  int64_t sample_size = -1;

  WorkingOptions() = default;
  explicit WorkingOptions(const Options &options)
      : format(options.format), header(options.header), delimiter(options.delimiter),
        quote(options.quote), escape(options.escape), sample_size(options.sample_size)
  {
    if (options.null_string.size() > MAX_RECORD_BYTES) {
      throw Error("file SQL NULL option limit exceeded");
    }
    null_string.assign(options.null_string.data(), options.null_string.size());
  }
};

struct WorkingColumn {
  BudgetString name;
  Type type = Type::Varchar;
  bool nullable = false;
  int scale = 0;
};

struct WorkingCell {
  BudgetString text;
  bool is_null = false;
};

struct WorkingDefinition {
  BudgetString path;
  WorkingOptions options;
  BudgetVector<WorkingColumn> columns;
  uint64_t file_size = 0;
  int64_t file_mtime_ns = 0;
  uint64_t file_device = 0;
  uint64_t file_inode = 0;
  int64_t file_ctime_ns = 0;

  WorkingDefinition() = default;
  explicit WorkingDefinition(const Definition &source)
  {
    check_count(source.columns.size(), MAX_COLUMNS, "column");
    check_count(source.path.size(), 4096, "path");
    path.assign(source.path.data(), source.path.size());
    options = WorkingOptions(source.options);
    file_size = source.file_size;
    file_mtime_ns = source.file_mtime_ns;
    file_device = source.file_device;
    file_inode = source.file_inode;
    file_ctime_ns = source.file_ctime_ns;
    columns.reserve(source.columns.size());
    for (const auto &column : source.columns) {
      check_count(column.name.size(), MAX_RECORD_BYTES, "column name");
      WorkingColumn copy;
      copy.name.assign(column.name.data(), column.name.size());
      copy.type = column.type;
      copy.nullable = column.nullable;
      copy.scale = column.scale;
      columns.push_back(std::move(copy));
    }
  }
};

Definition export_definition(const WorkingDefinition &source)
{
  Definition result;
  result.memory_reservation = std::make_shared<MemoryReservation>(active_file_sql_budget);
  MemoryReservation &copy = *result.memory_reservation;
  reserve_standard_copy(copy, source.path.size() + source.options.null_string.size() + 2);
  reserve_standard_copy(copy, source.columns.size() * sizeof(Column));
  for (const auto &column : source.columns) {
    reserve_standard_copy(copy, column.name.size() + 1);
  }
  result.path.assign(source.path.data(), source.path.size());
  result.options.format = source.options.format;
  result.options.header = source.options.header;
  result.options.delimiter = source.options.delimiter;
  result.options.quote = source.options.quote;
  result.options.escape = source.options.escape;
  result.options.null_string.assign(source.options.null_string.data(), source.options.null_string.size());
  result.options.sample_size = source.options.sample_size;
  result.file_size = source.file_size;
  result.file_mtime_ns = source.file_mtime_ns;
  result.file_device = source.file_device;
  result.file_inode = source.file_inode;
  result.file_ctime_ns = source.file_ctime_ns;
  result.columns.reserve(source.columns.size());
  for (const auto &column : source.columns) {
    Column output;
    output.name.assign(column.name.data(), column.name.size());
    output.type = column.type;
    output.nullable = column.nullable;
    output.scale = column.scale;
    result.columns.push_back(std::move(output));
  }
  return result;
}

BudgetVector<BudgetString> unique_names(const BudgetVector<BudgetString> &raw_names)
{
  BudgetVector<BudgetString> names;
  BudgetMap<BudgetString, int> counters;
  for (const BudgetString &raw_name : raw_names) {
    const BudgetString base = trim(raw_name);
    const BudgetString key = lowercase(base);
    BudgetMap<BudgetString, int>::iterator found = counters.find(key);
    if (found == counters.end()) {
      counters[key] = 1;
      names.push_back(base);
      continue;
    }
    int count = ++found->second;
    BudgetString candidate = key + "__" + budget_to_string(count);
    while (std::find(names.begin(), names.end(), candidate) != names.end()) {
      candidate = key + "__" + budget_to_string(++count);
    }
    found->second = count;
    names.push_back(candidate);
  }
  return names;
}

// ---------------------------------------------------------------------------
// CSV reader
// ---------------------------------------------------------------------------

/**
 * @brief Buffered reader over a file.
 */
class ByteSource {
public:
  explicit ByteSource(const std::shared_ptr<LocalFile> &file) : file_(file) {}

  int peek()
  {
    if (position_ >= buffer_.size() && !fill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[position_]);
  }

  int get()
  {
    const int item = peek();
    if (item >= 0) {
      check_count(record_bytes_ + 1, MAX_RECORD_BYTES + 2, "record");
      ++record_bytes_;
      ++position_;
    }
    return item;
  }

  void begin_record()
  {
    record_bytes_ = 0;
    record_start_ = offset_ - buffer_.size() + position_;
    ++record_number_;
  }

  Error error(std::string_view message) const
  {
    return Error("file SQL record " + budget_to_string(record_number_)
                 + ", byte " + budget_to_string(record_start_) + ": "
                 + BudgetString(message.substr(0, 1024)) + " (file "
                 + BudgetString(file_->path().data(), file_->path().size()) + ")");
  }
  void finish_record(size_t terminator_bytes)
  {
    check_count(record_bytes_ - terminator_bytes, MAX_RECORD_BYTES, "record");
  }

  bool read_line(BudgetString &line)
  {
    begin_record();
    line.clear();
    int item;
    while ((item = get()) >= 0) {
      if (item == '\n') {
        finish_record(!line.empty() && line.back() == '\r' ? 2 : 1);
        return true;
      }
      line.push_back(static_cast<char>(item));
    }
    finish_record(0);
    return !line.empty();
  }

  void rewind()
  {
    offset_ = 0;
    position_ = 0;
    record_bytes_ = 0;
    record_start_ = 0;
    record_number_ = 0;
    buffer_.clear();
  }

private:
  bool fill()
  {
    buffer_.resize(64 * 1024);
    const size_t count = file_->read_at(offset_, buffer_.data(), buffer_.size());
    offset_ += count;
    buffer_.resize(count);
    position_ = 0;
    return count > 0;
  }

  std::shared_ptr<LocalFile> file_;
  uint64_t offset_ = 0;
  BudgetVector<char> buffer_;
  size_t position_ = 0;
  size_t record_bytes_ = 0;
  uint64_t record_start_ = 0;
  uint64_t record_number_ = 0;
};

/**
 * @brief Reader of the records of a CSV file.
 *
 * A record is a list of values together with the flag telling whether the
 * value was enclosed in quotes. The flag is needed because an unquoted value
 * equal to the null string is a null value while a quoted one is a literal.
 */
class CsvRecordReader {
public:
  CsvRecordReader(const std::shared_ptr<LocalFile> &file, const WorkingOptions &options)
      : path_(file->path()), options_(options), source_(file)
  {
    skip_bom();
  }

  bool next_record(BudgetVector<BudgetString> &values, BudgetVector<bool> &quoted)
  {
    while (true) {
      values.clear();
      quoted.clear();
      source_.begin_record();
      size_t terminator_bytes = 0;
      if (source_.peek() < 0) {
        return false;
      }
      while (true) {
        check_count(values.size() + 1, MAX_COLUMNS, "CSV column");
        BudgetString value;
        bool was_quoted = false;
        const int first = source_.peek();
        if (first == options_.quote) {
          was_quoted = true;
          source_.get();
          read_quoted_value(value);
        } else {
          read_plain_value(value);
        }
        values.push_back(value);
        quoted.push_back(was_quoted);
        const int separator = source_.peek();
        if (separator == options_.delimiter) {
          source_.get();
          continue;
        }
        if (separator == '\r') {
          source_.get();
          terminator_bytes = 1;
          if (source_.peek() == '\n') {
            source_.get();
            terminator_bytes = 2;
          }
          break;
        }
        if (separator == '\n') {
          source_.get();
          terminator_bytes = 1;
          break;
        }
        if (separator >= 0) {
          throw error("unexpected characters after a closing CSV quote");
        }
        break;
      }
      source_.finish_record(terminator_bytes);
      // a line without any character is not a record
      if (values.size() == 1 && !quoted[0] && values[0].empty()) {
        continue;
      }
      return true;
    }
  }

  void reset()
  {
    source_.rewind();
    skip_bom();
  }

  Error error(std::string_view message) const { return source_.error(message); }

private:
  void skip_bom()
  {
    if (source_.peek() == 0xEF) {
      source_.get();
      if (source_.peek() == 0xBB) {
        source_.get();
        if (source_.peek() == 0xBF) {
          source_.get();
        }
      }
    }
  }

  void read_quoted_value(BudgetString &value)
  {
    while (true) {
      const int item = source_.get();
      if (item < 0) {
        throw error("the CSV file has an unterminated quoted value");
      }
      if (options_.escape != options_.quote && item == options_.escape) {
        const int escaped = source_.get();
        if (escaped < 0) {
          throw error("the CSV file has an unterminated quoted value");
        }
        value.push_back(static_cast<char>(escaped));
        continue;
      }
      if (item == options_.quote) {
        if (options_.escape == options_.quote && source_.peek() == options_.quote) {
          source_.get();
          value.push_back(static_cast<char>(options_.quote));
          continue;
        }
        break;
      }
      if (item == '\r') {
        if (source_.peek() == '\n') {
          source_.get();
        }
        value.push_back('\n');
        continue;
      }
      value.push_back(static_cast<char>(item));
    }
    // Ignore padding after a closing quote, but never consume the delimiter.
    while ((source_.peek() == ' ' || source_.peek() == '\t') && source_.peek() != options_.delimiter) {
      source_.get();
    }
  }

  void read_plain_value(BudgetString &value)
  {
    while (true) {
      const int item = source_.peek();
      if (item < 0 || item == options_.delimiter || item == '\n' || item == '\r') {
        break;
      }
      value.push_back(static_cast<char>(source_.get()));
    }
  }

  BudgetString path_;
  WorkingOptions options_;
  ByteSource source_;
};

// ---------------------------------------------------------------------------
// JSON reader
// ---------------------------------------------------------------------------

struct JsonMember {
  BudgetString key;
  BudgetString value;
  Type type = Type::Null;
  bool is_null = false;
};

void append_utf8(BudgetString &out, unsigned int code)
{
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

/**
 * @brief Parser of one line of a JSON lines file.
 *
 * Only flat objects are supported, a nested object or array is rejected.
 */
class JsonLineParser {
public:
  explicit JsonLineParser(const BudgetString &text) : text_(text), position_(0) {}

  bool parse(BudgetVector<JsonMember> &members, BudgetString &error)
  {
    members.clear();
    skip_space();
    if (position_ >= text_.size() || text_[position_] != '{') {
      error = "the JSON line is not an object";
      return false;
    }
    ++position_;
    skip_space();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      skip_space();
      if (position_ != text_.size()) {
        error = "the JSON line has trailing characters";
        return false;
      }
      return true;
    }
    while (true) {
      check_count(members.size() + 1, MAX_COLUMNS, "JSON field");
      skip_space();
      BudgetString key;
      if (!parse_string(key, error)) {
        return false;
      }
      skip_space();
      if (position_ >= text_.size() || text_[position_] != ':') {
        error = "the JSON object expects a colon after the key";
        return false;
      }
      ++position_;
      skip_space();
      JsonMember member;
      member.key = key;
      if (!parse_value(member, error)) {
        return false;
      }
      members.push_back(member);
      skip_space();
      if (position_ < text_.size() && text_[position_] == ',') {
        ++position_;
        continue;
      }
      if (position_ < text_.size() && text_[position_] == '}') {
        ++position_;
        break;
      }
      error = "the JSON object expects a comma or a closing brace";
      return false;
    }
    skip_space();
    if (position_ != text_.size()) {
      error = "the JSON line has trailing characters";
      return false;
    }
    return true;
  }

private:
  void skip_space()
  {
    while (position_ < text_.size() && ::isspace(static_cast<unsigned char>(text_[position_]))) {
      ++position_;
    }
  }

  bool parse_string(BudgetString &out, BudgetString &error)
  {
    if (position_ >= text_.size() || text_[position_] != '"') {
      error = "the JSON object expects a string key";
      return false;
    }
    ++position_;
    out.clear();
    while (position_ < text_.size()) {
      const char item = text_[position_++];
      if (item == '"') {
        return true;
      }
      if (item != '\\') {
        out.push_back(item);
        continue;
      }
      if (position_ >= text_.size()) {
        break;
      }
      const char escaped = text_[position_++];
      switch (escaped) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (position_ + 4 > text_.size()) {
            error = "the JSON string has an invalid unicode escape";
            return false;
          }
          unsigned int code = 0;
          for (int index = 0; index < 4; ++index) {
            const char digit = text_[position_++];
            code <<= 4;
            if (digit >= '0' && digit <= '9') {
              code |= static_cast<unsigned int>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
              code |= static_cast<unsigned int>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
              code |= static_cast<unsigned int>(digit - 'A' + 10);
            } else {
              error = "the JSON string has an invalid unicode escape";
              return false;
            }
          }
          if (code >= 0xD800 && code <= 0xDBFF && position_ + 6 <= text_.size() && text_[position_] == '\\'
              && text_[position_ + 1] == 'u') {
            const size_t saved = position_;
            position_ += 2;
            unsigned int low = 0;
            bool valid = true;
            for (int index = 0; index < 4; ++index) {
              const char digit = text_[position_++];
              low <<= 4;
              if (digit >= '0' && digit <= '9') {
                low |= static_cast<unsigned int>(digit - '0');
              } else if (digit >= 'a' && digit <= 'f') {
                low |= static_cast<unsigned int>(digit - 'a' + 10);
              } else if (digit >= 'A' && digit <= 'F') {
                low |= static_cast<unsigned int>(digit - 'A' + 10);
              } else {
                valid = false;
                break;
              }
            }
            if (valid && low >= 0xDC00 && low <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            } else {
              position_ = saved;
            }
          }
          append_utf8(out, code);
          break;
        }
        default:
          error = "the JSON string has an invalid escape sequence";
          return false;
      }
    }
    error = "the JSON string is not terminated";
    return false;
  }

  bool parse_value(JsonMember &member, BudgetString &error)
  {
    if (position_ >= text_.size()) {
      error = "the JSON object expects a value";
      return false;
    }
    const char item = text_[position_];
    if (item == '{') {
      error = "a nested JSON object is not supported";
      return false;
    }
    if (item == '[') {
      error = "a nested JSON array is not supported";
      return false;
    }
    if (item == '"') {
      member.type = Type::Varchar;
      return parse_string(member.value, error);
    }
    if (item == 't') {
      if (text_.compare(position_, 4, "true") != 0) {
        error = "the JSON object has an invalid literal";
        return false;
      }
      position_ += 4;
      member.value = "true";
      member.type = Type::Boolean;
      return true;
    }
    if (item == 'f') {
      if (text_.compare(position_, 5, "false") != 0) {
        error = "the JSON object has an invalid literal";
        return false;
      }
      position_ += 5;
      member.value = "false";
      member.type = Type::Boolean;
      return true;
    }
    if (item == 'n') {
      if (text_.compare(position_, 4, "null") != 0) {
        error = "the JSON object has an invalid literal";
        return false;
      }
      position_ += 4;
      member.value.clear();
      member.type = Type::Null;
      member.is_null = true;
      return true;
    }
    const size_t start = position_;
    const auto invalid_number = [&]() {
      error = "the JSON object has an invalid number";
      return false;
    };
    const auto consume_digits = [&]() {
      const size_t begin = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
        ++position_;
      }
      return position_ > begin;
    };
    if (text_[position_] == '-') {
      ++position_;
    }
    if (position_ < text_.size() && text_[position_] == '0') {
      ++position_;
    } else if (position_ < text_.size() && text_[position_] >= '1' && text_[position_] <= '9') {
      consume_digits();
    } else {
      return invalid_number();
    }
    bool has_fraction = false;
    bool has_exponent = false;
    if (position_ < text_.size() && text_[position_] == '.') {
      has_fraction = true;
      ++position_;
      if (!consume_digits()) {
        return invalid_number();
      }
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      has_exponent = true;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
        ++position_;
      }
      if (!consume_digits()) {
        return invalid_number();
      }
    }
    if (position_ < text_.size() && text_[position_] != ',' && text_[position_] != '}'
        && text_[position_] != ' ' && text_[position_] != '\t'
        && text_[position_] != '\r' && text_[position_] != '\n') {
      return invalid_number();
    }
    member.value = text_.substr(start, position_ - start);
    if (!has_fraction && !has_exponent) {
      errno = 0;
      char *end = nullptr;
      ::strtoll(member.value.c_str(), &end, 10);
      member.type = (errno != ERANGE && end != nullptr && *end == '\0') ? Type::Bigint : Type::Double;
    } else {
      member.type = Type::Double;
    }
    return true;
  }

  const BudgetString &text_;
  size_t position_;
};

// ---------------------------------------------------------------------------
// Row producers
// ---------------------------------------------------------------------------

/**
 * @brief Source of the rows of a file.
 */
class RowProducer {
public:
  virtual ~RowProducer() {}
  virtual bool next_row(BudgetVector<WorkingCell> &cells) = 0;
  virtual void reset() = 0;
};

class CsvRowProducer : public RowProducer {
public:
  CsvRowProducer(const WorkingDefinition &definition, const BudgetVector<int> &projection,
                 const std::shared_ptr<LocalFile> &file)
      : definition_(definition), projection_(projection), reader_(file, definition.options)
  {
    if (definition_.options.header) {
      skip_header();
    }
  }

  bool next_row(BudgetVector<WorkingCell> &cells) override
  {
    BudgetVector<BudgetString> values;
    BudgetVector<bool> quoted;
    if (!reader_.next_record(values, quoted)) {
      return false;
    }
    if (values.size() != definition_.columns.size()) {
      throw reader_.error("the CSV row has " + budget_to_string(values.size()) + " values but the file has "
                          + budget_to_string(definition_.columns.size()) + " columns");
    }
    cells.clear();
    cells.reserve(projection_.size());
    for (size_t index = 0; index < projection_.size(); ++index) {
      const size_t column = static_cast<size_t>(projection_[index]);
      WorkingCell cell;
      cell.is_null = !quoted[column] && values[column] == definition_.options.null_string;
      const Type type = definition_.columns[column].type;
      cell.text = cell.is_null || type == Type::Varchar || type == Type::Null
          ? values[column] : trim(values[column]);
      cells.push_back(cell);
    }
    return true;
  }

  void reset() override
  {
    reader_.reset();
    if (definition_.options.header) {
      skip_header();
    }
  }

private:
  void skip_header()
  {
    // the header describes the columns, it is not a row
    BudgetVector<BudgetString> values;
    BudgetVector<bool> quoted;
    reader_.next_record(values, quoted);
  }

  WorkingDefinition definition_;
  BudgetVector<int> projection_;
  CsvRecordReader reader_;
};

class JsonRowProducer : public RowProducer {
public:
  JsonRowProducer(const WorkingDefinition &definition, const BudgetVector<int> &projection,
                  const std::shared_ptr<LocalFile> &file)
      : definition_(definition), projection_(projection), source_(file)
  {
    for (size_t index = 0; index < definition_.columns.size(); ++index) {
      names_.push_back(lowercase(definition_.columns[index].name));
    }
  }

  bool next_row(BudgetVector<WorkingCell> &cells) override
  {
    BudgetString line;
    while (source_.read_line(line)) {
      if (!line.empty() && line[line.size() - 1] == '\r') {
        line.erase(line.size() - 1);
      }
      if (trim(line).empty()) {
        continue;
      }
      BudgetVector<JsonMember> members;
      BudgetString error;
      JsonLineParser parser(line);
      if (!parser.parse(members, error)) {
        throw source_.error(error);
      }
      cells.clear();
      cells.reserve(projection_.size());
      for (size_t index = 0; index < projection_.size(); ++index) {
        const BudgetString &name = names_[static_cast<size_t>(projection_[index])];
        WorkingCell cell;
        cell.is_null = true;
        for (size_t member = 0; member < members.size(); ++member) {
          if (lowercase(members[member].key) == name) {
            cell.text = members[member].value;
            cell.is_null = members[member].is_null;
            break;
          }
        }
        cells.push_back(cell);
      }
      return true;
    }
    return false;
  }

  void reset() override { source_.rewind(); }

private:
  WorkingDefinition definition_;
  BudgetVector<int> projection_;
  BudgetVector<BudgetString> names_;
  ByteSource source_;
};

class ParquetRowProducer : public RowProducer {
public:
  ParquetRowProducer(const WorkingDefinition &definition, const BudgetVector<int> &projection,
                     const std::shared_ptr<LocalFile> &file)
      : definition_(definition), projection_(projection)
  {
    scanner_.open(file);
    scanner_.set_projection(projection);
  }

  bool next_row(BudgetVector<WorkingCell> &cells) override
  {
    BudgetVector<BudgetString> texts;
    BudgetVector<bool> nulls;
    if (!scanner_.next_row(texts, nulls)) {
      return false;
    }
    cells.clear();
    cells.reserve(texts.size());
    for (size_t index = 0; index < texts.size(); ++index) {
      WorkingCell cell;
      cell.text = texts[index];
      cell.is_null = nulls[index];
      cells.push_back(cell);
    }
    return true;
  }

  void reset() override { scanner_.reset(); }

private:
  WorkingDefinition definition_;
  BudgetVector<int> projection_;
  ParquetScanner scanner_;
};

// ---------------------------------------------------------------------------
// Schema inference
// ---------------------------------------------------------------------------

void consume_csv_row(const BudgetVector<BudgetString> &values,
                     const BudgetVector<bool> &quoted,
                     const WorkingOptions &options,
                     BudgetVector<Type> &types,
                     BudgetVector<int> &scales,
                     BudgetVector<bool> &nullable,
                     const CsvRecordReader &reader)
{
  for (size_t index = 0; index < values.size(); ++index) {
    if (!is_valid_utf8(values[index].data(), values[index].size())) {
      throw reader.error("the CSV file contains a value that is not valid UTF-8");
    }
    const bool is_null = !quoted[index] && values[index] == options.null_string;
    if (is_null) {
      nullable[index] = true;
      continue;
    }
    Type type = Type::Null;
    if (values[index].empty()) {
      type = Type::Varchar;
    } else {
      int scale = 0;
      const BudgetString trimmed = trim(values[index]);
      type = infer_type(trimmed);
      if (type == Type::Datetime) {
        classify_temporal(trimmed, scale);
        scales[index] = std::max(scales[index], scale);
      }
    }
    types[index] = widen(types[index], type);
  }
}

void infer_csv(const std::shared_ptr<LocalFile> &file, WorkingDefinition &definition)
{
  const WorkingOptions &options = definition.options;
  CsvRecordReader reader(file, options);
  BudgetVector<BudgetString> values;
  BudgetVector<bool> quoted;
  const bool has_record = reader.next_record(values, quoted);
  if (!has_record) {
    throw Error("the CSV file is empty");
  }
  const size_t column_count = values.size();
  BudgetVector<BudgetString> names;
  if (options.header) {
    names = unique_names(values);
  } else {
    for (size_t index = 0; index < column_count; ++index) {
      names.push_back("column" + budget_to_string(index));
    }
  }
  BudgetVector<Type> types(column_count, Type::Null);
  BudgetVector<int> scales(column_count, 0);
  BudgetVector<bool> nullable(column_count, false);
  if (!options.header) {
    consume_csv_row(values, quoted, options, types, scales, nullable, reader);
  }
  while (reader.next_record(values, quoted)) {
    if (values.size() != column_count) {
      throw reader.error("the CSV row has " + budget_to_string(values.size()) + " values but the file has "
                         + budget_to_string(column_count) + " columns");
    }
    consume_csv_row(values, quoted, options, types, scales, nullable, reader);
  }
  definition.columns.clear();
  for (size_t index = 0; index < column_count; ++index) {
    WorkingColumn column;
    column.name = names[index];
    column.type = types[index];
    column.nullable = nullable[index];
    column.scale = scales[index];
    definition.columns.push_back(column);
  }
}

void infer_json(const std::shared_ptr<LocalFile> &file, WorkingDefinition &definition)
{
  ByteSource source(file);
  BudgetVector<BudgetString> names;
  BudgetMap<BudgetString, size_t> lookup;
  BudgetVector<Type> types;
  BudgetVector<int> scales;
  BudgetVector<bool> nullable;
  BudgetVector<int> occurrences;
  size_t row_count = 0;
  BudgetString line;
  while (source.read_line(line)) {
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    if (trim(line).empty()) {
      continue;
    }
    BudgetVector<JsonMember> members;
    BudgetString error;
    JsonLineParser parser(line);
    if (!parser.parse(members, error)) {
      throw source.error(error);
    }
    BudgetMap<BudgetString, bool> seen;
    for (size_t index = 0; index < members.size(); ++index) {
      const BudgetString key = lowercase(members[index].key);
      if (seen.count(key) > 0) {
        throw source.error("the JSON object has the duplicated key " + members[index].key);
      }
      seen[key] = true;
      BudgetMap<BudgetString, size_t>::iterator found = lookup.find(key);
      if (found == lookup.end()) {
        check_count(names.size() + 1, MAX_COLUMNS, "JSON column");
        found = lookup.insert(std::make_pair(key, names.size())).first;
        names.push_back(members[index].key);
        types.push_back(Type::Null);
        scales.push_back(0);
        nullable.push_back(false);
        occurrences.push_back(0);
      }
      const size_t column = found->second;
      ++occurrences[column];
      if (members[index].is_null) {
        nullable[column] = true;
        continue;
      }
      Type type = members[index].type;
      if (type == Type::Datetime) {
        int scale = 0;
        classify_temporal(trim(members[index].value), scale);
        scales[column] = std::max(scales[column], scale);
      }
      types[column] = widen(types[column], type);
    }
    ++row_count;
  }
  definition.columns.clear();
  for (size_t index = 0; index < names.size(); ++index) {
    WorkingColumn column;
    column.name = names[index];
    column.type = types[index];
    column.scale = scales[index];
    column.nullable = nullable[index] || static_cast<size_t>(occurrences[index]) < row_count;
    definition.columns.push_back(column);
  }
}

void infer_parquet(const std::shared_ptr<LocalFile> &file, WorkingDefinition &definition)
{
  ParquetScanner scanner;
  scanner.open(file);
  definition.columns.clear();
  const BudgetVector<ParquetColumn> &schema = scanner.schema();
  for (size_t index = 0; index < schema.size(); ++index) {
    WorkingColumn column;
    column.name = schema[index].name;
    column.type = schema[index].type;
    column.nullable = schema[index].nullable;
    definition.columns.push_back(column);
  }
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void append_u32(BudgetString &blob, uint32_t value)
{
  for (int index = 0; index < 4; ++index) {
    blob.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
  }
}

void append_u64(BudgetString &blob, uint64_t value)
{
  for (int index = 0; index < 8; ++index) {
    blob.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
  }
}

void append_i64(BudgetString &blob, int64_t value)
{
  append_u64(blob, static_cast<uint64_t>(value));
}

void append_string(BudgetString &blob, std::string_view value)
{
  check_count(value.size(), MAX_RECORD_BYTES, "serialized string");
  append_u32(blob, static_cast<uint32_t>(value.size()));
  blob.append(value.data(), value.size());
}

class BlobReader {
public:
  explicit BlobReader(const std::string &blob) : blob_(blob), position_(0) {}

  uint32_t u32()
  {
    uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      value |= static_cast<uint32_t>(byte()) << (8 * index);
    }
    return value;
  }

  uint64_t u64()
  {
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
      value |= static_cast<uint64_t>(byte()) << (8 * index);
    }
    return value;
  }

  int64_t i64() { return static_cast<int64_t>(u64()); }

  uint8_t u8() { return static_cast<uint8_t>(byte()); }

  BudgetString str()
  {
    const uint32_t length = u32();
    check_count(length, MAX_RECORD_BYTES, "serialized string");
    if (length > blob_.size() - position_) {
      throw Error("the serialized definition is corrupted");
    }
    const BudgetString value(blob_.data() + position_, length);
    position_ += length;
    return value;
  }

private:
  uint8_t byte()
  {
    if (position_ >= blob_.size()) {
      throw Error("the serialized definition is corrupted");
    }
    return static_cast<uint8_t>(blob_[position_++]);
  }

  const std::string &blob_;
  size_t position_;
};

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

const char *type_name(Type type)
{
  switch (type) {
    case Type::Boolean:
      return "BOOLEAN";
    case Type::Bigint:
      return "BIGINT";
    case Type::Double:
      return "DOUBLE";
    case Type::Date:
      return "DATE";
    case Type::Datetime:
      return "DATETIME";
    case Type::Null:
      return "NULL";
    default:
      return "VARCHAR";
  }
}

Type infer_type(std::string_view text)
{
  BudgetScope scope;
  const BudgetString value = trim(text);
  if (value.empty()) {
    return Type::Null;
  }
  const BudgetString lowered = lowercase(value);
  if (lowered == "true" || lowered == "false") {
    return Type::Boolean;
  }
  if (looks_like_integer(value)) {
    errno = 0;
    char *end = nullptr;
    ::strtoll(value.c_str(), &end, 10);
    if (errno == ERANGE) {
      return Type::Varchar;
    }
    return Type::Bigint;
  }
  if (looks_like_double(value)) {
    return Type::Double;
  }
  int scale = 0;
  const Type temporal = classify_temporal(value, scale);
  if (temporal != Type::Null) {
    return temporal;
  }
  return Type::Varchar;
}

Type widen(Type left, Type right)
{
  if (left == right) {
    return left;
  }
  if (left == Type::Null) {
    return right;
  }
  if (right == Type::Null) {
    return left;
  }
  if (left == Type::Varchar || right == Type::Varchar) {
    return Type::Varchar;
  }
  if ((left == Type::Bigint && right == Type::Double) || (left == Type::Double && right == Type::Bigint)) {
    return Type::Double;
  }
  if ((left == Type::Date && right == Type::Datetime) || (left == Type::Datetime && right == Type::Date)) {
    return Type::Datetime;
  }
  return Type::Varchar;
}

Format resolve_format(const std::string &path, const Options &options)
{
  check_count(path.size(), 4096, "path");
  if (options.format == Format::Csv) {
    return Format::Csv;
  }
  if (options.format == Format::Json) {
    return Format::Json;
  }
  if (options.format == Format::Parquet) {
    return Format::Parquet;
  }
  const std::string extension = extension_of(path);
  if (extension == "csv") {
    return Format::Csv;
  }
  if (extension == "jsonl" || extension == "ndjson" || extension == "json") {
    return Format::Json;
  }
  if (extension == "parquet") {
    return Format::Parquet;
  }
  throw Error("the file extension of " + path + " is not supported");
}

std::string resolve_within_root(const std::string &path, const std::string &root)
{
  if (root.empty() || root == "NULL") {
    throw Error("the directory allowed to read files from is not configured");
  }
  const std::string canonical_root = canonical_path(root);
  if (!is_directory(canonical_root)) {
    throw Error("the directory allowed to read files from is not a directory");
  }
  const std::string resolved = canonical_path(path);
  if (!is_within(resolved, canonical_root)) {
    throw Error("the file " + path + " is outside of the directory allowed to read files from");
  }
  return resolved;
}

Definition infer(const std::string &path, const std::string &root, const Options &options)
{
  BudgetScope scope;
  MemoryReservation bookkeeping(scope.budget(), 64 * 1024);
  if (options.sample_size != -1) {
    throw Error("only sample_size = -1 is supported");
  }
  auto file = std::make_shared<LocalFile>(LocalFile::open_read(path, root));
  WorkingDefinition definition;
  definition.path.assign(file->path().data(), file->path().size());
  definition.options = WorkingOptions(options);
  definition.options.format = resolve_format(file->path(), options);
  const FileIdentity identity = file->identity();
  definition.file_size = identity.size;
  definition.file_mtime_ns = identity.mtime_ns;
  definition.file_device = identity.device;
  definition.file_inode = identity.inode;
  definition.file_ctime_ns = identity.ctime_ns;
  switch (definition.options.format) {
    case Format::Csv:
      infer_csv(file, definition);
      break;
    case Format::Json:
      infer_json(file, definition);
      break;
    case Format::Parquet:
      infer_parquet(file, definition);
      break;
    default:
      throw Error("the file format is not supported");
  }
  file->validate_unchanged(identity);
  return export_definition(definition);
}

std::vector<std::string> discover(const std::string &pattern, const std::string &root,
                                  MemoryReservation *result_memory)
{
  BudgetScope scope;
  MemoryReservation bookkeeping(scope.budget(), 64 * 1024);
  check_count(pattern.size(), 4096, "glob pattern");
  if (pattern.find('\0') != std::string::npos) {
    throw Error("file SQL glob pattern contains NUL");
  }
  auto directory = LocalFile::open_directory(directory_of(pattern), root);
  const auto identity = directory.identity();
  const std::string mask = basename_of(pattern);
  const int fd = ::fcntl(directory.descriptor(), F_DUPFD_CLOEXEC, 0);
  if (fd < 0) {
    throw Error("cannot duplicate file SQL directory descriptor");
  }
  DIR *raw_handle = ::fdopendir(fd);
  if (raw_handle == nullptr) {
    ::close(fd);
    throw Error("cannot list the directory " + directory.path());
  }
  const auto close_directory = [](DIR *handle) { ::closedir(handle); };
  std::unique_ptr<DIR, decltype(close_directory)> handle(raw_handle, close_directory);
  BudgetVector<BudgetString> matches;
  size_t entries = 0;
  while (true) {
    if ((entries++ & 255) == 0) check_work();
    errno = 0;
    struct dirent *entry = ::readdir(handle.get());
    if (entry == nullptr) {
      if (errno != 0) {
        throw Error("cannot read the directory " + directory.path());
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == ".." || (!mask.empty() && ::fnmatch(mask.c_str(), name.c_str(), 0) != 0)) {
      continue;
    }
    const std::string candidate = directory.path() + "/" + name;
    auto file = LocalFile::open_read(candidate, root);
    check_count(matches.size() + 1, MAX_GLOB_MATCHES, "glob match");
    matches.emplace_back(file.path().data(), file.path().size());
  }
  directory.validate_unchanged(identity);
  std::sort(matches.begin(), matches.end());
  MemoryReservation local_output(scope.budget());
  MemoryReservation &output = result_memory ? *result_memory : local_output;
  reserve_standard_copy(output, matches.size() * sizeof(std::string));
  for (const auto &path : matches) reserve_standard_copy(output, path.size() + 1);
  std::vector<std::string> result;
  result.reserve(matches.size());
  for (const auto &path : matches) result.emplace_back(path.data(), path.size());
  return result;
}

std::string Definition::serialize(MemoryReservation *result_memory) const
{
  BudgetScope scope;
  MemoryReservation bookkeeping(scope.budget(), 64 * 1024);
  check_count(columns.size(), MAX_COLUMNS, "column");
  BudgetString blob;
  append_u32(blob, 0x46445346u); // "FSDF"
  append_u32(blob, 2);
  append_string(blob, path);
  append_u32(blob, static_cast<uint32_t>(options.format));
  blob.push_back(options.header ? '\1' : '\0');
  blob.push_back(options.delimiter);
  blob.push_back(options.quote);
  blob.push_back(options.escape);
  append_string(blob, options.null_string);
  append_i64(blob, options.sample_size);
  append_u64(blob, file_size);
  append_i64(blob, file_mtime_ns);
  append_u64(blob, file_device);
  append_u64(blob, file_inode);
  append_i64(blob, file_ctime_ns);
  append_u32(blob, static_cast<uint32_t>(columns.size()));
  for (size_t index = 0; index < columns.size(); ++index) {
    append_string(blob, columns[index].name);
    append_u32(blob, static_cast<uint32_t>(columns[index].type));
    blob.push_back(columns[index].nullable ? '\1' : '\0');
    append_u32(blob, static_cast<uint32_t>(columns[index].scale));
  }
  MemoryReservation local_output(scope.budget());
  MemoryReservation &output = result_memory ? *result_memory : local_output;
  reserve_standard_copy(output, blob.size() + 1);
  return std::string(blob.data(), blob.size());
}

Definition Definition::deserialize(const std::string &blob)
{
  BudgetScope scope;
  MemoryReservation bookkeeping(scope.budget(), 64 * 1024);
  BlobReader reader(blob);
  if (reader.u32() != 0x46445346u) {
    throw Error("the serialized definition has an unknown format");
  }
  if (reader.u32() != 2) {
    throw Error("the serialized definition has an unknown version");
  }
  WorkingDefinition definition;
  definition.path = reader.str();
  definition.options.format = static_cast<Format>(reader.u32());
  definition.options.header = reader.u8() != 0;
  definition.options.delimiter = static_cast<char>(reader.u8());
  definition.options.quote = static_cast<char>(reader.u8());
  definition.options.escape = static_cast<char>(reader.u8());
  definition.options.null_string = reader.str();
  definition.options.sample_size = reader.i64();
  definition.file_size = reader.u64();
  definition.file_mtime_ns = reader.i64();
  definition.file_device = reader.u64();
  definition.file_inode = reader.u64();
  definition.file_ctime_ns = reader.i64();
  const uint32_t count = reader.u32();
  check_count(count, MAX_COLUMNS, "column");
  for (uint32_t index = 0; index < count; ++index) {
    WorkingColumn column;
    column.name = reader.str();
    column.type = static_cast<Type>(reader.u32());
    column.nullable = reader.u8() != 0;
    column.scale = static_cast<int>(reader.u32());
    definition.columns.push_back(column);
  }
  return export_definition(definition);
}

class Scan::ScanImpl {
public:
  ScanImpl(const Definition &definition, const std::string &root, const std::vector<int> &projection)
      : budget_(active_file_sql_budget ? active_file_sql_budget : std::make_shared<MemoryBudget>()),
        bookkeeping_(budget_, 64 * 1024), output_memory_(budget_)
  {
    BudgetScope scope(budget_);
    check_count(projection.size(), MAX_COLUMNS, "projection");
    definition_ = WorkingDefinition(definition);
    projection_.assign(projection.begin(), projection.end());
    for (size_t index = 0; index < projection_.size(); ++index) {
      if (projection_[index] < 0 || static_cast<size_t>(projection_[index]) >= definition_.columns.size()) {
        throw Error("the column projection is out of range");
      }
    }
    file_ = std::make_shared<LocalFile>(LocalFile::open_read(definition.path, root));
    check_unchanged();
    switch (definition_.options.format) {
      case Format::Csv:
        producer_.reset(new CsvRowProducer(definition_, projection_, file_));
        break;
      case Format::Json:
        producer_.reset(new JsonRowProducer(definition_, projection_, file_));
        break;
      case Format::Parquet:
        producer_.reset(new ParquetRowProducer(definition_, projection_, file_));
        break;
      default:
        throw Error("the file format is not supported");
    }
  }

  bool next(std::vector<Cell> &row)
  {
    BudgetScope scope(budget_);
    // Actual reads validate their held descriptor before/after I/O. Validate
    // again before publishing any row (including buffered rows) or EOF.
    row.clear();
    output_memory_.clear();
    reserve_standard_copy(output_memory_, row.capacity() * sizeof(Cell));
    BudgetVector<WorkingCell> cells;
    const bool has_row = producer_->next_row(cells);
    check_unchanged();
    if (!has_row) {
      return false;
    }
    MemoryReservation &output = output_memory_;
    reserve_standard_copy(output, cells.size() * sizeof(Cell));
    for (const auto &cell : cells) {
      reserve_standard_copy(output, cell.text.size() + 1);
    }
    row.reserve(cells.size());
    for (const auto &cell : cells) {
      row.push_back(Cell{std::string(cell.text.data(), cell.text.size()), cell.is_null});
    }
    return true;
  }

  void rescan()
  {
    BudgetScope scope(budget_);
    check_unchanged();
    producer_->reset();
  }

private:
  void check_unchanged()
  {
    const FileIdentity expected{definition_.file_device, definition_.file_inode,
                                definition_.file_size, definition_.file_mtime_ns, definition_.file_ctime_ns};
    file_->validate_unchanged(expected);
  }

  std::shared_ptr<MemoryBudget> budget_;
  MemoryReservation bookkeeping_;
  MemoryReservation output_memory_;
  WorkingDefinition definition_;
  BudgetVector<int> projection_;
  std::shared_ptr<LocalFile> file_;
  std::unique_ptr<RowProducer> producer_;
};

Scan::Scan(const Definition &definition, const std::string &root, const std::vector<int> &projection)
    : impl_(new ScanImpl(definition, root, projection))
{
}

Scan::~Scan() {}

bool Scan::next(std::vector<Cell> &row) { return impl_->next(row); }

void Scan::rescan() { impl_->rescan(); }
class CsvWriter::CsvWriterImpl {
public:
  CsvWriterImpl()
      : budget_(active_file_sql_budget ? active_file_sql_budget : std::make_shared<MemoryBudget>()),
        bookkeeping_(budget_, 64 * 1024) {}

  void open(const Definition &output, const std::string &root)
  {
    BudgetScope scope(budget_);
    check_count(output.path.size(), PATH_MAX - 1, "output path");
    if (output.path.find('\0') != std::string::npos) {
      throw Error("file SQL output path contains NUL");
    }
    check_count(output.columns.size(), MAX_COLUMNS, "output column");
    check_count(root.size(), PATH_MAX - 1, "output root");
    check_count(output.options.null_string.size(), MAX_RECORD_BYTES, "NULL marker");
    size_t header_bytes = 0;
    for (const auto &column : output.columns) {
      check_count(column.name.size(), MAX_RECORD_BYTES - header_bytes, "CSV output header");
      header_bytes += column.name.size();
    }
    reserve_standard_copy(bookkeeping_, output.options.null_string.size() + 1);
    options_ = output.options;
    if (options_.delimiter == '\0' || options_.delimiter == '\n' || options_.delimiter == '\r'
        || options_.quote == '\0' || options_.quote == '\n' || options_.quote == '\r'
        || options_.delimiter == options_.quote || options_.escape == '\0'
        || options_.escape == '\r' || options_.escape == '\n'
        || options_.null_string.find_first_of(std::string("\0\r\n", 3)) != std::string::npos
        || options_.null_string.find(options_.delimiter) != std::string::npos
        || options_.null_string.find(options_.quote) != std::string::npos) {
      throw Error("invalid CSV output options");
    }
    root_ = root;
    target_ = basename_of(output.path);
    if (target_.empty() || target_ == "." || target_ == "..") {
      throw Error("invalid file SQL output name");
    }
    directory_.reset(new LocalFile(LocalFile::open_directory(directory_of(output.path), root)));
    struct stat info{};
    if (::fstatat(directory_->descriptor(), target_.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0
        || errno != ENOENT) {
      throw Error("file SQL output already exists or is inaccessible");
    }
    std::random_device random;
    for (int attempt = 0; attempt < 128 && fd_ < 0; ++attempt) {
      temp_ = ".seekdb-copy-" + std::to_string(::getpid()) + "-"
              + std::to_string(random()) + "-" + std::to_string(random());
      fd_ = ::openat(directory_->descriptor(), temp_.c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd_ < 0 && errno != EEXIST) {
        throw Error("cannot create exclusive file SQL temporary output");
      }
    }
    if (fd_ < 0) {
      throw Error("cannot allocate file SQL temporary output name");
    }
    if (options_.header) {
      MemoryReservation header_memory(budget_);
      reserve_standard_copy(header_memory, output.columns.size() * sizeof(Field) + header_bytes);
      std::vector<Field> header;
      header.reserve(output.columns.size());
      for (const auto &column : output.columns) {
        header.push_back({false, column.name, Type::Varchar});
      }
      write_row(header);
    }
  }

  void write(const std::vector<Field> &fields)
  {
    if (committed_ || fd_ < 0) {
      throw Error("file SQL output is not writable");
    }
    write_row(fields);
  }

  void commit()
  {
    BudgetScope scope(budget_);
    check_work();
    if (committed_ || fd_ < 0) {
      throw Error("file SQL output is not writable");
    }
    validate_directory();
    if (::fsync(fd_) != 0) {
      throw Error("cannot flush file SQL output");
    }
    check_work();
    // Link the held inode, never a replaceable temporary pathname. linkat
    // publishes exclusively: every existing directory entry yields EEXIST.
    int result = ::linkat(fd_, "", directory_->descriptor(), target_.c_str(), AT_EMPTY_PATH);
    if (result != 0 && (errno == EPERM || errno == ENOENT || errno == EINVAL)) {
      const std::string descriptor = "/proc/self/fd/" + std::to_string(fd_);
      result = ::linkat(AT_FDCWD, descriptor.c_str(), directory_->descriptor(),
                        target_.c_str(), AT_SYMLINK_FOLLOW);
    }
    if (result != 0) {
      throw Error("cannot publish file SQL output without replacing an existing entry");
    }
    try {
      validate_directory();
    } catch (...) {
      unlink_owned(target_);
      throw;
    }
    struct stat identity{};
    if (::fstat(fd_, &identity) != 0) {
      unlink_owned(target_);
      throw Error("cannot stat file SQL output before close");
    }
    unlink_owned(temp_);
    const int close_result = ::close(fd_);
    fd_ = -1;
    temp_.clear();
    if (close_result != 0) {
      unlink_inode(target_, identity);
      throw Error("cannot close file SQL output");
    }
    committed_ = true;
  }

  ~CsvWriterImpl() { cleanup(); }

private:
  void validate_directory()
  {
    const auto current = LocalFile::open_directory(directory_->path(), root_);
    const auto original = directory_->identity();
    const auto identity = current.identity();
    if (original.device != identity.device || original.inode != identity.inode) {
      throw Error("file SQL output directory changed");
    }
  }

  void unlink_inode(const std::string &name, const struct stat &held) noexcept
  {
    struct stat entry{};
    if (directory_
        && ::fstatat(directory_->descriptor(), name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0
        && held.st_dev == entry.st_dev && held.st_ino == entry.st_ino) {
      ::unlinkat(directory_->descriptor(), name.c_str(), 0);
    }
  }

  void unlink_owned(const std::string &name) noexcept
  {
    struct stat held{};
    if (fd_ >= 0 && ::fstat(fd_, &held) == 0) unlink_inode(name, held);
  }

  void cleanup() noexcept
  {
    if (!temp_.empty()) {
      unlink_owned(temp_);
      temp_.clear();
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  void write_row(const std::vector<Field> &fields)
  {
    check_count(fields.size(), MAX_COLUMNS, "output column");
    BudgetScope scope(budget_);
    check_work();
    BudgetString row;
    auto append = [&](std::string_view text) {
      if (text.size() > MAX_RECORD_BYTES - row.size()) {
        throw Error("CSV output record exceeds the size limit");
      }
      row.append(text.data(), text.size());
    };
    auto byte = [&](char value) { append(std::string_view(&value, 1)); };
    for (size_t i = 0; i < fields.size(); ++i) {
      if (i != 0) byte(options_.delimiter);
      const auto &field = fields[i];
      if (field.is_null) {
        append(options_.null_string);
        continue;
      }
      const auto &text = field.text;
      const bool quote = text.empty() || text == options_.null_string
          || text.find_first_of(std::string{options_.delimiter, options_.quote, options_.escape, '\n', '\r'})
             != std::string::npos || text.front() == ' ' || text.back() == ' ';
      if (quote) byte(options_.quote);
      size_t position = 0;
      for (const char value : text) {
        if ((position++ & 65535) == 0) check_work();
        if (quote && (value == options_.quote || value == options_.escape)) byte(options_.escape);
        byte(value);
      }
      if (quote) byte(options_.quote);
    }
    byte('\n');
    size_t position = 0;
    while (position < row.size()) {
      check_work();
      const ssize_t count = ::write(fd_, row.data() + position, row.size() - position);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw Error("cannot write file SQL output");
      position += static_cast<size_t>(count);
    }
  }

  std::shared_ptr<MemoryBudget> budget_;
  MemoryReservation bookkeeping_;
  Options options_;
  std::string root_;
  std::string target_;
  std::string temp_;
  std::unique_ptr<LocalFile> directory_;
  int fd_ = -1;
  bool committed_ = false;
};

CsvWriter::CsvWriter(const Definition &output, const std::string &root) : impl_(new CsvWriterImpl())
{
  impl_->open(output, root);
}

CsvWriter::~CsvWriter() {}

void CsvWriter::write(const std::vector<Field> &fields) { impl_->write(fields); }

void CsvWriter::commit() { impl_->commit(); }

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
