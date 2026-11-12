# MiniVectorDB 五项修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 ORDER BY、名称/DDL、向量转换、默认列名和整表重写五类问题，同时保持 JSONL、事务与 CURRENT 崩溃安全语义。

**Architecture:** Python 将受支持 SQL 降级为包含序号、显示文本和已验证限定名的 IR；C++ 统一执行大小写不敏感绑定与向量转换。存储以基础段加有序增量段表示逻辑表，DML 仅写变更，RootStore 继续通过一次 CURRENT 替换原子发布多表候选根。

**Tech Stack:** Python 3.12、SQLGlot 30.31.0、C++20、pybind11、CMake/CTest、pytest、Docker。

## Global Constraints

- 设计依据：`docs/superpowers/specs/2026-11-12-minivectordb-five-fixes-design.md`。
- 不修改 `STDIO_PROTOCOL.md`、`protocol_check`、`mysqltest` 或公开配置/断言。
- 不增加 NULL、JOIN、聚合、SELECT 1、OFFSET、DROP 等额外功能。
- 不使用现有数据库/向量引擎，不改变 Python+C++ 同进程架构。
- 每项先保存 RED 证据，再写生产代码；测试失败时修产品，不改预期迁就实现。
- 不切换分支，不创建 worktree，不提交、不推送；保留已有未跟踪文档。
- 构建使用 `build/task11-fresh` 与 `build/task11-fault`，证据写入 `.superpowers/sdd/five-fixes/`。

## 文件结构

- `minivectordb/python/minivectordb/frontend.py`：限定名、表级主键、括号、序号和原始投影文本降级。
- `minivectordb/cpp/types.{hpp,cpp}`：IR 类型、ASCII 名称规范化、Schema 校验、向量转换。
- `minivectordb/cpp/bindings.cpp`：严格解析新增 IR 字段。
- `minivectordb/cpp/executor.cpp`：序号/别名/显示列名绑定；后续接入增量 DML。
- `minivectordb/cpp/catalog.{hpp,cpp}`：规范化 Catalog 查找及表对象列表。
- `minivectordb/cpp/table_segments.{hpp,cpp}`（新增）：增量段读写与逻辑表归并。
- `minivectordb/cpp/root_store.{hpp,cpp}`：对象列表持久化、新段验证/同步和全段 GC。
- `minivectordb/tests/test_frontend.py`、`test_sql_semantics.py`：前端和端到端 SQL 回归。
- `minivectordb/tests/cpp/test_table_segments.cpp`（新增）：段归并与格式错误测试。
- `minivectordb/tests/test_recovery.py`、`test_transactions.py`：对象列表下的事务与故障安全。
- `minivectordb/CMakeLists.txt`：新源文件及 CTest。

---

### Task 1：ORDER BY 序号、括号与默认显示文本

**Files:**
- Modify: `minivectordb/python/minivectordb/frontend.py`
- Modify: `minivectordb/cpp/types.hpp`
- Modify: `minivectordb/cpp/bindings.cpp`
- Modify: `minivectordb/cpp/executor.cpp`
- Test: `minivectordb/tests/test_frontend.py`
- Test: `minivectordb/tests/test_sql_semantics.py`

**Interfaces:**
- Produces: `OrderPosition { std::uint64_t position; }`，只允许出现在 `OrderItem.expression`。
- Produces: `Projection { Expression expression; optional<string> alias; optional<string> display_text; }`。
- Produces: Python 投影字段 `display_text`，仅无 AS、非星号表达式携带。

- [ ] **Step 1：添加序号与显示文本 RED 测试**

在 `test_sql_semantics.py` 添加：

