# MiniVectorDB 第二轮性能优化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在当前 `299538bf` 功能与性能基线上，分别验证可信读取、块编解码、建索引合并、INSERT 快速解析、距离计算与扫描行复用，只保留有同口径测量收益的改动。

**Architecture:** 保留 Python JSONL/SQLGlot 主路径、C++20 执行核心、不可变段和 CURRENT 原子发布。优化通过显式可信读模式、无格式变化的编解码、CREATE INDEX 时的候选根合并和严格 INSERT fast path 实现；任何非 fast path 语句仍走 SQLGlot。第五项是实验性优化，不包含整体 C++/Rust 重写。

**Tech Stack:** C++20、pybind11、Python 3.12、SQLGlot 30.31.0、CTest、pytest、Docker、Linux perf、既有 `tools/benchmark/`。

## Global Constraints

- 不修改 `STDIO_PROTOCOL.md`、`protocol_check/`、`mysqltest/`、公开用例、断言或资源配置。
- 不使用现成数据库、存储或向量引擎；不引入新的运行时服务、端口或子进程。
- 不改变 JSONL、SQL IR、段 magic、Catalog magic、CURRENT 发布和故障注入边界，除非某项测试证明现有实现本身错误。
- 可信读只跳过已经由 writer、publish 或 recovery 验证过的语义检查；marker、长度、截断、维度、顺序和 schema 检查不能删除。
- 不启用全局 `-ffast-math`，不假设评测机支持 AVX2/AVX512，不改变 float32 存储或 double 距离语义。
- 每项独立 RED→GREEN→单轮同口径复测；输出目录必须全新，失败和退化结果保留。
- 单轮只用于保留/回退决策；最终保留项合并后重新跑三轮，以每轮 E 所需原始 T/B/M 的中位数口径比较，不拼接最佳值。
- 当前未跟踪的 `agent/conversation.json` 和 SQL 差分报告不是本轮性能源码，不修改、不混入性能 diff。
- 本计划不自动 commit/push；Docker Hub 不可用时可用固定镜像做临时派生构建，但必须记录，生产 Dockerfile 不改。
- 本地没有官方分项基线；用户提供的680分只作平台反馈，不据此虚构 E、排名或下一轮得分。

## 固定验证与性能口径

功能命令：

```bash
export MINIVECTORDB_FAULT_BUILD="$PWD/build/task11-fault/python"
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
export VECTORDB_CONTAINER_ENGINE=docker
export TASK=task1  # 每个任务改成实际 task1…task5
export VECTORDB_IMAGE="minivectordb:r2-$TASK"
cmake --build build/task11-fresh -j2
cmake --build build/task11-fault -j2
ctest --test-dir build/task11-fresh --output-on-failure
.venv/bin/python -m pytest minivectordb/tests -q --basetemp="/tmp/mvdb-r2-$TASK"
```

每项构建上述独立镜像，并运行：

```bash
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --handshake-only --output "/tmp/mvdb-r2-$TASK-handshake"
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output "/tmp/mvdb-r2-$TASK-public"
.venv/bin/python tools/benchmark/minivectordb_perf.py \
  --image "$VECTORDB_IMAGE" --rounds 1 \
  --output ".superpowers/sdd/performance-round2/$TASK-round1"
```

固定负载仍为10,000行、128维、100批、1,000次 TopK、K=10、500次过滤、种子20261112。每轮必须完成全量内容、float32 bits、恢复、精确 ID/距离和 Recall 校验。保留项原则上要求目标 T 有可重复下降；单轮差异不足5%时先再跑一个独立轮次，不把噪声当收益。B/M 任一上升必须在报告中明确保留。

---

### Task 1: 已提交不可变段的可信热查询读取

**Files:**
- Modify: `minivectordb/cpp/table_file.hpp`
- Modify: `minivectordb/cpp/table_file.cpp`
- Modify: `minivectordb/cpp/table_segments.hpp`
- Modify: `minivectordb/cpp/table_segments.cpp`
- Modify: `minivectordb/cpp/executor.cpp:272-350`
- Test: `minivectordb/tests/cpp/test_table_file.cpp`
- Test: `minivectordb/tests/cpp/test_table_segments.cpp`
- Test: `minivectordb/tests/test_recovery.py`

