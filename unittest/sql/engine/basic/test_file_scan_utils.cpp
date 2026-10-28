/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/file_sql_file_access.h"
#include "data/file_sql_parquet_fixtures.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
class FileScanTest : public testing::Test
{
protected:
  void SetUp() override
  {
    char name[] = "/tmp/seekdb-file-test-XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(name));
    root = name;
  }
  void TearDown() override { std::filesystem::remove_all(root); }
  std::string put(const std::string &name, const std::string &bytes)
  {
    std::string path = root + "/" + name;
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    out.close();
    return path;
  }
  std::string root;
};

TEST_F(FileScanTest, csv_quotes_bom_nulls_projection_and_rescan)
{
  auto path = put("data.csv", "\xef\xbb\xbfID,id,text,n\r\n1,2,\"hello,\r\nworld\",NULL\r\n3,4,\"say \"\"hi\"\"\",\"NULL\"\r\n");
  Options options; options.null_string = "NULL";
  auto definition = Definition::deserialize(infer(path, root, options).serialize());
  ASSERT_EQ(4U, definition.columns.size());
  EXPECT_EQ("ID", definition.columns[0].name);
  EXPECT_EQ("id__2", definition.columns[1].name);
  EXPECT_TRUE(definition.columns[3].nullable);
  Scan scan(definition, root, {2, 3});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("hello,\nworld", row[0].text);
  EXPECT_TRUE(row[1].is_null);
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("say \"hi\"", row[0].text);
  EXPECT_FALSE(row[1].is_null);
  EXPECT_EQ("NULL", row[1].text);
  EXPECT_FALSE(scan.next(row));
  scan.rescan();
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("hello,\nworld", row[0].text);
}

TEST_F(FileScanTest, original_parquet_snappy_v2_projection_and_rescan)
{
  auto path = put("original.parquet",
                  std::string(reinterpret_cast<const char *>(fixtures::original), sizeof(fixtures::original)));
  Options options; options.format = Format::Parquet;
  auto definition = infer(path, root, options);
  ASSERT_EQ(1U, definition.columns.size());
  EXPECT_EQ("c1", definition.columns[0].name);
  EXPECT_EQ(Type::Bigint, definition.columns[0].type);
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  for (int64_t expected = 0; expected < 7; ++expected) {
    ASSERT_TRUE(scan.next(row));
    ASSERT_EQ(1U, row.size());
    EXPECT_FALSE(row[0].is_null);
    EXPECT_EQ(std::to_string(expected), row[0].text);
  }
  EXPECT_FALSE(scan.next(row));
  scan.rescan();
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("0", row[0].text);
}

TEST_F(FileScanTest, parquet_multiple_row_groups_and_empty_projection)
{
  auto path = put("multi.parquet",
                  std::string(reinterpret_cast<const char *>(fixtures::multi_group), sizeof(fixtures::multi_group)));
  auto definition = infer(path, root, Options{});
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  for (const auto *expected : {"10", "20", "30"}) {
    ASSERT_TRUE(scan.next(row));
    ASSERT_EQ(1U, row.size());
    EXPECT_FALSE(row[0].is_null);
    EXPECT_EQ(expected, row[0].text);
  }
  EXPECT_FALSE(scan.next(row));
  scan.rescan();
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("10", row[0].text);
  Scan count(definition, root, {});
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(count.next(row));
    EXPECT_TRUE(row.empty());
  }
  EXPECT_FALSE(count.next(row));
}

TEST_F(FileScanTest, parquet_nullable_dictionary_values)
{
  auto path = put("dictionary.parquet", std::string(
      reinterpret_cast<const char *>(fixtures::nullable_dictionary), sizeof(fixtures::nullable_dictionary)));
  auto definition = infer(path, root, Options{});
  ASSERT_EQ(1U, definition.columns.size());
  EXPECT_TRUE(definition.columns[0].nullable);
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  EXPECT_FALSE(row[0].is_null);
  EXPECT_EQ("alpha", row[0].text);
  ASSERT_TRUE(scan.next(row));
  EXPECT_TRUE(row[0].is_null);
  ASSERT_TRUE(scan.next(row));
  EXPECT_FALSE(row[0].is_null);
  EXPECT_EQ("beta", row[0].text);
  EXPECT_FALSE(scan.next(row));
}