```python
@pytest.mark.parametrize('suffix,expected', [
    ('1,id', [[10,2],[20,1],[20,3]]),
    ('1 DESC,id', [[20,1],[20,3],[10,2]]),
    ('2 DESC', [[20,3],[10,2],[20,1]]),
])
def test_order_by_projection_ordinal(db, suffix, expected):
    db('CREATE TABLE ord (id BIGINT PRIMARY KEY,n BIGINT)')
    db('INSERT INTO ord VALUES (1,20),(2,10),(3,20)')
    assert db(f'SELECT n,id FROM ord ORDER BY {suffix}')['rows'] == expected

@pytest.mark.parametrize('position', ['0','2','18446744073709551616'])
@pytest.mark.parametrize('suffix', ['', ' LIMIT 0'])
def test_invalid_order_ordinal_is_rejected_before_scan(db, position, suffix):
    db('CREATE TABLE ord (id BIGINT PRIMARY KEY)')
    fails(db, f'SELECT id FROM ord ORDER BY {position}{suffix}', 'type_error')

def test_parentheses_and_unaliased_expression_display_text(db):
    db('CREATE TABLE disp (id BIGINT PRIMARY KEY,v VECTOR(2))')
    db("INSERT INTO disp VALUES (1,'[3,4]')")
    assert db("SELECT l2_distance(v,'[0,0]'), (id) FROM disp") == {
        'columns': ["l2_distance(v,'[0,0]')", '(id)'],
        'rows': [[5.0,1]],
    }
```

在前端测试中要求 `ORDER BY 1` 产生 `{"kind":"ordinal","position":1}`，无 AS 函数投影保留完整原始文本；将 `SELECT (id)` 从拒绝列表移除，将 `ORDER BY -1` 加入拒绝列表。

- [ ] **Step 2：运行并确认 RED**

```bash
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_frontend.py minivectordb/tests/test_sql_semantics.py -q --basetemp=/tmp/mvdb-five-task1-red | tee .superpowers/sdd/five-fixes/task1-red.log
```

预期：序号成为普通常量或 IR 不符合预期；括号与默认列名断言失败；无效序号错误成功。

- [ ] **Step 3：实现原始 select-list 文本切取与序号 IR**

在 `frontend.py` 新增辅助函数，使用 token 的 `start/end` 而非 AST 格式化：

```python
def projection_sources(sql, tokens, count):
    depth = 0
    from_index = None
    for i, token in enumerate(tokens[1:], 1):
        depth += token.token_type is TokenType.L_PAREN
        depth -= token.token_type is TokenType.R_PAREN
        if depth == 0 and token.token_type == TokenType.FROM:
            from_index = i
            break
    if from_index is None:
        unsupported("SELECT requires FROM")
    ranges, start, depth = [], 1, 0
    for i in range(1, from_index):
        token = tokens[i]
        if token.token_type == TokenType.L_PAREN: depth += 1
        elif token.token_type == TokenType.R_PAREN: depth -= 1
        elif token.token_type == TokenType.COMMA and depth == 0:
            ranges.append((start, i)); start = i + 1
    ranges.append((start, from_index))
    if len(ranges) != count:
        unsupported("cannot preserve projection source text")
    return [sql[tokens[a].start:tokens[b - 1].end + 1] for a, b in ranges]
```

从 `sqlglot.tokens` 导入 `TokenType`。`expression` 递归处理 `exp.Paren`。SELECT 先解析 FROM，再让 `column` 验证可选限定名；本任务允许 `{当前表, FROM别名}`，别名仍保持原拼写。

ORDER BY 对非字符串 `exp.Literal` 且文本只含数字时生成 `{"kind":"ordinal","position":int(...)}`；位置大于 `2**64-1` 时抛 `DatabaseError("type_error", "ORDER BY position is outside the projection")`，对 `exp.Neg` 数字明确 `unsupported("negative ORDER BY ordinal")`。投影无 AS 且非 Star 时加入对应 `display_text`。

在 C++ 类型中加入：