**Interfaces:**
- Produces: `enum class RowValidation { Full, StructureOnly };`
- Produces: reader 构造函数末尾的可选 `RowValidation validation = RowValidation::Full`。
- Consumes: 既有 `RootStore::publish`、`load_root` 全量验证；默认参数确保旧调用仍为 Full。

- [ ] **Step 1: 写可信/完整读取对照测试**

覆盖同一合法 base/delta 的逐行内容一致；截断、非法 marker、错误 row length、维度不符和乱序在两种模式下都拒绝。另用一个人工构造的非有限 float 段证明 Full 拒绝，而 StructureOnly 只用于已经验证的当前根，不能把它当作新的公开写入入口。

```cpp
TableFileReader full(path, schema, RowValidation::Full);
TableFileReader trusted(path, schema, RowValidation::StructureOnly);
Row a, b;
while (full.next(a)) {
  REQUIRE(trusted.next(b));
  REQUIRE(a == b);
}
REQUIRE_FALSE(trusted.next(b));
```

- [ ] **Step 2: 运行 RED**

Run: `cmake --build build/task11-fresh -j2 && ctest --test-dir build/task11-fresh -R 'task04_streaming_table|task08_table_segments' --output-on-failure`

Expected: 编译失败，`RowValidation` 及对应构造函数尚不存在。

- [ ] **Step 3: 实现最小可信读模式**

在 `table_file.hpp` 定义枚举并传递模式；默认 Full。只条件化读取后的语义验证：

```cpp
if (validation_ == RowValidation::Full) validate_row(schema_, decoded);
```

LogicalTableReader 将同一模式传给 base 和全部 delta。`select_rows` 构造：

```cpp
LogicalTableReader reader(table_paths(entry, store), schema,
                          RowValidation::StructureOnly);
```

`validate_table_segment`、`validate_table_objects`、外部排序、恢复和发布继续使用默认 Full。writer 的 `validate_row` 不删除。

- [ ] **Step 4: 完整功能验证**

Expected: 11/11 CTest、188/188 pytest；新增测试后数量相应增加。重点运行事务内 SELECT、ROLLBACK、CREATE INDEX 后修改、缺段和恢复中再次强杀。公开 handshake 与20/20必须通过。

- [ ] **Step 5: 单轮性能复测并决定保留**

记录 `task1-round1/summary.json`。若查询 T 无可重复下降，或 Full 边界被削弱导致任何功能失败，则回退本项；否则保留原始 B/M，即使它们没有改善。

---

### Task 2: 标量输入和整数/向量块输出

**Files:**
- Modify: `minivectordb/cpp/storage_codec.hpp`
- Modify: `minivectordb/cpp/table_file.cpp`
- Modify: `minivectordb/cpp/table_segments.cpp`
- Test: `minivectordb/tests/cpp/test_table_file.cpp`
- Test: `minivectordb/tests/cpp/test_table_segments.cpp`

**Interfaces:**
- Produces: `void Output::floats(const float* source, std::uint64_t count);`
- Preserves: `Input::remaining()`、`Output::finish()` 和现有 little-endian 字节格式。

- [ ] **Step 1: 写字节级 RED**

对含 `INT64_MIN/MAX`、空/长 VARCHAR、128维向量、负零和跨64KiB边界的表写后读，逐位比较；同时检查输出文件与旧编码的已知字节序列一致。

```cpp
const std::uint64_t value = 0x0807060504030201ULL;
out.u64(value);
// 文件中必须依次是 01 02 03 04 05 06 07 08。
```

调用尚不存在的 `Output::floats(vector.data(), vector.size())` 作为 RED。

- [ ] **Step 2: 运行 RED**

Run: `cmake --build build/task11-fresh -j2`

Expected: 编译失败，`Output::floats` 不存在。

- [ ] **Step 3: 实现固定宽度整数块读写**

Input 使用一个私有 `fill()` 补充64KiB缓冲；`u8` 直接取当前字节并减少 `left_`。u32/u64 各读入固定数组一次，再按 little-endian 组合：