TEST_F(FileScanTest, parquet_rejects_short_or_extra_column_without_fabricating_nulls)
{
  for (auto bytes : {
      std::string(reinterpret_cast<const char *>(fixtures::short_column), sizeof(fixtures::short_column)),
      std::string(reinterpret_cast<const char *>(fixtures::extra_column), sizeof(fixtures::extra_column))}) {
    auto path = put("mismatch.parquet", bytes);
    EXPECT_THROW({
      auto definition = infer(path, root, Options{});
      Scan scan(definition, root, {0});
      std::vector<Cell> row;
      while (scan.next(row)) {}
    }, Error);
  }
}

TEST_F(FileScanTest, parquet_truncated_footer_reports_file_error)
{
  auto bytes = std::string(reinterpret_cast<const char *>(fixtures::original), sizeof(fixtures::original));
  // A one-byte footer starting a LIST field cannot contain its list header.
  bytes[bytes.size() - 9] = static_cast<char>(0x19);
  bytes[bytes.size() - 8] = 1;
  for (size_t i = bytes.size() - 7; i < bytes.size() - 4; ++i) {
    bytes[i] = 0;
  }
  auto path = put("truncated.parquet", bytes);
  EXPECT_THROW(infer(path, root, Options{}), Error);
}

TEST_F(FileScanTest, parquet_prunes_unreferenced_pages_but_validates_footer)
{
  auto bytes = std::string(reinterpret_cast<const char *>(fixtures::pruned_column), sizeof(fixtures::pruned_column));
  auto path = put("pruned.parquet", bytes);
  auto definition = infer(path, root, Options{});
  ASSERT_EQ(2U, definition.columns.size());
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  for (const auto *expected : {"10", "20"}) {
    ASSERT_TRUE(scan.next(row));
    ASSERT_EQ(1U, row.size());
    EXPECT_EQ(expected, row[0].text);
  }
  EXPECT_FALSE(scan.next(row));
  EXPECT_THROW({
    Scan all(definition, root, {0, 1});
    all.next(row);
  }, Error);
  for (size_t i = bytes.size() - 8; i < bytes.size() - 4; ++i) {
    bytes[i] = static_cast<char>(0xff);
  }
  put("pruned.parquet", bytes);
  EXPECT_THROW(infer(path, root, Options{}), Error);
}

TEST_F(FileScanTest, json_union_missing_and_conflicting_types)
{
  auto path = put("data.jsonl", "{\"a\":1,\"b\":true}\n{\"A\":2.5,\"c\":\"value\"}\n{\"a\":\"text\",\"c\":null}\n");
  Options options; options.format = Format::Json;
  auto definition = infer(path, root, options);
  ASSERT_EQ(3U, definition.columns.size());
  EXPECT_EQ(Type::Varchar, definition.columns[0].type);
  EXPECT_TRUE(definition.columns[1].nullable);
  EXPECT_TRUE(definition.columns[2].nullable);
  Scan scan(definition, root, {0, 1, 2});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("1", row[0].text); EXPECT_TRUE(row[2].is_null);
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("2.5", row[0].text); EXPECT_TRUE(row[1].is_null);
  ASSERT_TRUE(scan.next(row)); EXPECT_TRUE(row[2].is_null);
  EXPECT_FALSE(scan.next(row));
}

TEST_F(FileScanTest, malformed_input_fails_with_no_partial_schema)
{
  Options options;
  EXPECT_THROW(infer(put("bad.csv", "a,b\n1\n"), root, options), Error);
  EXPECT_THROW(infer(put("quote.csv", "a\n\"unterminated"), root, options), Error);
  EXPECT_THROW(infer(put("utf8.csv", std::string("a\n\xff\n")), root, options), Error);
  options.sample_size = 1;
  EXPECT_THROW(infer(put("sample.csv", "a\n1\n"), root, options), Error);
  options.sample_size = -1; options.format = Format::Json;
  EXPECT_THROW(infer(put("nested.jsonl", "{\"a\":{}}\n"), root, options), Error);
  EXPECT_THROW(infer(put("array.jsonl", "{\"a\":[]}\n"), root, options), Error);
  EXPECT_THROW(infer(put("duplicate.jsonl", "{\"a\":1,\"A\":2}\n"), root, options), Error);
}

