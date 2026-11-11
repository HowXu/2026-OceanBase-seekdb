#include "engine.hpp"
#include "executor.hpp"
#include <type_traits>

namespace minivectordb {
Engine::Engine(const std::filesystem::path& data_dir)
    : store_(data_dir), committed_root_(store_.recover()) {}

void Engine::collect_garbage() {
  try {
    if (transaction_root_) store_.collect_garbage({committed_root_, *transaction_root_});
    else store_.collect_garbage({committed_root_});
  } catch (...) {
    closed_ = true;
    throw DbError("fatal_storage_error", "root cleanup failed; restart required");
  }
}

void Engine::publish(Catalog& candidate) {
  try {
    store_.publish(candidate);
  } catch (const DbError& error) {
    if (error.code == "fatal_storage_error") closed_ = true;
    // Never clean up after uncertain CURRENT replacement. A retryable failure
    // must retain both the committed root and earlier transaction successes.
    if (!closed_) collect_garbage();
    throw;
  }
  static_assert(std::is_nothrow_swappable_v<Catalog>);
  std::swap(committed_root_, candidate);
}

QueryResult Engine::execute(const Statement& statement, SearchMode mode) {
  if (closed_) throw DbError("engine_closed", "engine is closed");
  if (std::holds_alternative<Begin>(statement)) {
    if (transaction_root_) throw DbError("transaction_active", "transaction already active");
    transaction_root_ = committed_root_;
    return EmptyResult{};
  }
  if (std::holds_alternative<Commit>(statement)) {
    if (transaction_root_) {
      publish(*transaction_root_);
      transaction_root_.reset();
      collect_garbage();
    }
    return EmptyResult{};
  }
  if (std::holds_alternative<Rollback>(statement)) {
    if (transaction_root_) {
      transaction_root_.reset();
      collect_garbage();
    }
    return EmptyResult{};
  }
  Catalog& visible_root = transaction_root_ ? *transaction_root_ : committed_root_;
  Catalog statement_candidate = visible_root;
  auto result = execute_scalar(statement, statement_candidate, store_, mode);
  if (!std::holds_alternative<Select>(statement)) {
    if (transaction_root_) std::swap(*transaction_root_, statement_candidate);
    else publish(statement_candidate);
    collect_garbage();
  }
  return result;
}

void Engine::close() {
  if (closed_) return;
  transaction_root_.reset();
  closed_ = true;
  collect_garbage();
}
}  // namespace minivectordb