```cpp
struct OrderPosition { std::uint64_t position; bool operator==(const OrderPosition&) const = default; };
using Expression = std::variant<ColumnRef, Star, Literal, L2Distance, OrderPosition>;
struct Projection {
  Expression expression;
  std::optional<std::string> alias;
  std::optional<std::string> display_text;
};
```

`bindings.cpp` 严格接受 `ordinal` 和可选 `display_text`，但普通 `bind_expression` 遇到 `OrderPosition` 必须报 unsupported，防止其进入 WHERE/投影。

- [ ] **Step 4：在展开投影后绑定序号并使用显示文本**

`select_rows` 先展开 `*`，同时保留 `BoundExpression` 列表。非 AS 非星号结果列直接使用 `*p.display_text`。ORDER BY 的处理顺序为：

```cpp
if (auto position = std::get_if<OrderPosition>(&item.expression)) {
  if (position->position == 0 || position->position > projection.size())
    semantic("type_error", "ORDER BY position is outside the projection");
  bound = projection.at(position->position - 1);
} else {
  // 现有别名匹配与表达式绑定；别名匹配在 Task 2 改为大小写不敏感。
}
```

序号绑定仍在 `LIMIT 0` 返回之前。普通字面量投影的显示文本来自 SQL，不再 `to_string` 重建。

- [ ] **Step 5：重建并验证 GREEN**

```bash
cmake --build build/task11-fresh -j2
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_frontend.py minivectordb/tests/test_sql_semantics.py -q --basetemp=/tmp/mvdb-five-task1-green | tee .superpowers/sdd/five-fixes/task1-green.log
```

预期：目标测试及既有等价/歧义排序测试全部通过。

---

### Task 2：名称规范化、重复定义与表级主键

**Files:**
- Modify: `minivectordb/python/minivectordb/frontend.py`
- Modify: `minivectordb/cpp/types.{hpp,cpp}`
- Modify: `minivectordb/cpp/catalog.cpp`
- Modify: `minivectordb/cpp/executor.cpp`
- Test: `minivectordb/tests/test_frontend.py`
- Test: `minivectordb/tests/test_sql_semantics.py`

**Interfaces:**
- Produces: `std::string normalized_name(std::string_view)`，仅将 ASCII A–Z 转为 a–z。
- Preserves: Schema/Catalog 和 AS 的原始显示拼写。

- [ ] **Step 1：添加名称与表级主键 RED 测试**

```python
def test_ascii_case_insensitive_names_preserve_display_spelling(db):
    db('CREATE TABLE Docs (ID BIGINT PRIMARY KEY,N BIGINT,V VECTOR(2))')
    db("INSERT INTO docs (id,n,v) VALUES (2,10,'[0,2]'),(1,20,'[3,4]')")
    db("UPDATE DOCS SET n=11 WHERE Id=2")
    assert db('SELECT docs.ID AS Out,n AS Score FROM DOCS ORDER BY SCORE DESC,out') == {
        'columns':['Out','Score'], 'rows':[[1,20],[2,11]]}
    db('CREATE VECTOR INDEX IX ON docs(v)')
    fails(db, 'CREATE VECTOR INDEX ix ON DOCS(V)', 'index_exists')
    fails(db, 'DELETE FROM Docs WHERE MISSING=1', 'unknown_column')

def test_table_primary_key_and_case_duplicate_names(db):
    db('CREATE TABLE tablepk (id BIGINT,n BIGINT,PRIMARY KEY(ID))')
    db('INSERT INTO TABLEPK VALUES (1,7)')
    assert db('SELECT * FROM tablepk') == {'columns':['id','n'],'rows':[[1,7]]}
    fails(db, 'CREATE TABLE dup (id BIGINT PRIMARY KEY,ID BIGINT)', 'type_error')
    fails(db, 'CREATE TABLE dup (id BIGINT PRIMARY KEY,n BIGINT,PRIMARY KEY(id))', 'type_error')
    fails(db, 'CREATE TABLE dup (id BIGINT,n VARCHAR(4),PRIMARY KEY(n))', 'type_error')

def test_wrong_qualifier_is_not_silently_ignored(db):
    db('CREATE TABLE t (id BIGINT PRIMARY KEY)')
    fails(db, 'SELECT wrong.id FROM t', 'unknown_column')
    fails(db, 'SELECT a.id FROM t AS b', 'unknown_column')
```