TEST_F(FileScanTest, datetime_scale_null_columns_and_headerless_options)
{
  Options options; options.header = false; options.delimiter = ';'; options.quote = '\''; options.escape = '\\';
  auto path = put("options.csv", "2024-02-29 12:13:14.123456;;'a\\'b'\n2024-03-01 01:02:03.123456;;'c;d'\n");
  auto definition = Definition::deserialize(infer(path, root, options).serialize());
  ASSERT_EQ(3U, definition.columns.size());
  EXPECT_EQ("column0", definition.columns[0].name);
  EXPECT_EQ(Type::Datetime, definition.columns[0].type);
  EXPECT_EQ(6, definition.columns[0].scale);
  EXPECT_EQ(Type::Null, definition.columns[1].type);
  EXPECT_TRUE(definition.columns[1].nullable);
  Scan scan(definition, root, {2}); std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row)); EXPECT_EQ("a'b", row[0].text);
  ASSERT_TRUE(scan.next(row)); EXPECT_EQ("c;d", row[0].text);
  EXPECT_FALSE(scan.next(row));
}

TEST_F(FileScanTest, mutation_and_security)
{
  auto path = put("data.csv", "a\n1\n2\n");
  auto definition = infer(path, root, Options{});
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  put("data.csv", "a\n3\n4\n");
  EXPECT_THROW(scan.next(row), Error);
  EXPECT_THROW(scan.rescan(), Error);
  auto fresh = infer(path, root, Options{});
  put("data.csv", "a\n5\n6\n");
  EXPECT_THROW((Scan(fresh, root, {0})), Error);
  EXPECT_THROW(infer(path, root + "/other", Options{}), Error);
  EXPECT_THROW(infer(path, "NULL", Options{}), Error);
  EXPECT_THROW(infer(path, "", Options{}), Error);
  EXPECT_THROW(infer("/etc/passwd", root, Options{}), Error);
  ASSERT_EQ(0, ::symlink("/etc/passwd", (root + "/escape.csv").c_str()));
  EXPECT_THROW(infer(root + "/escape.csv", root, Options{}), Error);
  EXPECT_THROW(discover(root + "/escape.*", root), Error);
}

TEST_F(FileScanTest, security_detects_replacement_with_identical_size_and_mtime)
{
  auto path = put("identity.csv", "a\n1\n2\n");
  struct stat before;
  ASSERT_EQ(0, ::stat(path.c_str(), &before));
  auto definition = Definition::deserialize(infer(path, root, Options{}).serialize());
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  auto replacement = put("replacement.csv", "a\n3\n4\n");
  const struct timespec times[] = {before.st_atim, before.st_mtim};
  ASSERT_EQ(0, ::utimensat(AT_FDCWD, replacement.c_str(), times, 0));
  ASSERT_EQ(0, ::rename(replacement.c_str(), path.c_str()));
  EXPECT_THROW((Scan(definition, root, {0})), Error);
  EXPECT_THROW(scan.next(row), Error);
  EXPECT_THROW(scan.rescan(), Error);
}

TEST_F(FileScanTest, security_detects_inplace_mutation_with_restored_mtime)
{
  auto path = put("identity.jsonl", "{\"a\":1}\n{\"a\":2}\n");
  struct stat before;
  ASSERT_EQ(0, ::stat(path.c_str(), &before));
  auto definition = infer(path, root, Options{});
  Scan scan(definition, root, {0});
  put("identity.jsonl", "{\"a\":3}\n{\"a\":4}\n");
  const struct timespec times[] = {before.st_atim, before.st_mtim};
  ASSERT_EQ(0, ::utimensat(AT_FDCWD, path.c_str(), times, 0));
  std::vector<Cell> row;
  EXPECT_THROW((Scan(definition, root, {0})), Error);
  EXPECT_THROW(scan.next(row), Error);
}

