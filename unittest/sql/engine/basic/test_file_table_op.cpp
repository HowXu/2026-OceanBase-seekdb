/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/basic/ob_file_table_op.h"

namespace oceanbase { namespace sql {
namespace {
class FileTableOpTestPeer : public ObFileTableOp
{
public:
  using ObFileTableOp::ObFileTableOp;
  const ObBatchRows &batch() const { return brs_; }
};

TEST(FileTableOp, empty_glob_batch_reports_eof_and_close_is_idempotent)
{
  common::ObArenaAllocator allocator;
  ObExecContext ctx(allocator);
  ObFileTableSpec spec(allocator, PHY_FILE_TABLE);
  spec.is_glob_ = true;
  spec.max_batch_size_ = 16;
  FileTableOpTestPeer op(ctx, spec, nullptr);
  ASSERT_EQ(common::OB_SUCCESS, op.inner_get_next_batch(16));
  EXPECT_EQ(0, op.batch().size_);
  EXPECT_TRUE(op.batch().end_);
  EXPECT_EQ(common::OB_SUCCESS, op.inner_get_next_batch(16));
  EXPECT_EQ(common::OB_SUCCESS, op.inner_close());
  EXPECT_EQ(common::OB_SUCCESS, op.inner_close());
  op.destroy();
}
} // namespace
}} // namespace oceanbase::sql