- [ ] **Step 2：运行 RED 并保存**

```bash
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_frontend.py minivectordb/tests/test_sql_semantics.py -q --basetemp=/tmp/mvdb-five-task2-red | tee .superpowers/sdd/five-fixes/task2-red.log
```

预期：大小写引用、表级主键和限定名相关用例失败。

- [ ] **Step 3：实现前端表级主键和限定名验证**

CREATE 的 Schema 子项分别处理 `exp.ColumnDef` 与 `exp.PrimaryKey`。PrimaryKey 必须恰有一个 `exp.Identifier`，通过大小写规范化名称找到对应列，并将该列 IR 的 `primary_key` 设为 true；找不到主键列抛 `DatabaseError("unknown_column",...)`；重复或多个主键声明抛 `DatabaseError("type_error",...)`。继续让 C++ 验证 BIGINT 与唯一主键。

`table_reference` 接受 `exp.Table(this,alias)`，alias 只允许一个 Identifier。SELECT 的 `expression/where` 接收由当前表名和别名组成的 qualifier 集；`exp.Column` 若带 table 字段，不匹配则报 `unknown_column`，匹配后只降级列名。UPDATE/DELETE 的 qualifier 集仅含目标表。

- [ ] **Step 4：实现原生统一名称查找**

在 `types.hpp/.cpp` 添加：

```cpp
std::string normalized_name(std::string_view name);
inline bool same_name(std::string_view a, std::string_view b) {
  return normalized_name(a) == normalized_name(b);
}
```

`validate_schema` 用规范化名称判重；`column_index`、Catalog 表/索引查找和唯一性、ORDER BY 输出别名匹配均改用 `same_name` 或规范化 map key。保留 `ColumnSchema.name`、结果 alias 和 SQL 显示文本不变。错误限定名在 Python 直接抛 `DatabaseError("unknown_column",...)`，不依赖 C++ 猜测表上下文。

- [ ] **Step 5：验证 GREEN 和完整 SQL 回归**

```bash
cmake --build build/task11-fresh -j2
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_frontend.py minivectordb/tests/test_sql_semantics.py minivectordb/tests/test_exact_search.py -q --basetemp=/tmp/mvdb-five-task2-green | tee .superpowers/sdd/five-fixes/task2-green.log
```

预期：全部通过，原始输出拼写未被小写化。

---

### Task 3：向量十进制词法与下溢转换

**Files:**
- Modify: `minivectordb/cpp/types.cpp`
- Test: `minivectordb/tests/cpp/test_types.cpp`
- Test: `minivectordb/tests/test_sql_semantics.py`

**Interfaces:**
- Preserves: `std::vector<float> parse_vector(const std::string&, std::uint64_t)`，所有路径继续共用它。

- [ ] **Step 1：添加合法格式、下溢和溢出 RED 测试**

在 C++ 测试中对二维向量逐项检查 bit pattern：

```cpp
for (const auto* text : {"[+1,01]", "[.5,1.]", "[1e-46,-1e-50]", "[1e-999,0]"}) {
  auto value = parse_vector(text, 2);
  check(value.size() == 2);
  for (float x : value) check(std::isfinite(x));
}
check(std::bit_cast<std::uint32_t>(parse_vector("[1e-46,0]",2)[0]) == 0);
check(std::bit_cast<std::uint32_t>(parse_vector("[-1e-50,0]",2)[0]) == 0x80000000u);
for (const auto* text : {"[1e39,0]","[1e309,0]","[NaN,0]","[Inf,0]",
                         "[+.,0]","[1e,0]","[1e+,0]","[1,]","[1 2,0]"})
  rejects([&] { parse_vector(text, 2); });
```