```cpp
std::uint64_t Input::u64() {
  std::array<char, 8> raw;
  bytes(raw.data(), raw.size());
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
    value |= std::uint64_t(static_cast<unsigned char>(raw[i])) << (8 * i);
  return value;
}
```

Output 对称实现，`floats` 检查 `count <= UINT64_MAX / 4` 后一次调用 `bytes`。两个 writer 用：

```cpp
out.floats(vector.data(), vector.size());
```

不改变 reader 的维度/remaining 检查，不使用裸宿主导出作为新格式。

- [ ] **Step 4: 功能与故障验证**

Expected: 全部 CTest/pytest、事务/恢复 fault 测试和公开20/20通过；写入后重启 bits 必须完全一致。

- [ ] **Step 5: 独立性能复测**

与 Task 1 保留版本比较写入和查询 T/B/M。输入和输出虽然同任务实现，perf 报告须分别列出 `Input::bytes/u64` 与 `Output::bytes/u32` 热点变化；若整体退化，保留证据并回退 Task 2，而不是回退 Task 1。

---

### Task 3: CREATE VECTOR INDEX 准备阶段确定性合并

**Files:**
- Modify: `minivectordb/cpp/executor.cpp:366-405,490-509`
- Test: `minivectordb/tests/test_incremental_storage.py`
- Test: `minivectordb/tests/test_vector_index.py`
- Test: `minivectordb/tests/test_transactions.py`
- Test: `minivectordb/tests/test_recovery.py`

**Interfaces:**
- Produces: `void compact_table(const std::string& table, const std::vector<DeltaRecord>& changes, Catalog& candidate, RootStore& store);`
- Reuses: 既有 LogicalTableReader、TableFileWriter、OwnedObjects、Catalog::replace_object。
- Preserves: FlatScan 描述符、事务候选根、主键集合内容和 CURRENT 原子发布。

- [ ] **Step 1: 写合并触发 RED**

创建含基础段和多个 delta 的表，交错 INSERT/UPDATE/DELETE，记录 `CREATE VECTOR INDEX` 前的行内容、ANN/exact 结果和可见 PK。建索引后断言 Catalog 只有一个 base 对象，逻辑行和主键集合不变，旧根在 ROLLBACK 时仍指向旧段。

```python
before = select_all(engine)
keys_before = primary_keys(engine)
create_vector_index(engine)
assert object_count(engine, 'items') == 1
assert select_all(engine) == before
assert primary_keys(engine) == keys_before
```

同时覆盖空表、已有索引名冲突、事务内建索引后回滚、建索引后继续 DML、ready 前强杀。

- [ ] **Step 2: 运行 RED**

Run: `.venv/bin/python -m pytest minivectordb/tests/test_incremental_storage.py -q --basetemp=/tmp/mvdb-r2-task3-red`

Expected: 建索引后对象数仍大于1，新断言 FAIL。

- [ ] **Step 3: 提取并实现 compact_table**

将现有“超过64段”的合并主体提取为辅助函数，不复制第二套合并逻辑：

```cpp
void compact_table(const std::string& table,
                   const std::vector<DeltaRecord>& changes,
                   Catalog& candidate, RootStore& store) {
  const auto entry = candidate.find(table);
  OwnedObjects owned(store);
  auto ref = owned.create();
  TableFileWriter writer(store.object_path(ref), entry.schema);
  LogicalTableReader reader(table_paths(entry, store), entry.schema);
  Row row;
  std::size_t i = 0;
  auto emit = [&](const DeltaRecord& change) {
    if (!change.deleted) writer.append(change.row);
  };
  while (reader.next(row)) {
    auto current_key = key(row, entry.schema);
    while (i < changes.size() && changes[i].key < current_key) emit(changes[i++]);
    if (i < changes.size() && changes[i].key == current_key) {
      emit(changes[i++]);
      continue;
    }
    writer.append(row);
  }
  while (i < changes.size()) emit(changes[i++]);
  writer.finish();
  candidate.replace_object(table, ref);
  owned.release(ref);
}
```

超过阈值的 DML 调用 `compact_table(table, changes, candidate, store)`，以上述归并代码为唯一实现。CREATE INDEX 先执行 `add_vector_index` 做全部名称/列检查，再仅当 `objects.size()>1` 时以空 changes 调用合并。异常只丢弃 statement_candidate，不改旧根。

