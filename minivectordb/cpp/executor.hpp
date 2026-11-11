#pragma once
#include "catalog.hpp"
#include "root_store.hpp"

namespace minivectordb {
// Mutations update only the caller's statement candidate, never the visible root.
QueryResult execute_scalar(const Statement& statement, Catalog& statement_candidate,
                           RootStore& store, SearchMode mode);
}  // namespace minivectordb
