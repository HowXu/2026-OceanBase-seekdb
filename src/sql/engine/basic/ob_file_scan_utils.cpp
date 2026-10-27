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

#include <algorithm>
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

std::string lowercase(const std::string &value)
{
  std::string result(value);
  for (char &item : result) {
    item = static_cast<char>(::tolower(static_cast<unsigned char>(item)));
  }
  return result;
}

std::string trim(const std::string &value)
{
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && ::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  while (end > begin && ::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
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

bool parse_digits(const std::string &value, size_t offset, size_t count, int &result)
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

bool looks_like_integer(const std::string &value)
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

bool looks_like_double(const std::string &value)
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
Type classify_temporal(const std::string &value, int &scale)
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
  return lowercase(base.substr(dot + 1));
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

/**
 * @brief The identity of a file, used to detect that it changed.
 */
struct Fingerprint {
  uint64_t size = 0;
  int64_t mtime_ns = 0;
};

bool stat_path(const std::string &path, struct stat &info)
{
  return ::stat(path.c_str(), &info) == 0;
}

Fingerprint fingerprint_of(const std::string &path)
{
  struct stat info;
  if (!stat_path(path, info)) {
    throw Error("cannot stat the file " + path);
  }
  Fingerprint fingerprint;
  fingerprint.size = static_cast<uint64_t>(info.st_size);
#if defined(__APPLE__) || defined(__FreeBSD__)
  fingerprint.mtime_ns = static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000000000
                         + static_cast<int64_t>(info.st_mtimespec.tv_nsec);
#else
  fingerprint.mtime_ns = static_cast<int64_t>(info.st_mtim.tv_sec) * 1000000000
                         + static_cast<int64_t>(info.st_mtim.tv_nsec);
#endif
  return fingerprint;
}

std::string canonical_path(const std::string &path)
{
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
  if (path == root) {
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
std::vector<std::string> unique_names(const std::vector<std::string> &raw_names)
{
  std::vector<std::string> names;
  std::map<std::string, int> counters;
  for (const std::string &raw_name : raw_names) {
    const std::string base = trim(raw_name);
    const std::string key = lowercase(base);
    std::map<std::string, int>::iterator found = counters.find(key);
    if (found == counters.end()) {
      counters[key] = 1;
      names.push_back(base);
      continue;
    }
    int count = ++found->second;
    std::string candidate = key + "__" + std::to_string(count);
    while (std::find(names.begin(), names.end(), candidate) != names.end()) {
      candidate = key + "__" + std::to_string(++count);
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
  explicit ByteSource(const std::string &path) : stream_(path.c_str(), std::ios::binary)
  {
    if (!stream_.good()) {
      throw Error("cannot open the file " + path);
    }
  }

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
      ++position_;
    }
    return item;
  }

  void rewind()
  {
    stream_.clear();
    stream_.seekg(0, std::ios::beg);
    position_ = 0;
    buffer_.clear();
  }

private:
  bool fill()
  {
    buffer_.resize(64 * 1024);
    stream_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    const std::streamsize count = stream_.gcount();
    buffer_.resize(static_cast<size_t>(count));
    position_ = 0;
    return count > 0;
  }

  std::ifstream stream_;
  std::vector<char> buffer_;
  size_t position_ = 0;
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
  CsvRecordReader(const std::string &path, const Options &options) : path_(path), options_(options), source_(path)
  {
    skip_bom();
  }

  bool next_record(std::vector<std::string> &values, std::vector<bool> &quoted)
  {
    while (true) {
      values.clear();
      quoted.clear();
      if (source_.peek() < 0) {
        return false;
      }
      while (true) {
        std::string value;
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
          if (source_.peek() == '\n') {
            source_.get();
          }
          break;
        }
        if (separator == '\n') {
          source_.get();
          break;
        }
        break;
      }
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

  void read_quoted_value(std::string &value)
  {
    while (true) {
      const int item = source_.get();
      if (item < 0) {
        throw Error("the CSV file has an unterminated quoted value");
      }
      if (options_.escape != options_.quote && item == options_.escape) {
        const int escaped = source_.get();
        if (escaped < 0) {
          throw Error("the CSV file has an unterminated quoted value");
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
    // the characters between the closing quote and the separator are ignored
    while (source_.peek() == ' ' || source_.peek() == '\t') {
      source_.get();
    }
  }

  void read_plain_value(std::string &value)
  {
    while (true) {
      const int item = source_.peek();
      if (item < 0 || item == options_.delimiter || item == '\n' || item == '\r') {
        break;
      }
      value.push_back(static_cast<char>(source_.get()));
    }
  }

  std::string path_;
  Options options_;
  ByteSource source_;
};

// ---------------------------------------------------------------------------
// JSON reader
// ---------------------------------------------------------------------------

struct JsonMember {
  std::string key;
  std::string value;
  Type type = Type::Null;
  bool is_null = false;
};

void append_utf8(std::string &out, unsigned int code)
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
  explicit JsonLineParser(const std::string &text) : text_(text), position_(0) {}

  bool parse(std::vector<JsonMember> &members, std::string &error)
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
      skip_space();
      std::string key;
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

  bool parse_string(std::string &out, std::string &error)
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

  bool parse_value(JsonMember &member, std::string &error)
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
    if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
      ++position_;
    }
    bool has_digit = false;
    bool has_fraction = false;
    bool has_exponent = false;
    while (position_ < text_.size()) {
      const char current = text_[position_];
      if (current >= '0' && current <= '9') {
        has_digit = true;
        ++position_;
        continue;
      }
      if (current == '.' && !has_fraction && !has_exponent) {
        has_fraction = true;
        ++position_;
        continue;
      }
      if ((current == 'e' || current == 'E') && has_digit && !has_exponent) {
        has_exponent = true;
        ++position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) {
          ++position_;
        }
        continue;
      }
      break;
    }
    if (!has_digit) {
      error = "the JSON object has an invalid value";
      return false;
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

  const std::string &text_;
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
  virtual bool next_row(std::vector<Cell> &cells) = 0;
  virtual void reset() = 0;
};

class CsvRowProducer : public RowProducer {
public:
  CsvRowProducer(const Definition &definition, const std::vector<int> &projection)
      : definition_(definition), projection_(projection), reader_(definition.path, definition.options)
  {
    if (definition_.options.header) {
      skip_header();
    }
  }

  bool next_row(std::vector<Cell> &cells) override
  {
    std::vector<std::string> values;
    std::vector<bool> quoted;
    if (!reader_.next_record(values, quoted)) {
      return false;
    }
    if (values.size() != definition_.columns.size()) {
      throw Error("the CSV row has " + std::to_string(values.size()) + " values but the file has "
                  + std::to_string(definition_.columns.size()) + " columns");
    }
    cells.clear();
    cells.reserve(projection_.size());
    for (size_t index = 0; index < projection_.size(); ++index) {
      const size_t column = static_cast<size_t>(projection_[index]);
      Cell cell;
      cell.text = values[column];
      cell.is_null = !quoted[column] && values[column] == definition_.options.null_string;
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
    std::vector<std::string> values;
    std::vector<bool> quoted;
    reader_.next_record(values, quoted);
  }

  Definition definition_;
  std::vector<int> projection_;
  CsvRecordReader reader_;
};

class JsonRowProducer : public RowProducer {
public:
  JsonRowProducer(const Definition &definition, const std::vector<int> &projection)
      : definition_(definition), projection_(projection), path_(definition.path)
  {
    for (size_t index = 0; index < definition_.columns.size(); ++index) {
      names_.push_back(lowercase(definition_.columns[index].name));
    }
    open();
  }

  bool next_row(std::vector<Cell> &cells) override
  {
    std::string line;
    while (std::getline(stream_, line)) {
      if (!line.empty() && line[line.size() - 1] == '\r') {
        line.erase(line.size() - 1);
      }
      if (trim(line).empty()) {
        continue;
      }
      std::vector<JsonMember> members;
      std::string error;
      JsonLineParser parser(line);
      if (!parser.parse(members, error)) {
        throw Error(error);
      }
      cells.clear();
      cells.reserve(projection_.size());
      for (size_t index = 0; index < projection_.size(); ++index) {
        const std::string &name = names_[static_cast<size_t>(projection_[index])];
        Cell cell;
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

  void reset() override
  {
    stream_.close();
    open();
  }

private:
  void open()
  {
    stream_.open(path_.c_str(), std::ios::binary);
    if (!stream_.good()) {
      throw Error("cannot open the file " + path_);
    }
  }

  Definition definition_;
  std::vector<int> projection_;
  std::vector<std::string> names_;
  std::string path_;
  std::ifstream stream_;
};

class ParquetRowProducer : public RowProducer {
public:
  ParquetRowProducer(const Definition &definition, const std::vector<int> &projection)
      : definition_(definition), projection_(projection)
  {
    scanner_.open(definition.path);
    scanner_.set_projection(projection);
  }

  bool next_row(std::vector<Cell> &cells) override
  {
    std::vector<std::string> texts;
    std::vector<bool> nulls;
    if (!scanner_.next_row(texts, nulls)) {
      return false;
    }
    cells.clear();
    cells.reserve(texts.size());
    for (size_t index = 0; index < texts.size(); ++index) {
      Cell cell;
      cell.text = texts[index];
      cell.is_null = nulls[index];
      cells.push_back(cell);
    }
    return true;
  }

  void reset() override { scanner_.reset(); }

private:
  Definition definition_;
  std::vector<int> projection_;
  ParquetScanner scanner_;
};

// ---------------------------------------------------------------------------
// Schema inference
// ---------------------------------------------------------------------------

void consume_csv_row(const std::vector<std::string> &values,
                     const std::vector<bool> &quoted,
                     const Options &options,
                     std::vector<Type> &types,
                     std::vector<int> &scales,
                     std::vector<bool> &nullable)
{
  for (size_t index = 0; index < values.size(); ++index) {
    if (!is_valid_utf8(values[index].data(), values[index].size())) {
      throw Error("the CSV file contains a value that is not valid UTF-8");
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
      const std::string trimmed = trim(values[index]);
      type = infer_type(trimmed);
      if (type == Type::Datetime) {
        classify_temporal(trimmed, scale);
        scales[index] = std::max(scales[index], scale);
      }
    }
    types[index] = widen(types[index], type);
  }
}

void infer_csv(const std::string &path, Definition &definition)
{
  const Options &options = definition.options;
  CsvRecordReader reader(path, options);
  std::vector<std::string> values;
  std::vector<bool> quoted;
  const bool has_record = reader.next_record(values, quoted);
  if (!has_record) {
    throw Error("the CSV file is empty");
  }
  const size_t column_count = values.size();
  std::vector<std::string> names;
  if (options.header) {
    names = unique_names(values);
  } else {
    for (size_t index = 0; index < column_count; ++index) {
      names.push_back("column" + std::to_string(index));
    }
  }
  std::vector<Type> types(column_count, Type::Null);
  std::vector<int> scales(column_count, 0);
  std::vector<bool> nullable(column_count, false);
  if (!options.header) {
    consume_csv_row(values, quoted, options, types, scales, nullable);
  }
  while (reader.next_record(values, quoted)) {
    if (values.size() != column_count) {
      throw Error("the CSV row has " + std::to_string(values.size()) + " values but the file has "
                  + std::to_string(column_count) + " columns");
    }
    consume_csv_row(values, quoted, options, types, scales, nullable);
  }
  definition.columns.clear();
  for (size_t index = 0; index < column_count; ++index) {
    Column column;
    column.name = names[index];
    column.type = types[index];
    column.nullable = nullable[index];
    column.scale = scales[index];
    definition.columns.push_back(column);
  }
}

void infer_json(const std::string &path, Definition &definition)
{
  std::ifstream stream(path.c_str(), std::ios::binary);
  if (!stream.good()) {
    throw Error("cannot open the file " + path);
  }
  std::vector<std::string> names;
  std::map<std::string, size_t> lookup;
  std::vector<Type> types;
  std::vector<int> scales;
  std::vector<bool> nullable;
  std::vector<int> occurrences;
  size_t row_count = 0;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    if (trim(line).empty()) {
      continue;
    }
    std::vector<JsonMember> members;
    std::string error;
    JsonLineParser parser(line);
    if (!parser.parse(members, error)) {
      throw Error(error);
    }
    std::map<std::string, bool> seen;
    for (size_t index = 0; index < members.size(); ++index) {
      const std::string key = lowercase(members[index].key);
      if (seen.count(key) > 0) {
        throw Error("the JSON object has the duplicated key " + members[index].key);
      }
      seen[key] = true;
      std::map<std::string, size_t>::iterator found = lookup.find(key);
      if (found == lookup.end()) {
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
  if (row_count == 0) {
    throw Error("the JSON lines file is empty");
  }
  definition.columns.clear();
  for (size_t index = 0; index < names.size(); ++index) {
    Column column;
    column.name = names[index];
    column.type = types[index];
    column.scale = scales[index];
    column.nullable = nullable[index] || static_cast<size_t>(occurrences[index]) < row_count;
    definition.columns.push_back(column);
  }
}

void infer_parquet(const std::string &path, Definition &definition)
{
  ParquetScanner scanner;
  scanner.open(path);
  definition.columns.clear();
  const std::vector<ParquetColumn> &schema = scanner.schema();
  for (size_t index = 0; index < schema.size(); ++index) {
    Column column;
    column.name = schema[index].name;
    column.type = schema[index].type;
    column.nullable = schema[index].nullable;
    definition.columns.push_back(column);
  }
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

void append_u32(std::string &blob, uint32_t value)
{
  for (int index = 0; index < 4; ++index) {
    blob.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
  }
}

void append_u64(std::string &blob, uint64_t value)
{
  for (int index = 0; index < 8; ++index) {
    blob.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
  }
}

void append_i64(std::string &blob, int64_t value)
{
  append_u64(blob, static_cast<uint64_t>(value));
}

void append_string(std::string &blob, const std::string &value)
{
  append_u32(blob, static_cast<uint32_t>(value.size()));
  blob.append(value);
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

  std::string str()
  {
    const uint32_t length = u32();
    if (position_ + length > blob_.size()) {
      throw Error("the serialized definition is corrupted");
    }
    const std::string value = blob_.substr(position_, length);
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

Type infer_type(const std::string &text)
{
  const std::string value = trim(text);
  if (value.empty()) {
    return Type::Null;
  }
  const std::string lowered = lowercase(value);
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
  if (options.sample_size != -1) {
    throw Error("only sample_size = -1 is supported");
  }
  const std::string resolved = resolve_within_root(path, root);
  if (!is_regular_file(resolved)) {
    throw Error("the path " + path + " is not a regular file");
  }
  Definition definition;
  definition.path = resolved;
  definition.options = options;
  definition.options.format = resolve_format(resolved, options);
  const Fingerprint fingerprint = fingerprint_of(resolved);
  definition.file_size = fingerprint.size;
  definition.file_mtime_ns = fingerprint.mtime_ns;
  switch (definition.options.format) {
    case Format::Csv:
      infer_csv(resolved, definition);
      break;
    case Format::Json:
      infer_json(resolved, definition);
      break;
    case Format::Parquet:
      infer_parquet(resolved, definition);
      break;
    default:
      throw Error("the file format is not supported");
  }
  return definition;
}

std::vector<std::string> discover(const std::string &pattern, const std::string &root)
{
  if (root.empty() || root == "NULL") {
    throw Error("the directory allowed to read files from is not configured");
  }
  const std::string canonical_root = canonical_path(root);
  if (!is_directory(canonical_root)) {
    throw Error("the directory allowed to read files from is not a directory");
  }
  const std::string directory = directory_of(pattern);
  const std::string mask = basename_of(pattern);
  const std::string canonical_directory = canonical_path(directory);
  if (!is_within(canonical_directory, canonical_root)) {
    throw Error("the pattern " + pattern + " is outside of the directory allowed to read files from");
  }
  DIR *handle = ::opendir(canonical_directory.c_str());
  if (handle == nullptr) {
    throw Error("cannot list the directory " + canonical_directory);
  }
  std::vector<std::string> matches;
  struct dirent *entry = nullptr;
  while ((entry = ::readdir(handle)) != nullptr) {
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (!mask.empty() && ::fnmatch(mask.c_str(), name.c_str(), 0) != 0) {
      continue;
    }
    const std::string candidate = canonical_directory + "/" + name;
    const std::string resolved = canonical_path(candidate);
    if (!is_within(resolved, canonical_root)) {
      ::closedir(handle);
      throw Error("the file " + candidate + " is outside of the directory allowed to read files from");
    }
    matches.push_back(resolved);
  }
  ::closedir(handle);
  std::sort(matches.begin(), matches.end());
  return matches;
}

std::string Definition::serialize() const
{
  std::string blob;
  append_u32(blob, 0x46445346u); // "FSDF"
  append_u32(blob, 1);
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
  append_u32(blob, static_cast<uint32_t>(columns.size()));
  for (size_t index = 0; index < columns.size(); ++index) {
    append_string(blob, columns[index].name);
    append_u32(blob, static_cast<uint32_t>(columns[index].type));
    blob.push_back(columns[index].nullable ? '\1' : '\0');
    append_u32(blob, static_cast<uint32_t>(columns[index].scale));
  }
  return blob;
}

Definition Definition::deserialize(const std::string &blob)
{
  BlobReader reader(blob);
  if (reader.u32() != 0x46445346u) {
    throw Error("the serialized definition has an unknown format");
  }
  if (reader.u32() != 1) {
    throw Error("the serialized definition has an unknown version");
  }
  Definition definition;
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
  const uint32_t count = reader.u32();
  for (uint32_t index = 0; index < count; ++index) {
    Column column;
    column.name = reader.str();
    column.type = static_cast<Type>(reader.u32());
    column.nullable = reader.u8() != 0;
    column.scale = static_cast<int>(reader.u32());
    definition.columns.push_back(column);
  }
  return definition;
}

class Scan::ScanImpl {
public:
  ScanImpl(const Definition &definition, const std::string &root, const std::vector<int> &projection)
      : definition_(definition), root_(root), projection_(projection)
  {
    for (size_t index = 0; index < projection_.size(); ++index) {
      if (projection_[index] < 0 || static_cast<size_t>(projection_[index]) >= definition_.columns.size()) {
        throw Error("the column projection is out of range");
      }
    }
    path_ = resolve_within_root(definition_.path, root_);
    check_unchanged();
    switch (definition_.options.format) {
      case Format::Csv:
        producer_.reset(new CsvRowProducer(definition_, projection_));
        break;
      case Format::Json:
        producer_.reset(new JsonRowProducer(definition_, projection_));
        break;
      case Format::Parquet:
        producer_.reset(new ParquetRowProducer(definition_, projection_));
        break;
      default:
        throw Error("the file format is not supported");
    }
  }

  bool next(std::vector<Cell> &row)
  {
    check_unchanged();
    row.clear();
    std::vector<Cell> cells;
    if (!producer_->next_row(cells)) {
      return false;
    }
    row.swap(cells);
    return true;
  }

  void rescan()
  {
    check_unchanged();
    producer_->reset();
  }

private:
  void check_unchanged()
  {
    const Fingerprint fingerprint = fingerprint_of(path_);
    if (fingerprint.size != definition_.file_size || fingerprint.mtime_ns != definition_.file_mtime_ns) {
      throw Error("the file " + definition_.path + " has been modified since it was read");
    }
  }

  Definition definition_;
  std::string root_;
  std::vector<int> projection_;
  std::string path_;
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
  void open(const Definition &output, const std::string &root)
  {
    root_ = root;
    options_ = output.options;
    target_path_ = resolve_output_path(output.path, root);
    if (is_regular_file(target_path_)) {
      throw Error("the file " + target_path_ + " already exists");
    }
    static int counter = 0;
    ++counter;
    const std::string directory = directory_of(target_path_);
    char suffix[64];
    ::snprintf(suffix, sizeof(suffix), "/.seekdb-copy-%ld-%d.csv", static_cast<long>(::getpid()), counter);
    temp_path_ = directory + suffix;
    stream_.open(temp_path_.c_str(), std::ios::binary | std::ios::trunc);
    if (!stream_.good()) {
      throw Error("cannot create the temporary file " + temp_path_);
    }
    if (options_.header) {
      std::vector<Field> header;
      for (size_t index = 0; index < output.columns.size(); ++index) {
        Field field;
        field.is_null = false;
        field.text = output.columns[index].name;
        field.type = Type::Varchar;
        header.push_back(field);
      }
      write_row(header);
    }
  }

  void write(const std::vector<Field> &fields)
  {
    if (committed_) {
      throw Error("the file was already written");
    }
    write_row(fields);
  }

  void commit()
  {
    if (committed_) {
      throw Error("the file was already written");
    }
    stream_.flush();
    stream_.close();
    if (is_regular_file(target_path_)) {
      cleanup();
      throw Error("the file " + target_path_ + " already exists");
    }
    if (::rename(temp_path_.c_str(), target_path_.c_str()) != 0) {
      cleanup();
      throw Error("cannot write the file " + target_path_);
    }
    committed_ = true;
  }

  ~CsvWriterImpl()
  {
    if (!committed_) {
      cleanup();
    }
  }

private:
  void cleanup()
  {
    if (stream_.is_open()) {
      stream_.close();
    }
    if (!temp_path_.empty()) {
      ::remove(temp_path_.c_str());
      temp_path_.clear();
    }
  }

  void write_row(const std::vector<Field> &fields)
  {
    for (size_t index = 0; index < fields.size(); ++index) {
      if (index > 0) {
        stream_ << options_.delimiter;
      }
      write_field(fields[index]);
    }
    stream_ << '\n';
    if (!stream_.good()) {
      throw Error("cannot write the file " + target_path_);
    }
  }

  void write_field(const Field &field)
  {
    if (field.is_null) {
      stream_ << options_.null_string;
      return;
    }
    const std::string &text = field.text;
    const bool needs_quotes = text.empty() || (!options_.null_string.empty() && text == options_.null_string)
                              || text.find(options_.delimiter) != std::string::npos
                              || text.find(options_.quote) != std::string::npos
                              || (options_.escape != options_.quote && text.find(options_.escape) != std::string::npos)
                              || text.find('\n') != std::string::npos || text.find('\r') != std::string::npos
                              || text.front() == ' ' || text.back() == ' ';
    if (!needs_quotes) {
      stream_ << text;
      return;
    }
    stream_ << options_.quote;
    for (size_t index = 0; index < text.size(); ++index) {
      const char item = text[index];
      if (item == options_.quote) {
        if (options_.escape == options_.quote) {
          stream_ << options_.quote << options_.quote;
        } else {
          stream_ << options_.escape << options_.quote;
        }
      } else if (options_.escape != options_.quote && item == options_.escape) {
        stream_ << options_.escape << options_.escape;
      } else {
        stream_ << item;
      }
    }
    stream_ << options_.quote;
  }

  std::string resolve_output_path(const std::string &path, const std::string &root)
  {
    if (root.empty() || root == "NULL") {
      throw Error("the directory allowed to write files to is not configured");
    }
    const std::string canonical_root = canonical_path(root);
    if (!is_directory(canonical_root)) {
      throw Error("the directory allowed to write files to is not a directory");
    }
    const std::string name = basename_of(path);
    if (name.empty() || name == "." || name == "..") {
      throw Error("the output path " + path + " is not a file name");
    }
    const std::string directory = canonical_path(directory_of(path));
    if (!is_within(directory, canonical_root)) {
      throw Error("the output path " + path + " is outside of the directory allowed to write files to");
    }
    return directory + "/" + name;
  }

  Options options_;
  std::string root_;
  std::string target_path_;
  std::string temp_path_;
  std::ofstream stream_;
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
