/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oceanbase { namespace sql { namespace file_sql {

struct FileIdentity {
  uint64_t device = 0;
  uint64_t inode = 0;
  uint64_t size = 0;
  int64_t mtime_ns = 0;
  int64_t ctime_ns = 0;

  bool operator==(const FileIdentity &other) const;
  bool operator!=(const FileIdentity &other) const { return !(*this == other); }
};

// Owns an authorized descriptor. Canonical paths are opened component by
// component under a pinned root, without following links during the open.
class LocalFile {
public:
  static LocalFile open_read(const std::string &path, const std::string &root);
  static LocalFile open_directory(const std::string &path, const std::string &root);
  ~LocalFile();
  LocalFile(LocalFile &&other) noexcept;
  LocalFile &operator=(LocalFile &&other) noexcept;
  LocalFile(const LocalFile &) = delete;
  LocalFile &operator=(const LocalFile &) = delete;

  size_t read_at(uint64_t offset, void *buffer, size_t size);
  FileIdentity identity() const;
  void validate_unchanged(const FileIdentity &expected) const;
  const std::string &path() const { return path_; }
  int descriptor() const { return fd_; }

private:
  LocalFile() = default;
  static LocalFile open(const std::string &path, const std::string &root, bool directory);

  int fd_ = -1;
  bool directory_ = false;
  std::string path_;
  std::string root_;
  FileIdentity initial_;
};

}}} // namespace oceanbase::sql::file_sql