Python 增加 INSERT、UPDATE、L2_DISTANCE 三条路径，确认 `[1e-46,0]` 实际回读为 `[0.0,0.0]`，旧行在非法向量失败后保持不变。

- [ ] **Step 2：构建并确认 RED**

```bash
cmake --build build/task11-fresh -j2
ctest --test-dir build/task11-fresh -R types --output-on-failure | tee .superpowers/sdd/five-fixes/task3-red.log
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_sql_semantics.py -q --basetemp=/tmp/mvdb-five-task3-red | tee -a .superpowers/sdd/five-fixes/task3-red.log
```

预期：前导加号/零、省略整数或小数部分和下溢输入被拒绝。

- [ ] **Step 3：实现 double 解析后有限 float32 转换**

修改数字扫描：接受 `+`/`-`，记录小数点两侧数字总数，总数必须大于零；指数仍必须有数字。完整 token 用 `std::from_chars<double>` 解析。前导 `+` 可在语法验证后去掉再调用 from_chars。

若 from_chars 返回 `result_out_of_range`，仅对该完整 token 使用 `std::strtod` 复核：`end` 必须完整消费，结果必须有限；double 下溢到零允许，double 上溢到 Inf 拒绝。然后：

```cpp
float stored = static_cast<float>(value);
if (!std::isfinite(stored)) invalid("nonfinite or out-of-range float32");
result.push_back(stored);
```

包含 `<cerrno>`、`<cstdlib>`。不用 `from_chars<float>` 的范围错误直接判断下溢，不放宽 NaN/Inf 词法。

- [ ] **Step 4：验证 GREEN 与数值边界**

```bash
cmake --build build/task11-fresh -j2
ctest --test-dir build/task11-fresh -R types --output-on-failure | tee .superpowers/sdd/five-fixes/task3-green.log
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_sql_semantics.py minivectordb/tests/test_exact_search.py -q --basetemp=/tmp/mvdb-five-task3-green | tee -a .superpowers/sdd/five-fixes/task3-green.log
```

预期：合法下溢、负零和最大有限 float32 通过；溢出、NaN/Inf、维度与语法错误保持拒绝。

---

### Task 4：对象列表 Catalog、增量段格式与逻辑表归并

**Files:**
- Create: `minivectordb/cpp/table_segments.hpp`
- Create: `minivectordb/cpp/table_segments.cpp`
- Modify: `minivectordb/cpp/catalog.{hpp,cpp}`
- Modify: `minivectordb/cpp/root_store.cpp`
- Modify: `minivectordb/cpp/executor.cpp`
- Modify: `minivectordb/CMakeLists.txt`
- Create: `minivectordb/tests/cpp/test_table_segments.cpp`
- Modify: `minivectordb/tests/cpp/test_root_store.cpp`

**Interfaces:**
- `TableEntry { TableSchema schema; std::vector<std::string> objects; }`，首项基础段，后续为增量段。
- `Catalog::replace_objects(const std::string&, std::vector<std::string>)`。
- `DeltaRecord { std::int64_t key; bool deleted; Row row; }`。
- `DeltaFileWriter(path,schema)` / `append(const DeltaRecord&)` / `finish()`。
- `LogicalTableReader(object_paths,schema)` / `bool next(Row&)`。

- [ ] **Step 1：添加段归并和 Catalog 对象列表 RED 测试**

新 C++ 测试建立基础行 `{1:a,3:c}`，第一段更新 1、删除 3、插入 2，第二段重插入 3 并更新 2，断言：

```cpp
LogicalTableReader reader(paths, schema);
Row row;
std::vector<std::int64_t> keys;
while (reader.next(row)) keys.push_back(key(row,schema));
check(keys == std::vector<std::int64_t>({1,2,3}));
```

