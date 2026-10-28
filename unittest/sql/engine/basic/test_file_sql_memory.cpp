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