TEST_F(FileScanTest, security_rejects_embedded_nul_in_paths_roots_and_globs)
{
  auto path = put("identity.csv", "a\n1\n");
  EXPECT_THROW(infer(path + std::string("\0suffix", 7), root, Options{}), Error);
  EXPECT_THROW(infer(path, root + std::string("\0suffix", 7), Options{}), Error);
  EXPECT_THROW(discover(root + std::string("/*.csv\0suffix", 13), root), Error);
}

TEST_F(FileScanTest, security_internal_links_and_directory_replacement)
{
  std::filesystem::create_directory(root + "/inner");
  auto path = put("inner/data.csv", "a\n1\n");
  ASSERT_EQ(0, ::symlink("inner/data.csv", (root + "/alias.csv").c_str()));
  auto definition = infer(root + "/alias.csv", root, Options{});
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  EXPECT_EQ("1", row[0].text);
  std::filesystem::rename(root + "/inner", root + "/moved");
  ASSERT_EQ(0, ::symlink("/etc", (root + "/inner").c_str()));
  EXPECT_THROW(scan.next(row), Error);
  EXPECT_THROW(scan.rescan(), Error);
}

TEST_F(FileScanTest, security_checks_file_identity_before_returning_eof)
{
  auto path = put("eof.csv", "a\n1\n");
  auto definition = infer(path, root, Options{});
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  ASSERT_FALSE(scan.next(row));
  put("eof.csv", "a\n2\n");
  EXPECT_THROW(scan.next(row), Error);
  EXPECT_THROW(scan.rescan(), Error);
}

TEST_F(FileScanTest, security_parquet_replacement_and_truncation)
{
  const auto bytes = std::string(reinterpret_cast<const char *>(fixtures::original), sizeof(fixtures::original));
  auto path = put("identity.parquet", bytes);
  struct stat before;
  ASSERT_EQ(0, ::stat(path.c_str(), &before));
  auto definition = infer(path, root, Options{});
  Scan scan(definition, root, {0});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row));
  auto replacement = put("replacement.parquet", bytes);
  const struct timespec times[] = {before.st_atim, before.st_mtim};
  ASSERT_EQ(0, ::utimensat(AT_FDCWD, replacement.c_str(), times, 0));
  ASSERT_EQ(0, ::rename(replacement.c_str(), path.c_str()));
  EXPECT_THROW(scan.next(row), Error);
  auto fresh = infer(path, root, Options{});
  Scan fresh_scan(fresh, root, {0});
  ASSERT_EQ(0, ::truncate(path.c_str(), 8));
  EXPECT_THROW(fresh_scan.next(row), Error);
}

TEST_F(FileScanTest, security_descriptor_ownership_bounds_and_directory_root)
{
  auto path = put("descriptor.csv", "a\n1\n");
  auto file = LocalFile::open_read(path, root);
  EXPECT_NE(0, ::fcntl(file.descriptor(), F_GETFD) & FD_CLOEXEC);
  EXPECT_EQ(O_RDONLY, ::fcntl(file.descriptor(), F_GETFL) & O_ACCMODE);
  auto moved = std::move(file);
  EXPECT_THROW(file.identity(), Error);
  char bytes[16] = {};
  EXPECT_EQ(4U, moved.read_at(0, bytes, sizeof(bytes)));
  EXPECT_EQ("a\n1\n", std::string(bytes, 4));
  EXPECT_EQ(0U, moved.read_at(4, bytes, sizeof(bytes)));
  EXPECT_THROW(moved.read_at(5, bytes, sizeof(bytes)), Error);
  const int old_descriptor = moved.descriptor();
  auto other = LocalFile::open_read(path, root);
  moved = std::move(other);
  EXPECT_EQ(-1, ::fcntl(old_descriptor, F_GETFD));
  auto directory = LocalFile::open_directory(root, root);
  struct stat info;
  ASSERT_EQ(0, ::fstat(directory.descriptor(), &info));
  EXPECT_TRUE(S_ISDIR(info.st_mode));
}