同时断言同主键取最新完整行、DELETE 不输出、非严格递增增量写入失败、截断段/错误 Schema/错误 magic 读取失败。RootStore 测试改为 `entry.objects`，并验证多段 root 重启后全部可读、缺失任一提交段均拒绝 ready。

- [ ] **Step 2：编译并确认 RED**

```bash
cmake --build build/task11-fresh -j2 2>&1 | tee .superpowers/sdd/five-fixes/task4-red.log
```

预期：新类型/接口尚不存在或旧 Catalog 单对象断言失败；不能把编译命令环境错误当作目标 RED。

- [ ] **Step 3：实现增量段和逻辑表 reader**

增量段 magic 使用 `MVDBD001`，接着写 Schema；记录按主键严格递增，footer 为 0，PUT marker 为 1，DELETE marker 为 2。PUT 写完整行，DELETE 不写行体；记录长度与 footer 检查沿用 TableFileReader 的边界策略。

`LogicalTableReader` 为每个段保存当前 record 和段序号，使用最小堆选择主键；相同主键时段序号较大的记录优先。取同主键整组后，只保留最新 PUT 作为输出，随后推进组内所有游标。基础段 reader 将普通行适配为 `deleted=false` 的 DeltaRecord。

提供只读校验函数：

```cpp
void validate_table_objects(const std::vector<std::filesystem::path>& paths,
                            const TableSchema& schema);
```

它流式消费 `LogicalTableReader`，不将整个表载入内存。

- [ ] **Step 4：迁移 Catalog、RootStore 和现有执行路径**

Catalog magic 升级为 `MVDBC003`，为每表编码对象数及有序对象引用。GC、recover 和 publish 的对象枚举均覆盖整个 vector。`load_root` 用 `validate_table_objects` 验证逻辑表。

本任务中 INSERT/UPDATE/DELETE 仍合并成单一新基础段，因此 `replace_objects(table,{new_base})` 后所有既有行为应保持不变；SELECT 和 compaction 输入改用 LogicalTableReader。创建空表仍写基础段。外部排序临时 run 格式不变。

- [ ] **Step 5：验证段测试及现有回归 GREEN**

```bash
cmake --build build/task11-fresh -j2
cmake --build build/task11-fault -j2
export MINIVECTORDB_FAULT_BUILD="$PWD/build/task11-fault/python"
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
ctest --test-dir build/task11-fresh --output-on-failure | tee .superpowers/sdd/five-fixes/task4-green.log
.venv/bin/python -m pytest minivectordb/tests -q --basetemp=/tmp/mvdb-five-task4-green | tee -a .superpowers/sdd/five-fixes/task4-green.log
```

预期：全部通过；此任务结束时仍允许整表重写，增量 DML 在下一任务实现。

---

### Task 5：增量 DML、确定性合并与新段发布

**Files:**
- Modify: `minivectordb/cpp/executor.cpp`
- Modify: `minivectordb/cpp/root_store.{hpp,cpp}`
- Test: `minivectordb/tests/test_sql_semantics.py`
- Test: `minivectordb/tests/test_transactions.py`
- Test: `minivectordb/tests/test_recovery.py`
- Test: `minivectordb/tests/cpp/test_root_store.cpp`

**Interfaces:**
- Produces: `append_delta_or_compact(table,changes,candidate,store,owned)`，增量达到 64 后，下一次修改生成 `{new_base}`。
- Produces: RootStore 已提交对象集合，用于只验证/同步候选中新对象。

- [ ] **Step 1：添加写入放大、无操作与跨合并 RED 测试**

新增 Python 原生集成测试，在同一已提交表上连续单行 UPDATE：

