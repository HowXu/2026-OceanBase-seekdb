#pragma once
#include <filesystem>
#include "types.hpp"
#include "root_store.hpp"

namespace minivectordb {
class Engine {
 public:
  explicit Engine(const std::filesystem::path& data_dir);
  QueryResult execute(const Statement& statement, SearchMode mode);
  void close();
 private:
  RootStore store_;
  Catalog committed_root_;
  std::optional<Catalog> transaction_root_;
  bool closed_ = false;
  void publish(Catalog& candidate);
  void collect_garbage();
};
}  // namespace minivectordb