- [ ] **Step 4: 完整功能与准备成本验证**

Expected: 全部测试通过。记录 CREATE INDEX 的独立耗时、B/M；确认合并发生在查询计时之外，但不从 B/M 中删除。

- [ ] **Step 5: 单轮复测并评估交换**

重点比较查询 T、多段堆/Delta 热点、准备阶段耗时、B 和 M。若 T 无可重复下降，或准备成本导致本地预算/正确性失败，则回退。即使 T 下降但 M/B 明显上升，也必须在报告中给出取舍，不隐藏。

---

### Task 4: 严格多行 INSERT C++ fast path，SQLGlot 保持 fallback

**Files:**
- Create: `minivectordb/cpp/sql_fast_path.hpp`
- Create: `minivectordb/cpp/sql_fast_path.cpp`
- Modify: `minivectordb/cpp/engine.hpp`
- Modify: `minivectordb/cpp/engine.cpp`
- Modify: `minivectordb/cpp/bindings.cpp`
- Modify: `minivectordb/python/minivectordb/__main__.py`
- Modify: `minivectordb/CMakeLists.txt`
- Create: `minivectordb/tests/cpp/test_sql_fast_path.cpp`
- Test: `minivectordb/tests/test_frontend.py`
- Test: `minivectordb/tests/test_protocol.py`
- Test: `minivectordb/tests/test_sql_semantics.py`

**Interfaces:**
- Produces: `std::optional<Insert> try_parse_insert_values(std::string_view sql);`
- Produces: `std::optional<QueryResult> Engine::try_execute_insert(std::string_view sql, SearchMode mode);`
- Binding: `Engine.try_execute_insert(sql, search_mode)` 返回 `None` 表示不匹配的语句形状，否则返回既有 result payload。
- Fallback: 未匹配的 DDL、SELECT、UPDATE、DELETE、目标列列表、带列限定或复杂语法继续 `parse_statement`。

- [ ] **Step 1: 写 fast/fallback 差分测试**

有效覆盖：大小写 INSERT/VALUES、100行、空字符串、Unicode、`''` 引号转义、int64 边界、向量字符串和末尾单分号。无效覆盖：尾随逗号、缺右括号、重复主键、非法 BIGINT、NaN/Inf、错误维度、多语句和孤立 surrogate。每个 fast 成功结果与 SQLGlot 路径的 IR/执行结果一致；fast 不匹配的合法语句必须能 fallback。

```cpp
auto parsed = try_parse_insert_values(
    "INSERT INTO items VALUES (1,2,'a','[0,1]'),(2,3,'b','[1,0]')");
REQUIRE(parsed.has_value());
REQUIRE(parsed->rows.size() == 2);
REQUIRE(try_parse_insert_values("SELECT 1") == std::nullopt);
```

- [ ] **Step 2: 运行 RED**

Run: `cmake --build build/task11-fresh -j2`

Expected: 新测试目标因 `sql_fast_path.hpp` 和符号不存在而编译失败。

- [ ] **Step 3: 实现严格、有界的快速解析**

只识别简单表名和无目标列的 `INSERT INTO ... VALUES (...),...`：

- ASCII 大小写不敏感关键字；标识符不匹配时返回 nullopt；
- 十进制整数直接用 `from_chars<int64_t>`，拒绝溢出；
- 字符串支持 SQL 单引号及双单引号转义；遇反斜杠等未实现方言返回 nullopt，而不是自行发明语义；
- 一旦已明确识别为 fast INSERT 形状，语法错误直接抛 `unsupported_sql`，不能 fallback 给更宽松的 SQLGlot；
- 向量仍作为 string Literal 交给既有 `bind_literal/parse_vector`，不复制第二套浮点转换；
- 完整消费空白和可选单个分号，拒绝额外内容。

Python 在调用 native 前保留 `sql.encode('utf-8')` 检查；返回 None 才调用 SQLGlot。成功 fast path 仍进入同一 `Engine::execute`，所以 PK 集合、事务、发布和错误映射不另建路径。

- [ ] **Step 4: 完整验证与解析分解**