```python
def test_single_row_updates_do_not_rewrite_whole_base(db, tmp_path):
    db('CREATE TABLE inc (id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(32))')
    payload = 'x' * 32
    db("INSERT INTO inc VALUES " + ','.join(
        f"({i},{i},'{payload}')" for i in range(256)))
    before = {p.name:p.stat().st_size for p in (tmp_path/'objects').iterdir()}
    for i in range(64):
        assert db(f'UPDATE inc SET n={1000+i} WHERE id={i}') == {'affected_rows':1}
    after = {p.name:p.stat().st_size for p in (tmp_path/'objects').iterdir()}
    assert sum(after.values()) - sum(before.values()) < 64 * 1024
    assert db('SELECT id,n FROM inc ORDER BY id LIMIT 3')['rows'] == [[0,1000],[1,1001],[2,1002]]
```

再加入 65 次以上 UPDATE/DELETE/重插入、事务回滚、COMMIT 后重开、无匹配 UPDATE 不新增数据对象、失败多行 INSERT 不留下候选对象。测试不得仅数文件，还须逐行校验最终状态和恢复状态。

- [ ] **Step 2：运行 RED**

```bash
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_sql_semantics.py minivectordb/tests/test_transactions.py minivectordb/tests/test_recovery.py -q --basetemp=/tmp/mvdb-five-task5-red | tee .superpowers/sdd/five-fixes/task5-red.log
```

预期：旧整表重写字节数断言失败；对象列表下的事务/恢复测试暴露任何遗漏引用。

- [ ] **Step 3：实现只写变更的 DML**

INSERT 排序并检查批内重复，通过 LogicalTableReader 检查冲突，将 additions 转为 PUT DeltaRecord。UPDATE 扫描逻辑表，仅收集匹配行的完整更新；DELETE 仅收集匹配主键的墓碑。无 changes 时不创建对象并复用原列表。

`append_delta_or_compact`：

```text
objects.size() <= 64：写一个 delta，追加到候选 objects。
objects.size() == 65：把当前逻辑表与 changes 归并写入一个新基础段，候选 objects={new_base}。
```

所有 changes 在写段前完成 Schema/重复验证；写入过程按 PK 严格递增。失败由 OwnedObjects 回收新段。返回 affected_rows 仍使用匹配/新增行数。

- [ ] **Step 4：发布只处理新对象**

RootStore 增加 `std::set<std::string> committed_objects_`。recover 从 CURRENT 填充；publish 开始时以它判断候选新对象，仅对新对象执行单段结构验证与 fsync：位置 0 的新基础段用 TableFileReader 完整消费，后续新增量段用 DeltaFileReader 完整消费；不为发布重新扫描未变化的基础段。完整跨段逻辑校验仍在 recover 的 `validate_table_objects` 中执行。发布成功后将集合精确替换为 candidate 的对象集合；失败前不改变已提交状态。

保留现有 checkpoint 名称与顺序，尤其 `objects_synced`、`before_current_replace`、`current_replaced`、`data_dir_synced`。更新 fault build 和 recovery 测试，验证多表事务仍是全旧或全新，缺失任一 delta 不能 ready，GC 同时保护基础段、增量段和事务候选段。

- [ ] **Step 5：验证 GREEN 与小型写入复测**

```bash
cmake --build build/task11-fresh -j2
cmake --build build/task11-fault -j2
export MINIVECTORDB_FAULT_BUILD="$PWD/build/task11-fault/python"
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
ctest --test-dir build/task11-fresh --output-on-failure | tee .superpowers/sdd/five-fixes/task5-green.log
.venv/bin/python -m pytest minivectordb/tests/test_sql_semantics.py minivectordb/tests/test_transactions.py minivectordb/tests/test_recovery.py -q --basetemp=/tmp/mvdb-five-task5-green | tee -a .superpowers/sdd/five-fixes/task5-green.log
python3 .superpowers/sdd/teammate-comparison/probe_storage_amplification.py | tee .superpowers/sdd/five-fixes/task5-storage-before-docker-rebuild.log
```

最后一个命令若仍指向旧镜像，只能作为旧基线记录，不能用于证明新实现；新实现的 Docker 写入验收在 Task 6 用新镜像执行。

