#pragma once
#include "catalog.hpp"
#include <filesystem>
#include <set>
#include <vector>
#ifdef MINIVECTORDB_TESTING
#include <functional>
#endif

namespace minivectordb {
class RootStore {
public:
  explicit RootStore(const std::filesystem::path &data_dir);
  const std::filesystem::path &data_dir() const { return data_dir_; }
  // Reserves an exclusive empty candidate under objects/. SQL names are never
  // paths.
  std::string create_object();
  std::filesystem::path object_path(const std::string &reference) const;
  // Writes a new metadata object but DOES NOT publish CURRENT or promise
  // durability.
  std::string write_catalog(const Catalog &catalog);
  Catalog read_catalog(const std::string &reference) const;
  // Empty only when CURRENT is absent. Existing roots and all rows are
  // validated.
  Catalog load_root() const;
  // Validate CURRENT completely before reclaiming abandoned candidates.
  Catalog recover();
  // Closed immutable objects already reside in their final directory. Publication
  // syncs them before the one atomic multi-table CURRENT replacement.
  void publish(const Catalog& candidate);
  // Always protects CURRENT plus any active transaction/statement roots supplied
  // by the caller. Call only at safe statement boundaries, not during a rewrite.
  void collect_garbage(const std::vector<Catalog>& protected_roots = {});
#ifdef MINIVECTORDB_TESTING
  // Compiled only into native tests; never exported by the production extension.
  void set_test_hook(std::function<void(const std::string&)> hook) {
    test_hook_ = std::move(hook);
  }
#endif

private:
  std::filesystem::path data_dir_;
  bool poisoned_ = false;
  std::set<std::string> committed_objects_;
  std::string current_reference() const;
  void checkpoint(const std::string& name) const;
#ifdef MINIVECTORDB_TESTING
  std::function<void(const std::string&)> test_hook_;
#endif
  std::filesystem::path checked_path(const std::string &reference,
                                     const std::string &directory,
                                     const std::string &extension) const;
  std::string reserve(const std::string &directory,
                      const std::string &extension);
};
} // namespace minivectordb