TEST_F(FileScanTest, security_glob_failures_do_not_leak_directory_descriptors)
{
  put("one.csv", "a\n1\n");
  put("two.csv", "a\n2\n");
  const auto paths = discover(root + "/*.csv", root);
  ASSERT_EQ(2U, paths.size());
  EXPECT_EQ(root + "/one.csv", paths[0]);
  EXPECT_EQ(root + "/two.csv", paths[1]);
  ASSERT_EQ(0, ::symlink("missing.csv", (root + "/dangling.csv").c_str()));
  const auto fd_count = []() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
  };
  const auto before = fd_count();
  for (int i = 0; i < 128; ++i) {
    EXPECT_THROW(discover(root + "/*.csv", root), Error);
  }
  EXPECT_EQ(before, fd_count());
}

TEST_F(FileScanTest, security_rejects_nonregular_files_and_closes_failed_reads)
{
  auto fifo = root + "/fifo.csv";
  ASSERT_EQ(0, ::mkfifo(fifo.c_str(), 0600));
  EXPECT_THROW(infer(fifo, root, Options{}), Error);
  auto path = put("invalid.csv", "a\n\"unterminated");
  const auto fd_count = []() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
  };
  const auto before = fd_count();
  for (int i = 0; i < 128; ++i) {
    EXPECT_THROW(infer(path, root, Options{}), Error);
  }
  EXPECT_EQ(before, fd_count());
}

TEST_F(FileScanTest, copy_roundtrip_and_atomic_cleanup)
{
  Definition output; output.path = root + "/out.csv";
  output.options.null_string = "NULL";
  output.columns = {{"name", Type::Varchar, false}, {"value", Type::Varchar, true}};
  {
    CsvWriter writer(output, root);
    writer.write({{false, "a,\"b\n", Type::Varchar}, {true, "", Type::Null}});
    writer.write({{false, "", Type::Varchar}, {false, "NULL", Type::Varchar}});
    EXPECT_FALSE(std::filesystem::exists(output.path));
    writer.commit();
  }
  EXPECT_THROW(CsvWriter(output, root), Error);
  auto definition = infer(output.path, root, output.options);
  Scan scan(definition, root, {0, 1});
  std::vector<Cell> row;
  ASSERT_TRUE(scan.next(row)); EXPECT_EQ("a,\"b\n", row[0].text); EXPECT_TRUE(row[1].is_null);
  ASSERT_TRUE(scan.next(row)); EXPECT_EQ("", row[0].text); EXPECT_FALSE(row[0].is_null);
  EXPECT_EQ("NULL", row[1].text); EXPECT_FALSE(row[1].is_null);
  output.path = root + "/failed.csv";
  { CsvWriter writer(output, root); writer.write({{true, "", Type::Null}, {true, "", Type::Null}}); }
  EXPECT_FALSE(std::filesystem::exists(output.path));
  for (auto &entry : std::filesystem::directory_iterator(root))
    EXPECT_EQ(std::string::npos, entry.path().filename().string().find(".seekdb-copy-"));
}

TEST(FileSqlTypes, boundaries_dates_and_widening)
{
  EXPECT_EQ(Type::Bigint, infer_type("-9223372036854775808"));
  EXPECT_EQ(Type::Bigint, infer_type("9223372036854775807"));
  EXPECT_EQ(Type::Varchar, infer_type("9223372036854775808"));
  EXPECT_EQ(Type::Varchar, infer_type("-9223372036854775809"));
  EXPECT_EQ(Type::Double, infer_type("1e20"));
  EXPECT_EQ(Type::Boolean, infer_type("tRuE"));
  EXPECT_EQ(Type::Date, infer_type("2024-02-29"));
  EXPECT_EQ(Type::Varchar, infer_type("2023-02-29"));
  EXPECT_EQ(Type::Datetime, infer_type("2024-02-29T23:59:59.123456"));
  EXPECT_EQ(Type::Varchar, infer_type("2024-02-29T24:00:00"));
  EXPECT_EQ(Type::Double, widen(Type::Bigint, Type::Double));
  EXPECT_EQ(Type::Datetime, widen(Type::Date, Type::Datetime));
  EXPECT_EQ(Type::Varchar, widen(Type::Date, Type::Boolean));
  EXPECT_EQ(Type::Null, widen(Type::Null, Type::Null));
}
} // namespace
}}}
