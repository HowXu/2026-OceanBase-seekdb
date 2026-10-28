/*
 * Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <limits>
#include <memory_resource>
#include "sql/engine/basic/file_sql_memory.h"

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
TEST(FileSqlMemory, boundary_overflow_and_reuse)
{
  MemoryBudget budget(64);
  budget.reserve(64);
  EXPECT_EQ(64U, budget.used());
  EXPECT_THROW(budget.reserve(1), Error);
  EXPECT_THROW(budget.reserve(std::numeric_limits<size_t>::max()), Error);
  EXPECT_EQ(64U, budget.used());
  budget.release(32);
  budget.reserve(32);
  EXPECT_EQ(64U, budget.used());
  budget.release(64);
  EXPECT_EQ(0U, budget.used());
  check_count(MAX_GLOB_MATCHES, MAX_GLOB_MATCHES, "glob match");
  EXPECT_THROW(check_count(MAX_GLOB_MATCHES + 1, MAX_GLOB_MATCHES, "glob match"), Error);
}

class FailingResource : public std::pmr::memory_resource {
  void *do_allocate(size_t, size_t) override { throw std::bad_alloc(); }
  void do_deallocate(void *, size_t, size_t) override {}
  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
};

TEST(FileSqlMemory, allocation_failure_rolls_back_charge)
{
  FailingResource upstream;
  auto budget = std::make_shared<MemoryBudget>(1024, &upstream);
  BudgetAllocator<int> allocator(budget);
  EXPECT_THROW(allocator.allocate(16), std::bad_alloc);
  EXPECT_EQ(0U, budget->used());
  EXPECT_THROW(allocator.allocate(std::numeric_limits<size_t>::max()), Error);
  EXPECT_EQ(0U, budget->used());
}

TEST(FileSqlMemory, sibling_budgets_share_query_usage_and_roll_back_failed_charge)
{
  size_t engine_bytes = 16;
  auto query = std::make_shared<MemoryBudget>(1024, std::pmr::new_delete_resource(),
      std::shared_ptr<MemoryBudget>{}, [&](size_t bytes) {
        if (bytes > 96 - engine_bytes) throw Error("query memory limit");
      });
  MemoryBudget left(64, std::pmr::new_delete_resource(), query);
  MemoryBudget right(64, std::pmr::new_delete_resource(), query);
  left.reserve(48);
  EXPECT_THROW(right.reserve(48), Error);
  EXPECT_EQ(48U, query->used());
  EXPECT_EQ(0U, right.used());
  left.release(16);
  right.reserve(48);
  EXPECT_EQ(80U, query->used());
  EXPECT_THROW(left.reserve(1), Error);
  EXPECT_EQ(32U, left.used());
  left.release(32);
  right.release(48);
  EXPECT_EQ(0U, query->used());
}

TEST(FileSqlMemory, parent_allocation_failure_releases_both_counters)
{
  auto query = std::make_shared<MemoryBudget>(1024);
  FailingResource upstream;
  auto child = std::make_shared<MemoryBudget>(1024, &upstream, query);
  BudgetAllocator<int> allocator(child);
  EXPECT_THROW(allocator.allocate(16), std::bad_alloc);
  EXPECT_EQ(0U, child->used());
  EXPECT_EQ(0U, query->used());
}

TEST(FileSqlMemory, cancellation_checks_do_not_require_an_allocation)
{
  bool cancelled = false;
  auto query = std::make_shared<MemoryBudget>(1024, std::pmr::new_delete_resource(),
      std::shared_ptr<MemoryBudget>{}, [&](size_t) {
        if (cancelled) throw Error("query cancelled");
      });
  auto scanner = std::make_shared<MemoryBudget>(1024, std::pmr::new_delete_resource(), query);
  BudgetScope scope(scanner);
  check_work();
  cancelled = true;
  EXPECT_THROW(check_work(), Error);
  EXPECT_THROW(scanner->reserve(32), Error);
  EXPECT_EQ(0U, scanner->used());
  EXPECT_EQ(0U, query->used());
}

TEST(FileSqlMemory, zero_reservations_do_not_poll_and_reused_reservations_release_usage)
{
  int checks = 0;
  auto budget = std::make_shared<MemoryBudget>(1024, std::pmr::new_delete_resource(),
      std::shared_ptr<MemoryBudget>{}, [&](size_t) { ++checks; });
  budget->reserve(0);
  EXPECT_EQ(0, checks);
  MemoryReservation output(budget);
  output.add(64);
  output.clear();
  EXPECT_EQ(0U, budget->used());
  output.add(32);
  EXPECT_EQ(32U, budget->used());
}

TEST(FileSqlMemory, nested_containers_and_copy_peak_share_one_budget)
{
  auto budget = std::make_shared<MemoryBudget>(4096);
  {
    BudgetScope scope(budget);
    BudgetVector<BudgetString> values;
    values.emplace_back(1024, 'a');
    EXPECT_GE(budget->used(), 1025U);
    {
      auto copy = values;
      EXPECT_GE(budget->used(), 2050U);
      EXPECT_THROW(copy.emplace_back(3072, 'b'), Error);
    }
    EXPECT_GE(budget->used(), 1025U);
  }
  EXPECT_EQ(0U, budget->used());
}

TEST(FileSqlMemory, allocator_keeps_budget_after_scope_exit)
{
  auto budget = std::make_shared<MemoryBudget>(4096);
  BudgetVector<int> values;
  {
    BudgetScope scope(budget);
    values.resize(128);
  }
  EXPECT_EQ(128U * sizeof(int), budget->used());
  values.resize(256);
  EXPECT_EQ(256U * sizeof(int), budget->used());
  { auto moved = std::move(values); }
  EXPECT_EQ(0U, budget->used());
}
} // namespace
}}} // namespace oceanbase::sql::file_sql
