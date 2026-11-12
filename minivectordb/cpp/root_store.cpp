#include "root_store.hpp"
#include "storage_codec.hpp"
#include "table_segments.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <set>
#ifdef MINIVECTORDB_TESTING
#include <cstdlib>
#include <csignal>
#endif

namespace minivectordb {
namespace {
const std::string magic = "MVDBC003";
void ensure_directory(const std::filesystem::path &path) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    storage::fail("cannot inspect directory");
  if (std::filesystem::is_symlink(status))
    storage::fail("symlink directory rejected");
  if (!std::filesystem::exists(status)) {
    ec.clear();
    std::filesystem::create_directories(path, ec);
    if (ec)
      storage::fail("cannot create directory");
  } else if (!std::filesystem::is_directory(status))
    storage::fail("not a directory");
}
void no_link(const std::filesystem::path &path, bool directory = false) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  if (ec && ec != std::errc::no_such_file_or_directory)
    storage::fail("cannot inspect path");
  if (std::filesystem::is_symlink(status))
    storage::fail("symlink path rejected");
  if (directory && !std::filesystem::is_directory(status))
    storage::fail("missing object directory");
  if (!directory && std::filesystem::exists(status) &&
      !std::filesystem::is_regular_file(status))
    storage::fail("object is not regular file");
}
void sync_path(const std::filesystem::path& path, bool directory = false) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                  (directory ? O_DIRECTORY : 0));
  if (fd < 0) storage::fail("cannot open path for fsync");
  int result;
  do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
  int closed = ::close(fd);
  if (result != 0 || closed != 0) storage::fail("cannot fsync/close path");
}
} // namespace
RootStore::RootStore(const std::filesystem::path &data_dir) {
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec)
    storage::fail("cannot create data directory");
  data_dir_ = std::filesystem::canonical(data_dir, ec);
  if (ec)
    storage::fail("cannot resolve data directory");
  for (const auto &name : {"objects", "catalogs", "staging"})
    ensure_directory(data_dir_ / name);
}
std::filesystem::path RootStore::checked_path(const std::string &ref,
                                              const std::string &dir,
                                              const std::string &ext) const {
  storage::validate_reference(ref, dir, ext);
  no_link(data_dir_ / dir, true);
  auto path = data_dir_ / ref;
  no_link(path);
  return path;
}
std::filesystem::path RootStore::object_path(const std::string &ref) const {
  return checked_path(ref, "objects", ".tbl");
}
std::string RootStore::reserve(const std::string &dir, const std::string &ext) {
  static std::atomic<std::uint64_t> sequence{0};
  while (true) {
    auto ref =
        dir + "/" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        "-" + std::to_string(getpid()) + "-" + std::to_string(sequence++) + ext;
    auto path = checked_path(ref, dir, ext);
    int fd = ::open(path.c_str(),
                    O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      if (::close(fd) != 0)
        storage::fail("cannot close reserved object");
      return ref;
    }
    if (errno != EEXIST)
      storage::fail("cannot reserve object");
  }
}
std::string RootStore::create_object() { return reserve("objects", ".tbl"); }
std::string RootStore::write_catalog(const Catalog &catalog) {
  // Validate every reference before creating the candidate metadata object.
  for (const auto &[name, entry] : catalog.tables()) {
    auto schema = entry.schema;
    validate_schema(schema);
    if (entry.objects.empty()) storage::fail("table has no objects");
    for (const auto& object : entry.objects) object_path(object);
  }
  auto ref = reserve("catalogs", ".bin");
  storage::Output out(checked_path(ref, "catalogs", ".bin"));
  out.bytes(magic.data(), magic.size());
  out.u64(catalog.tables().size());
  for (const auto &[name, entry] : catalog.tables()) {
    storage::write_schema(out, entry.schema);
    if (entry.objects.empty()) storage::fail("table has no objects");
    out.text(entry.objects.front());
    out.u64(entry.objects.size() - 1);
    for (std::size_t i = 1; i < entry.objects.size(); ++i) out.text(entry.objects[i]);
  }
  out.u64(catalog.vector_indexes().size());
  for (const auto &[name, index] : catalog.vector_indexes()) {
    out.text(index.name);
    out.text(index.table);
    out.text(index.column);
    out.u8(static_cast<std::uint8_t>(index.backend));
  }
  out.u8(0);
  out.finish();
  return ref;
}
Catalog RootStore::read_catalog(const std::string &ref) const {
  storage::Input in(checked_path(ref, "catalogs", ".bin"));
  std::string version(8, '\0');
  in.bytes(version.data(), version.size());
  if (version != magic) storage::fail("invalid catalog magic");
  auto count = in.u64();
  if (count > in.remaining() / 20)
    storage::fail("invalid table count");
  Catalog c;
  for (std::uint64_t i = 0; i < count; ++i) {
    auto schema = storage::read_schema(in);
    auto base = in.text();
    object_path(base);
    auto delta_count = in.u64();
    if (delta_count > in.remaining() / 8) storage::fail("invalid delta count");
    std::vector<std::string> objects{base};
    for (std::uint64_t i = 0; i < delta_count; ++i) {
      auto delta = in.text();
      object_path(delta);
      objects.push_back(std::move(delta));
    }
    c.add(schema, base);
    c.replace_objects(schema.name, std::move(objects));
  }
  {
    auto indexes = in.u64();
    if (indexes > in.remaining() / 25) storage::fail("invalid index count");
    for (std::uint64_t i = 0; i < indexes; ++i) {
      auto name = in.text();
      auto table = in.text();
      auto column = in.text();
      auto backend = in.u8();
      if (backend != 1) storage::fail("unknown vector index backend");
      c.add_vector_index({std::move(name), std::move(table), std::move(column),
                          VectorIndexBackend::FlatScan});
    }
  }
  if (in.u8() != 0)
    storage::fail("invalid catalog footer");
  in.end();
  return c;
}
std::string RootStore::current_reference() const {
  auto current = data_dir_ / "CURRENT";
  no_link(current);
  std::error_code ec;
  if (!std::filesystem::exists(current, ec)) {
    if (ec)
      storage::fail("cannot inspect CURRENT");
    return {};
  }
  storage::Input in(current);
  if (in.remaining() > 512 || in.remaining() < 2)
    storage::fail("invalid CURRENT");
  std::string ref(in.remaining(), '\0');
  in.bytes(ref.data(), ref.size());
  if (ref.back() != '\n')
    storage::fail("unterminated CURRENT");
  ref.pop_back();
  storage::validate_reference(ref, "catalogs", ".bin");
  return ref;
}
Catalog RootStore::load_root() const {
  auto ref = current_reference();
  if (ref.empty()) return {};
  auto catalog = read_catalog(ref);
  for (const auto &[name, entry] : catalog.tables()) {
    std::vector<std::filesystem::path> paths;
    for (const auto& object : entry.objects) paths.push_back(object_path(object));
    validate_table_objects(paths, entry.schema);
  }
  return catalog;
}
void RootStore::checkpoint(const std::string& name) const {
#ifdef MINIVECTORDB_TESTING
  if (test_hook_) test_hook_(name);
  // Explicit testing builds only: stop in-process at a real boundary so the
  // parent can SIGKILL or provoke a filesystem error without timing races.
  const char* stop = std::getenv("MINIVECTORDB_TEST_STOP");
  if (stop && name == stop) ::raise(SIGSTOP);
#else
  (void)name;
#endif
}
Catalog RootStore::recover() {
  auto root = load_root();
  committed_objects_.clear();
  for (const auto& [name, entry] : root.tables())
    for (const auto& object : entry.objects) committed_objects_.insert(object);
  // CURRENT may be visible after an unacknowledged rename. Make that choice
  // durable before reclaiming the previous root's catalog or table objects.
  checkpoint("before_recovery_dir_sync");
  sync_path(data_dir_, true);
  checkpoint("recovery_dir_synced");
  collect_garbage();
  return root;
}
void RootStore::publish(const Catalog& candidate) {
  if (poisoned_) throw DbError("fatal_storage_error", "publication state is uncertain");
  bool replacing = false;
  try {
    for (const auto& [name, entry] : candidate.tables()) {
      // Old committed objects were validated during recovery or an earlier
      // publication. Only new immutable segments need another scan and fsync.
      for (std::size_t i = 0; i < entry.objects.size(); ++i) {
        const auto& object = entry.objects[i];
        if (committed_objects_.contains(object)) continue;
        auto path = object_path(object);
        validate_table_segment(path, entry.schema, i != 0);
        sync_path(path);
      }
    }
    checkpoint("objects_synced");
    sync_path(data_dir_ / "objects", true);
    checkpoint("objects_dir_synced");
    auto ref = write_catalog(candidate);
    sync_path(checked_path(ref, "catalogs", ".bin"));
    checkpoint("catalog_synced");
    sync_path(data_dir_ / "catalogs", true);
    checkpoint("catalogs_dir_synced");
    auto temporary = reserve("staging", ".tmp");
    auto path = checked_path(temporary, "staging", ".tmp");
    storage::Output out(path);
    auto contents = ref + "\n";
    out.bytes(contents.data(), contents.size());
    out.finish();
    sync_path(path);
    checkpoint("current_synced");
    no_link(data_dir_ / "CURRENT");
    checkpoint("before_current_replace");
    replacing = true;
    // Both paths belong to the same data directory/filesystem.
    if (::rename(path.c_str(), (data_dir_ / "CURRENT").c_str()) != 0)
      storage::fail("cannot replace CURRENT");
    checkpoint("current_replaced");
    sync_path(data_dir_, true);
    checkpoint("data_dir_synced");
    std::set<std::string> published;
    for (const auto& [name, entry] : candidate.tables())
      for (const auto& object : entry.objects) published.insert(object);
    committed_objects_.swap(published);
  } catch (...) {
    if (replacing) {
      poisoned_ = true;
      throw DbError("fatal_storage_error", "CURRENT publication uncertain; restart required");
    }
    throw;
  }
}
void RootStore::collect_garbage(const std::vector<Catalog>& protected_roots) {
  if (poisoned_) throw DbError("fatal_storage_error", "publication state is uncertain");
  std::set<std::string> live;
  auto protect = [&](const Catalog& root) {
    for (const auto& [name, entry] : root.tables()) {
      for (const auto& object : entry.objects) {
        object_path(object);
        live.insert(object);
      }
    }
  };
  auto current = current_reference();
  if (!current.empty()) {
    live.insert(current);
    protect(read_catalog(current));
  }
  for (const auto& root : protected_roots) protect(root);
  for (const auto& [directory, extension] :
       {std::pair{"objects", ".tbl"}, {"catalogs", ".bin"}, {"staging", ".tmp"}}) {
    auto dir = data_dir_ / directory;
    no_link(dir, true);
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    if (ec) storage::fail("cannot enumerate garbage");
    while (it != end) {
      auto path = it->path();
      auto ref = std::string(directory) + "/" + path.filename().string();
      bool owned = true;
      try { storage::validate_reference(ref, directory, extension); }
      catch (const DbError&) { owned = false; }
      // Only recognized immutable/temp leaves are owned; never recurse or follow
      // symlinks, nor delete unrelated user files in the data directory.
      if (owned && !live.contains(ref)) {
        auto status = it->symlink_status(ec);
        if (ec) storage::fail("cannot inspect garbage");
        if (std::filesystem::is_directory(status)) storage::fail("directory in object namespace");
        checkpoint("before_garbage_remove");
        std::filesystem::remove(path, ec);
        if (ec) storage::fail("cannot remove garbage");
        checkpoint("after_garbage_remove");
      }
      it.increment(ec);
      if (ec) storage::fail("cannot enumerate garbage");
    }
    sync_path(dir, true);
  }
}
} // namespace minivectordb