Expected: 全部测试通过。重新运行 `.superpowers/sdd/performance-round2` 同形状的 parse-only 分解，fast INSERT 不应再出现 SQLGlot parser 热点；所有非 fast SQL 的公开结果保持一致。

- [ ] **Step 5: 单轮性能与回退条件**

比较写入 T/B/M。目标是从上一保留版本获得至少10%的可重复写入 T 下降；若未达到，或严格 SQL 差分出现任何错误，则回退 fast path。查询 T 不应被宣传为 fast INSERT 的直接收益。

---

### Task 5: 距离计算与单基础段扫描行复用实验

**Files:**
- Modify: `minivectordb/cpp/exact_search.cpp`
- Modify: `minivectordb/cpp/table_file.cpp`
- Modify: `minivectordb/cpp/executor.cpp`
- Test: `minivectordb/tests/cpp/test_types.cpp`
- Test: `minivectordb/tests/cpp/test_table_file.cpp`
- Test: `minivectordb/tests/test_exact_search.py`
- Test: `minivectordb/tests/test_vector_index.py`

**Interfaces:**
- Preserves: `double l2_distance_squared(const std::vector<float>&, const std::vector<float>&)`。
- Produces: TableFileReader 对传入 `Row&` 的原位解码复用；单基础段查询可直接使用 TableFileReader。
- Depends: 行复用的固定负载收益通常依赖 Task 3 已把表合并为单基础段；若 Task 3 被回退，单独记录该依赖。

- [ ] **Step 1A: 为距离实验建立位级 RED**

用固定随机向量、极大/极小有限 float、负零、相同距离和近并列样本，比较候选实现与当前顺序 double 累加的 `uint64_t` bits，并验证 `(distance,id)` 顺序。

```cpp
auto expected = std::bit_cast<std::uint64_t>(reference_l2(stored, query));
auto actual = std::bit_cast<std::uint64_t>(candidate_l2(stored, query));
REQUIRE(actual == expected);
```

- [ ] **Step 2A: 只做可证明等价的距离实验**

不使用重关联或 fast-math。若候选循环重排不能保持所有位级测试，立即停止该分支并保留 RED/失败证据。只有位级一致时才跑单轮基准；无可重复收益则恢复当前简单循环。

- [ ] **Step 1B: 为扫描行复用写 RED**

同一 Row 连续读取至少三行，记录 vector data/capacity，验证非 TopK 选中行能复用分配且内容不串行；单基础段直接扫描与 LogicalTableReader 的结果逐行一致。截断和 schema 错误仍失败。

```cpp
Row row;
REQUIRE(reader.next(row));
auto* storage = std::get<std::vector<float>>(row[vector_index]).data();
REQUIRE(reader.next(row));
REQUIRE(std::get<std::vector<float>>(row[vector_index]).data() == storage);
```

- [ ] **Step 2B: 原位解码和单段扫描**

TableFileReader 按 schema 对已有 Row 的 Cell 逐列 resize/assign，字符串和 vector 复用 capacity；移动进 TopK heap 后允许新分配，不能把已选结果别名化。`select_rows` 仅在 `entry.objects.size()==1` 时使用直接 base reader，其他表仍走 LogicalTableReader。Full/StructureOnly 模式继续生效。

- [ ] **Step 3: 分开测量5A和5B**

使用不同镜像/输出目录，不能把5A与5B合并后只报一个收益。每项均跑完整 CTest/pytest、精确 ID/距离、并列、Recall 和恢复。无收益或位级语义变化的分支独立回退。

---

## 最终验收

- [ ] 对最终保留组合构建新镜像，原公开 handshake、20/20、95 SQL通过。
- [ ] 最新 CTest/pytest、fault 事务/恢复、2GiB超内存、ready前强杀全部通过。
- [ ] 使用新目录运行 `--rounds 3`，独立重算三轮 median；写入和查询分别报告 T/B/M，并保留 M/B 不利变化。
- [ ] 重新 perf 写入/查询，确认热点移动；写样本过少时只作定性结论。
- [ ] 报告每项单轮、回退项、最终三轮和构建边界，不计算无官方基线的正式 E。
- [ ] 清理容器、卷和 perf 进程；确认协议/公开包零 diff。未经用户新授权不 commit/push。
