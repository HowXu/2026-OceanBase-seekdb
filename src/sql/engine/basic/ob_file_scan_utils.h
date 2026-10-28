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

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {

/**
 * @brief Error raised when a file cannot be read, a schema cannot be inferred
 *        or a security constraint forbids the access.
 *
 * Every failure of this module is reported by throwing this exception, the
 * caller converts it into an SQL error code.
 */
class Error : public std::runtime_error
{
public:
  explicit Error(std::string_view message) : std::runtime_error(std::string(message.substr(0, 1024))) {}
};

/**
 * @brief Column types produced by the schema inference.
 */
enum class Type {
  Null = 0,
  Boolean = 1,
  Bigint = 2,
  Double = 3,
  Varchar = 4,
  Date = 5,
  Datetime = 6,
};

/**
 * @brief File formats understood by the file scan utilities.
 *
 * Format::Auto lets the utilities pick the format from the file extension.
 */
enum class Format {
  Auto = 0,
  Csv = 1,
  Json = 2,
  Parquet = 3,
};

/**
 * @brief Options controlling how a file is parsed.
 */
struct Options {
  Format format = Format::Auto;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_string;
  int64_t sample_size = -1;
};

/**
 * @brief One column of an inferred schema.
 */
struct Column {
  std::string name;
  Type type = Type::Varchar;
  bool nullable = false;
  int scale = 0;
};

/**
 * @brief One scanned value, kept in its textual form plus a null flag.
 */
struct Cell {
  std::string text;
  bool is_null = false;
};

/**
 * @brief One value handed to the CSV writer.
 */
struct Field {
  bool is_null = false;
  std::string text;
  Type type = Type::Varchar;
};

/**
 * @brief The schema of a file together with the fingerprint of the file it
 *        was inferred from.
 *
 * The fingerprint allows the scanner to detect that the file changed between
 * the inference and the scan.
 */
class Definition {
public:
  std::string path;
  Options options;
  std::vector<Column> columns;
  uint64_t file_size = 0;
  int64_t file_mtime_ns = 0;
  uint64_t file_device = 0;
  uint64_t file_inode = 0;
  int64_t file_ctime_ns = 0;

  /**
   * @brief Serialize the definition into a binary blob.
   */
  std::string serialize() const;

  /**
   * @brief Restore a definition serialized by serialize().
   */
  static Definition deserialize(const std::string &blob);
};

/**
 * @brief The name of a type, as it is shown by a describe.
 */
const char *type_name(Type type);

/**
 * @brief Infer the type of a single textual value.
 */
Type infer_type(std::string_view text);

/**
 * @brief Combine the types of two values of the same column.
 */
Type widen(Type left, Type right);

/**
 * @brief Pick the format of a file from the options and the file extension.
 * @throw Error when the extension is not supported.
 */
Format resolve_format(const std::string &path, const Options &options);

/**
 * @brief Resolve a path and make sure it stays inside the root directory.
 * @param path  the file to access
 * @param root  the only directory the file may live in
 * @return the canonical path of the file
 * @throw Error when the root is invalid or the path escapes the root
 */
std::string resolve_within_root(const std::string &path, const std::string &root);

/**
 * @brief Infer the schema of a file by reading all of its rows.
 * @param path     the file to read
 * @param root     the only directory the file may live in
 * @param options  the parsing options
 * @throw Error when the file cannot be read or the content is malformed
 */
Definition infer(const std::string &path, const std::string &root, const Options &options);

/**
 * @brief List the files matching a glob pattern.
 * @param pattern  the glob pattern, its directory part must stay in root
 * @param root     the only directory the files may live in
 * @throw Error when the pattern escapes the root or a match is a dangling link
 */
std::vector<std::string> discover(const std::string &pattern, const std::string &root);

/**
 * @brief Stream the rows of a file.
 *
 * The scan never holds the whole file in memory, rows are produced on demand
 * and the file is checked for modifications between rows.
 */
class Scan {
public:
  /**
   * @param definition  the schema returned by infer()
   * @param root        the only directory the file may live in
   * @param projection  the indexes of the columns to return, in output order.
   *                    An empty projection counts the rows without returning
   *                    any value.
   * @throw Error when the file changed since the schema was inferred
   */
  Scan(const Definition &definition, const std::string &root, const std::vector<int> &projection);
  ~Scan();

  /**
   * @brief Read the next row.
   * @param row  receives one cell per projected column
   * @return false when the end of the file is reached
   * @throw Error when the file changed while it was being scanned
   */
  bool next(std::vector<Cell> &row);

  /**
   * @brief Restart the scan from the first row.
   * @throw Error when the file changed while it was being scanned
   */
  void rescan();

private:
  class ScanImpl;
  std::unique_ptr<ScanImpl> impl_;
};

/**
 * @brief Write rows of a table into a CSV file.
 *
 * The rows are written into a temporary file next to the target and are moved
 * to the target by commit(). The target must not exist yet.
 */
class CsvWriter {
public:
  /**
   * @param output  the target file and its options, output.columns gives the
   *                column names written as the header
   * @param root    the only directory the target may live in
   * @throw Error when the target exists or the path escapes the root
   */
  CsvWriter(const Definition &output, const std::string &root);
  ~CsvWriter();

  /**
   * @brief Append one row.
   * @throw Error when the writer was committed already
   */
  void write(const std::vector<Field> &fields);

  /**
   * @brief Move the temporary file to the target.
   * @throw Error when the target appeared in the meantime
   */
  void commit();

private:
  class CsvWriterImpl;
  std::unique_ptr<CsvWriterImpl> impl_;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
