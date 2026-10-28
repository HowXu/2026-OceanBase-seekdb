/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#include "sql/engine/basic/file_sql_file_access.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/file_sql_memory.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits.h>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif
#endif

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
std::string canonical_path(const std::string &path)
{
  check_count(path.size(), PATH_MAX - 1, "path");
  if (path.find('\0') != std::string::npos) {
    throw Error("file SQL path contains NUL");
  }
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) == nullptr) {
    throw Error("cannot resolve file SQL path: " + path);
  }
  return resolved;
}

FileIdentity identity_of(const struct stat &info)
{
  FileIdentity result;
  result.device = static_cast<uint64_t>(info.st_dev);
  result.inode = static_cast<uint64_t>(info.st_ino);
  result.size = static_cast<uint64_t>(info.st_size);
#if defined(__APPLE__) || defined(__FreeBSD__)
  result.mtime_ns = static_cast<int64_t>(info.st_mtimespec.tv_sec) * 1000000000 + info.st_mtimespec.tv_nsec;
  result.ctime_ns = static_cast<int64_t>(info.st_ctimespec.tv_sec) * 1000000000 + info.st_ctimespec.tv_nsec;
#else
  result.mtime_ns = static_cast<int64_t>(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
  result.ctime_ns = static_cast<int64_t>(info.st_ctim.tv_sec) * 1000000000 + info.st_ctim.tv_nsec;
#endif
  return result;
}
} // namespace

bool FileIdentity::operator==(const FileIdentity &other) const
{
  return device == other.device && inode == other.inode && size == other.size
         && mtime_ns == other.mtime_ns && ctime_ns == other.ctime_ns;
}

LocalFile::~LocalFile()
{
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

LocalFile::LocalFile(LocalFile &&other) noexcept
{
  *this = std::move(other);
}

LocalFile &LocalFile::operator=(LocalFile &&other) noexcept
{
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
    directory_ = other.directory_;
    path_ = std::move(other.path_);
    root_ = std::move(other.root_);
    initial_ = other.initial_;
  }
  return *this;
}

LocalFile LocalFile::open_read(const std::string &path, const std::string &root)
{
  return open(path, root, false);
}

LocalFile LocalFile::open_directory(const std::string &path, const std::string &root)
{
  return open(path, root, true);
}

LocalFile LocalFile::open(const std::string &path, const std::string &root, bool directory)
{
  if (root.empty() || root == "NULL") {
    throw Error("the directory allowed to read files from is not configured");
  }
  LocalFile result;
  result.root_ = canonical_path(root);
  result.path_ = canonical_path(path);
  result.directory_ = directory;
  if (result.root_ != "/" && result.path_ != result.root_
      && (result.path_.size() <= result.root_.size()
          || result.path_.compare(0, result.root_.size(), result.root_) != 0
          || result.path_[result.root_.size()] != '/')) {
    throw Error("file SQL path is outside the authorized directory: " + path);
  }

  LocalFile current;
  current.fd_ = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (current.fd_ < 0) {
    throw Error("cannot open filesystem root");
  }
  // Pin the authorized root first, then resolve the canonical relative path
  // against that descriptor. A concurrent symlink swap fails with O_NOFOLLOW;
  // realpath is normalization, never the authorization for a bare-path open.
  for (int stage = 0; stage < 2; ++stage) {
    const auto &absolute = stage == 0 ? result.root_ : result.path_;
    size_t pos = stage == 0 ? 1 : result.root_.size();
    while (pos < absolute.size()) {
      if (absolute[pos] == '/') {
        ++pos;
        continue;
      }
      const size_t end = absolute.find('/', pos);
      const std::string component = absolute.substr(pos, end - pos);
      const bool last_file = stage == 1 && end == std::string::npos && !directory;
      const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC
                        | (last_file ? O_NONBLOCK : O_DIRECTORY);
      LocalFile next;
      next.fd_ = ::openat(current.fd_, component.c_str(), flags);
      if (next.fd_ < 0) {
        throw Error("cannot open authorized file SQL path: " + path);
      }
      current = std::move(next);
      pos = end == std::string::npos ? absolute.size() : end + 1;
    }
  }
  struct stat info;
  if (::fstat(current.fd_, &info) != 0 || info.st_size < 0
      || (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
    throw Error(directory ? "file SQL path is not a directory: " + path
                          : "file SQL input is not a regular file: " + path);
  }
  result.fd_ = current.fd_;
  current.fd_ = -1;
  result.initial_ = identity_of(info);
  return result;
}

FileIdentity LocalFile::identity() const
{
  struct stat info;
  if (::fstat(fd_, &info) != 0 || info.st_size < 0) {
    throw Error("cannot stat authorized file SQL descriptor: " + path_);
  }
  return identity_of(info);
}

void LocalFile::validate_unchanged(const FileIdentity &expected) const
{
  if (identity() != expected) {
    throw Error("file SQL input has changed: " + path_);
  }
  // Check the current directory entry as well as the held descriptor. A rename
  // replacement must not be hidden by an otherwise unchanged old descriptor.
#if defined(SYS_openat2) && defined(RESOLVE_NO_SYMLINKS)
  // The stored path is already canonical and root-authorized. Resolve every
  // component without links in one kernel call, instead of walking the whole
  // root twice for every row. This descriptor is used only for identity checks;
  // all content reads still use the originally authorized descriptor.
  struct open_how how = {};
  how.flags = O_PATH | O_CLOEXEC;
  how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
  const int fd = static_cast<int>(::syscall(SYS_openat2, AT_FDCWD, path_.c_str(), &how, sizeof(how)));
  if (fd >= 0) {
    struct stat info;
    const int status = ::fstat(fd, &info);
    ::close(fd);
    if (status != 0 || info.st_size < 0 || identity_of(info) != expected) {
      throw Error("file SQL input was replaced: " + path_);
    }
    return;
  }
  // Older kernels/headers and syscall-filtered containers retain the safe
  // component-by-component fallback; they never fall back to a bare read open.
  if (errno != ENOSYS && errno != EINVAL && errno != EPERM) {
    throw Error("cannot revalidate authorized file SQL path: " + path_);
  }
#endif
  auto entry = open(path_, root_, directory_);
  if (entry.identity() != expected || identity() != expected) {
    throw Error("file SQL input was replaced: " + path_);
  }
}

size_t LocalFile::read_at(uint64_t offset, void *buffer, size_t size)
{
  validate_unchanged(initial_);
  if (offset > initial_.size || offset > static_cast<uint64_t>(INT64_MAX)) {
    throw Error("file SQL read offset is out of bounds: " + path_);
  }
  const size_t wanted = static_cast<size_t>(std::min<uint64_t>(size, initial_.size - offset));
  size_t done = 0;
  while (done < wanted) {
    const ssize_t count = ::pread(fd_, static_cast<char *>(buffer) + done, wanted - done,
                                  static_cast<off_t>(offset + done));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw Error("cannot read authorized file SQL input: " + path_);
    }
    done += static_cast<size_t>(count);
  }
  validate_unchanged(initial_);
  return done;
}

}}} // namespace oceanbase::sql::file_sql