---

### Task 6：完整回归、差分复测与干净 Docker 验收

**Files:**
- Modify: `minivectordb/README.md`（仅更新实际格式与语义）
- Modify: `docs/superpowers/experiments/2026-11-12-minivectordb-known-good-comparison.md`（追加修复结果，不改旧实测）
- Create: `docs/superpowers/experiments/2026-11-12-minivectordb-five-fixes-verification.md`

- [ ] **Step 1：运行完整本机验证**

```bash
export MINIVECTORDB_FAULT_BUILD="$PWD/build/task11-fault/python"
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
export VECTORDB_CONTAINER_ENGINE=docker
cmake --build build/task11-fresh -j2
cmake --build build/task11-fault -j2
ctest --test-dir build/task11-fresh --output-on-failure | tee .superpowers/sdd/five-fixes/final-ctest.log
.venv/bin/python -m pytest minivectordb/tests -q --basetemp=/tmp/mvdb-five-final-pytest | tee .superpowers/sdd/five-fixes/final-pytest.log
```

预期：无失败、无资源清理 warning；若测试引用旧默认 PYTHONPATH，显式使用上述环境，不修改测试来绕过。

- [ ] **Step 2：从干净源码构建新 Docker 镜像**

```bash
git diff --exit-code f4d4e26653af823a7601668802f80e374ad6643c -- STDIO_PROTOCOL.md protocol_check mysqltest
docker build --no-cache -t minivectordb:five-fixes . 2>&1 | tee .superpowers/sdd/five-fixes/final-docker-build.log
docker image inspect minivectordb:five-fixes > .superpowers/sdd/five-fixes/final-image-inspect.json
```

不使用旧 `325ba...` 镜像证明新代码。检查镜像 Cmd/Entrypoint、动态库和生产故障注入关闭状态。

- [ ] **Step 3：运行原公开驱动和定向 Docker 差分**

```bash
python3 protocol_check/run.py --image minivectordb:five-fixes --handshake-only --output /tmp/mvdb-five-handshake
python3 protocol_check/run.py --image minivectordb:five-fixes --case sql_crud --output /tmp/mvdb-five-crud
python3 protocol_check/run.py --image minivectordb:five-fixes --output /tmp/mvdb-five-public
```

将差分脚本复制到新的证据目录，只把 `ours-c4a54eb` 镜像名替换为 `minivectordb:five-fixes`、标签改为 `ours-five-fixes`，不改 SQL 或独立预期。五个目标组必须与参考行为一致；未纳入范围的额外语法允许继续不同。

- [ ] **Step 4：执行新镜像写入与恢复验收**

对 `probe_storage_amplification.py` 做同样的仅镜像/输出目录参数化，运行同一 8,000 行、64 次单行 UPDATE。读取新 JSON 并断言：

```python
assert result['rows'] == 8000
assert result['single_row_updates'] == 64
assert max(result['update_wbytes'].values()) < 31_247_565
assert result['before_crash']['rows'] == result['final']['rows']
assert result['cleaned'] is True
```

再运行超过 64 段合并阈值的连续修改、两次实际 KILL 恢复，以及此前 2 GiB 超 RAM 诊断。每个新容器使用独立卷，完成后清理。

- [ ] **Step 5：记录证据和剩余边界**

验证报告列出每项 RED/GREEN 命令、测试数、镜像 ID、公开 20/20、差分原始结果、写入字节、合并与恢复结果。保留不利结果，不把本地通过写成正式隐藏用例通过。

最后执行：

```bash
git diff --check
git diff --exit-code HEAD -- STDIO_PROTOCOL.md protocol_check mysqltest
git status --short --branch
docker ps -a --filter name=vectordb-judge- --format '{{.Names}}'
docker volume ls --filter name=vectordb-judge- --format '{{.Name}}'
```

不 stage、不 commit、不 push。
