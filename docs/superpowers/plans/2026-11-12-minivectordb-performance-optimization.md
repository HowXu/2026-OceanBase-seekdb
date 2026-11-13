# MiniVectorDB 首轮性能优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不改变逻辑表语义的前提下，降低行解码、重复打开段和 INSERT 全表重复键扫描成本，并与固定基线做同口径比较。

**Architecture:** storage::Input 使用有界缓冲并提供 float 块读取；TableEntry 惰性持有可 reset 的 LogicalTableReader 和 COW 主键集合。三个改动分别验证，最后统一做三轮与 perf 复测。

**Tech Stack:** C++20、pybind11、pytest/CTest、Docker/cgroup v2、perf。

## Global Constraints

- 设计：`docs/superpowers/specs/2026-11-12-minivectordb-performance-optimization-design.md`。
- 固定基线：写入 T=6.732231464、B=11284480、M=42348544；查询 T=58.799768207、B=11288576、M=42373120。
- 不改变文件格式、SQL、协议、事务、恢复和公开材料。
- 每个新机制先有 RED；性能比较使用相同种子/镜像构建，保留退化结果。
- 不提交、不推送，除非后续明确授权。

### Task 1：有界 Input 缓冲与向量块读取

**Files:**
- Modify: `minivectordb/cpp/storage_codec.hpp`
- Modify: `minivectordb/cpp/table_file.cpp`
- Modify: `minivectordb/cpp/table_segments.cpp`
- Test: `minivectordb/tests/cpp/test_table_file.cpp`
- Test: `minivectordb/tests/cpp/test_table_segments.cpp`

- [ ] **Step 1：RED**

增加跨64KiB边界 VARCHAR/VECTOR 读写、全部 float bits、大块直接读取和截断测试。要求 `Input::floats` 尚不存在时编译失败；记录 `opt-task1-red.log`。

- [ ] **Step 2：实现缓冲**

Input 保存文件总大小和逻辑 left；64KiB `std::vector<char>`、pos/available。bytes 先复制现有缓冲，空缓冲时预读，大目标直接读。left 只在实际交付给调用方时减少，remaining 包含缓冲未消费部分。

新增：

```cpp
void floats(float* destination, std::uint64_t count) {
  if (count > UINT64_MAX / sizeof(float)) fail("vector byte size overflow");
  bytes(reinterpret_cast<char*>(destination), count * sizeof(float));
}
```

- [ ] **Step 3：两个 reader 使用向量块读取**

用 `std::vector<float> vector(n); in_.floats(vector.data(),n)` 替代逐分量 u32 循环；保留维度/remaining 校验。运行 CTest 和完整 pytest，保存 GREEN。

- [ ] **Step 4：单轮同口径复测**

构建 `minivectordb:opt-buffer`，运行一轮到独立输出；与基线比较 T/B/M，不将单轮称为最终 median。

### Task 2：持有并重置 LogicalTableReader

**Files:**
- Modify: `minivectordb/cpp/storage_codec.hpp`
- Modify: `minivectordb/cpp/table_file.{hpp,cpp}`
- Modify: `minivectordb/cpp/table_segments.{hpp,cpp}`
- Modify: `minivectordb/cpp/catalog.hpp`
- Modify: `minivectordb/cpp/executor.cpp`
- Test: `minivectordb/tests/cpp/test_table_segments.cpp`
- Test: `minivectordb/tests/test_transactions.py`
- Test: `minivectordb/tests/test_recovery.py`

- [ ] **Step 1：RED**

C++ 测试要求同一 LogicalTableReader 在 reset 后可重复返回相同 PK/内容；覆盖 base+delta、tombstone和合并前状态。表段替换时 reader 失效，Catalog 复制未修改表时共享 reader。

- [ ] **Step 2：实现 reset**

Input.reset 使用保存的 size，clear/seekg(0)、清缓冲并恢复 left。两个段 reader reset 后重新读取 magic/schema。LogicalTableReader.reset 清空 heap，对所有 cursor 调底层 reset 并 advance。

- [ ] **Step 3：TableEntry 持有 reader**

增加 `mutable std::shared_ptr<LogicalTableReader> reader`。Executor 的 table_reader 惰性创建、每次扫描前 reset；现有 stack reader 改为该实例。replace_objects 清空变化表 reader，旧根与未修改表保持共享。

- [ ] **Step 4：GREEN 与单轮复测**

运行完整事务/恢复/故障测试及全套测试，构建 `minivectordb:opt-readers` 并做独立一轮。检查文件描述符和 M 无有界性异常。

### Task 3：INSERT 的 COW 主键集合

**Files:**
- Modify: `minivectordb/cpp/catalog.hpp`
- Modify: `minivectordb/cpp/executor.cpp`
- Test: `minivectordb/tests/test_incremental_storage.py`
- Test: `minivectordb/tests/test_transactions.py`

- [ ] **Step 1：RED**

增加重复键生命周期测试：初始集合惰性构建、事务内重复、回滚后重插入、失败多行 INSERT 后旧集合不变、删除后重插入、跨合并和重启。性能路径通过固定写入基准确认旧实现全表扫描、新实现集合查询。

- [ ] **Step 2：实现惰性集合**

TableEntry 增加 `mutable std::shared_ptr<std::unordered_set<std::int64_t>> primary_keys`。Executor 首次需要时用 table_reader 建立；INSERT 用 contains 检查已存在键，保留批内重复检查。

- [ ] **Step 3：COW 发布**

成功写入前复制集合并加入本批键，append_delta_or_compact 成功后写入候选 entry。旧根不原地修改。UPDATE/DELETE 保留集合，replace_objects 不因普通行修改清空 PK 集合。

- [ ] **Step 4：GREEN 与单轮复测**

运行完整测试，构建 `minivectordb:opt-pkset` 并做一轮；比较写入 T/B/M，同时确保查询无回归。

### Task 4：最终三轮、perf 与报告

**Files:**
- Modify: `tools/benchmark/README.md`
- Create: `docs/superpowers/experiments/2026-11-12-minivectordb-performance-optimization.md`

- [ ] **Step 1：完整验证**

运行11项或新增后的全部 CTest、完整 pytest、原公开20/20，检查生产 fault injection OFF。

- [ ] **Step 2：三轮同口径**

对最终镜像运行 `minivectordb_perf.py --rounds 3` 到新目录，独立重算 median、计数、全量内容和 Recall。列表格比较基线/最终 T/B/M 与变化比例。

- [ ] **Step 3：perf 复测**

用最终源码构建 RelWithDebInfo，分别采样纯写入和查询，比较热点是否从 iostream/多段读取/全表PK检查移动。保留所有样本和 correctness。

- [ ] **Step 4：边界与清理**

报告每项单独单轮结果，包括无收益项；不计算官方 E。检查 git diff、公开材料不变、无遗留容器/卷/perf进程。不 commit/push。
