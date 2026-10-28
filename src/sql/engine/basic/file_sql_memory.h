/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <atomic>
#include <limits>
#include <map>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase { namespace sql { namespace file_sql {

inline constexpr size_t MAX_RECORD_BYTES = 16U * 1024 * 1024;
inline constexpr size_t MAX_COLUMNS = 4096;
inline constexpr size_t MAX_PARQUET_BYTES = 64U * 1024 * 1024;
inline constexpr size_t MAX_GLOB_MATCHES = 100000;
inline constexpr size_t MAX_WORKSPACE_BYTES = 256U * 1024 * 1024;

class MemoryBudget {
public:
  explicit MemoryBudget(size_t limit = MAX_WORKSPACE_BYTES,
                        std::pmr::memory_resource *upstream = std::pmr::new_delete_resource())
      : limit_(limit), upstream_(upstream) {}

  void reserve(size_t bytes)
  {
    size_t used = used_.load(std::memory_order_relaxed);
    do {
      if (bytes > limit_ - used) {
        throw Error("file SQL memory budget exceeded");
      }
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
  }
  void release(size_t bytes) noexcept { used_.fetch_sub(bytes, std::memory_order_relaxed); }
  size_t used() const { return used_.load(std::memory_order_relaxed); }

  void *allocate(size_t bytes, size_t alignment)
  {
    reserve(bytes);
    try {
      return upstream_->allocate(bytes, alignment);
    } catch (...) {
      release(bytes);
      throw;
    }
  }
  void deallocate(void *ptr, size_t bytes, size_t alignment) noexcept
  {
    upstream_->deallocate(ptr, bytes, alignment);
    release(bytes);
  }

private:
  size_t limit_;
  std::pmr::memory_resource *upstream_;
  std::atomic<size_t> used_{0};
};

// Only our allocators consult this scope; the process-wide allocator and PMR
// default resource are never changed. Each allocation retains its own budget.
inline thread_local std::shared_ptr<MemoryBudget> active_file_sql_budget;

class BudgetScope {
public:
  explicit BudgetScope(std::shared_ptr<MemoryBudget> budget = {})
      : previous_(active_file_sql_budget),
        budget_(budget ? std::move(budget) : previous_ ? previous_ : std::make_shared<MemoryBudget>())
  {
    active_file_sql_budget = budget_;
  }
  ~BudgetScope() { active_file_sql_budget = previous_; }
  BudgetScope(const BudgetScope &) = delete;
  BudgetScope &operator=(const BudgetScope &) = delete;
  const std::shared_ptr<MemoryBudget> &budget() const { return budget_; }

private:
  std::shared_ptr<MemoryBudget> previous_;
  std::shared_ptr<MemoryBudget> budget_;
};

// For standard-library API boundary copies and bounded path/descriptor
// bookkeeping whose storage cannot use BudgetAllocator.
class MemoryReservation {
public:
  explicit MemoryReservation(std::shared_ptr<MemoryBudget> budget, size_t bytes = 0)
      : budget_(std::move(budget)) { add(bytes); }
  ~MemoryReservation() { budget_->release(bytes_); }
  MemoryReservation(const MemoryReservation &) = delete;
  MemoryReservation &operator=(const MemoryReservation &) = delete;
  void add(size_t bytes) { budget_->reserve(bytes); bytes_ += bytes; }

private:
  std::shared_ptr<MemoryBudget> budget_;
  size_t bytes_ = 0;
};

template <typename T>
class BudgetAllocator {
public:
  using value_type = T;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  BudgetAllocator() noexcept : budget_(active_file_sql_budget) {}
  explicit BudgetAllocator(std::shared_ptr<MemoryBudget> budget) noexcept : budget_(std::move(budget)) {}
  template <typename U>
  BudgetAllocator(const BudgetAllocator<U> &other) noexcept : budget_(other.budget_) {}

  T *allocate(size_t count)
  {
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
      throw Error("file SQL allocation size overflow");
    }
    if (!budget_) {
      budget_ = active_file_sql_budget ? active_file_sql_budget : std::make_shared<MemoryBudget>();
    }
    return static_cast<T *>(budget_->allocate(count * sizeof(T), alignof(T)));
  }
  void deallocate(T *ptr, size_t count) noexcept
  {
    budget_->deallocate(ptr, count * sizeof(T), alignof(T));
  }
  template <typename U>
  bool operator==(const BudgetAllocator<U> &other) const noexcept { return budget_ == other.budget_; }
  template <typename U>
  bool operator!=(const BudgetAllocator<U> &other) const noexcept { return !(*this == other); }

private:
  template <typename U> friend class BudgetAllocator;
  std::shared_ptr<MemoryBudget> budget_;
};

inline void check_count(size_t count, size_t limit, const char *what)
{
  if (count > limit) {
    throw Error(std::string("file SQL ") + what + " limit exceeded");
  }
}

inline void reserve_standard_copy(MemoryReservation &reservation, size_t bytes)
{
  if (bytes > std::numeric_limits<size_t>::max() / 2) {
    throw Error("file SQL boundary copy size overflow");
  }
  // Include geometric growth/temporary capacity while crossing a std API.
  reservation.add(bytes * 2);
}

using BudgetString = std::basic_string<char, std::char_traits<char>, BudgetAllocator<char>>;
template <typename T> using BudgetVector = std::vector<T, BudgetAllocator<T>>;
template <typename K, typename V>
using BudgetMap = std::map<K, V, std::less<K>, BudgetAllocator<std::pair<const K, V>>>;

template <typename T>
BudgetString budget_to_string(T value)
{
  // Numeric conversion uses at most a bounded scalar-sized temporary.
  const auto text = std::to_string(value);
  return BudgetString(text.data(), text.size());
}

}}} // namespace oceanbase::sql::file_sql
