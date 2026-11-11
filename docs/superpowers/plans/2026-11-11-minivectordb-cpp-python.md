# MiniVectorDB C++ / Python Implementation Plan

> **For agentic workers:** 用户已确认 pybind11，并授权实施阶段最多同时运行 3 个 subagent。按本计划依赖图和文件归属执行，使用 `superpowers:executing-plans` 或 `superpowers:subagent-driven-development`；每任务以少量独立测试固定行为。当前请求仅更新文档，不启动实现或 subagent。

**Goal:** 使用 C++ 与 Python 实现轻量向量数据库，首先打通仓库现有测试链路，然后使 `mysqltest/manifest.json` 的 20 个公开用例、95 条 SQL 全部通过；压力测试、ANN 加速和性能排名优化在功能基线完成后另行开展。

**Architecture:** Python 在单个数据库进程内负责 JSONL 协议、SQLGlot 解析与 AST 到内部语句的转换，通过 pybind11 调用 C++ 核心。C++ 自行实现类型检查、执行、精确向量查询、磁盘存储、语句原子性、事务和恢复。初版使用不可变表文件与原子提交根指针，不采用现成数据库或向量索引引擎。

**Tech Stack:** Linux x86_64、Python 3.12、C++20、CMake、SQLGlot 30.31.0、pybind11 3.1.0；测试使用 CTest、pytest 9.2.0 与仓库原有 `protocol_check/run.py`。依赖版本在构建材料中固定。

## Global Constraints

- 当前本地分支为 `kernel/round2`；正式提交目标分支是题目要求的 `2026-competition-round2`。本轮不切分支、不提交代码。
- 本轮只更新本计划文档，不覆盖现有 `.gitignore` 修改和已有未跟踪文档。
- 每个任务配置少量独立开发测试作为完成门槛；`mysqltest/cases` 是赛题集成/提交验收集，不作为日常每任务的唯一测试。
- 实施阶段允许最多同时运行 3 个 subagent（实现、测试、评审合计），不是必须凑满 3 个；禁止子代理再次派生代理绕过上限。
- 初版目标是公开功能测试，不宣称公开样例通过等于正式功能准入通过。
- 运行期单机、单进程、单客户端、最多一个显式事务；Python 与 C++ 在同一进程，不为解析器逐语句启动子进程。
- 对外协议以当前仓库 `STDIO_PROTOCOL.md`、`protocol_check/` 的实际行为为准：stdin/stdout JSONL，不是 MySQL 网络协议，也不是裸 SQL 输入。
- SQL 使用题目规定的 MySQL 风格子集，不以完整 MySQL 行为代替本题语义。特别是排序并列、Unicode、事务和向量规则。
- 只使用 SQLGlot 的 tokenizer/parser/AST，不使用它的执行引擎、优化执行能力或其他数据库代执行。
- 存储、查询执行、向量检索和事务核心自行实现。不得以 SQLite、RocksDB、Faiss、VSAG、HNSWlib 等代执行。
- 数据、提交元数据、未提交文件、排序临时文件统一写入数据目录；容器默认 `/data`。
- 不修改公开 `.test`、`.result`、manifest、断言规则或资源配置来制造通过；数据库实现运行时不读取这些测试答案。
- 所有 SQL 暂时返回 `ok:false` 只允许出现在协议/解析骨架阶段；必须有非空错误码，不得将其标成 SQL 功能通过。
- BEGIN、COMMIT、ROLLBACK、shutdown 与预计的 SQL 错误均须有明确语义；错误后不能无故中止整个会话。
- 先实现精确扫描。`ann` 初版可明确使用精确回退，但不得声称已实现近似索引加速或满足未验证的正式 ANN 结构要求。
- 本轮不跑压测，不引入 GPU、HNSW、PQ、复杂优化器、MVCC、并发控制、后台 compaction 或分布式功能。

---

## 1. 已核对的仓库事实

### 1.1 权威入口

| 路径 | 用途 |
| --- | --- |
| `STDIO_PROTOCOL.md` | 当前实际 JSONL 通信格式、启动、关闭、重启 |
| `mysqltest/README.md` | 公开样例范围、断言规则、正式要求边界 |
| `mysqltest/manifest.json` | `minivectordb-public-v4`，20 个样例、95 条 SQL |
| `mysqltest/cases/*.test` | 每个非空非注释行是一条完整 SQL 或驱动指令 |
| `mysqltest/cases/*.result` | JSON 断言数组，不是通信响应模板 |
| `protocol_check/run.py` | 可直接复用的自测入口，支持本地进程和 Docker |
| `protocol_check/_transport.py` | ready、请求、shutdown、超时、重启、强杀的通信实现 |
| `protocol_check/_assertions.py` | 类型、浮点、列、行和错误断言 |
| `protocol_check/config.json` | 当前 2 CPU、2 GiB、128 PID；启动 120 秒、请求 60 秒、用例 600 秒 |
| `protocol_check/example_stdio.py` | 仅通信示例，execute 返回 `NOT_IMPLEMENTED` |
| `Dockerfile` | 当前为空，需要实现阶段补齐 |
| `conversation.json` | 当前根目录文件为空；学习材料路径需要单独与正式入口核对，不与 SQL 功能混为一谈 |

与先前网页描述有歧义时，本计划采用本仓库的实际协议：**每个请求是一行 JSON，SQL 是其中的 `sql` 字段**。

### 1.2 本轮已经做过的验证，不是数据库实现成绩

1. 逐项核对 manifest 的 SQL 数量与 result 数量：20 个用例、95 条 SQL，对应一致。
2. 运行现有通信示例的 `--handshake-only`：退出码 0、握手 PASS。
3. 用现有通信示例运行公开用例：退出码 1、20 个用例 FAIL，原因均为 CREATE 返回 `NOT_IMPLEMENTED`，没有超时。
4. 在 `/tmp` 独立目录安装 SQLGlot 30.31.0，使用 MySQL 方言解析 95 条 SQL：94 条得到结构化 AST，1 条 `CREATE VECTOR INDEX` 退化为 `exp.Command`。
5. `VECTOR(2)` 可得到类型 AST；`L2_DISTANCE` 为函数 AST；BEGIN/COMMIT/ROLLBACK 分别为事务节点；负整数需要处理 `Neg(Literal)`，不能转 float。

SQLGlot 探测没有安装进项目环境。开发和构建环境仍须按任务建立。探测只证明语法层兼容性，不证明语义或执行正确。

### 1.3 测试集覆盖与里程碑

| 组 | 公开用例 | 目标 |
| --- | --- | --- |
| 基础 SQL（8） | `sql_crud`、`sql_comparisons`、`sql_ordering`、`sql_empty`、`sql_primary_key`、`sql_unicode`、`sql_bigint`、`sql_unsupported` | 标量类型与执行正确 |
| 存储（3） | `storage_restart`、`storage_mutations`、`storage_multiple_tables` | 定义、记录、修改在重启后保留 |
| 向量（4） | `vector_euclidean`、`vector_ties_and_filter`、`vector_float32`、`vector_index_maintenance` | 精确距离、过滤、排序、索引声明及维护语义 |
| 事务（5） | `transaction_commit`、`transaction_rollback`、`transaction_failed_insert`、`transaction_autocommit_atomic`、`transaction_shutdown_rollback` | 提交、回滚、失败语句原子性、关闭回滚 |

公开样例没有强杀指令，不能用正常重启 PASS 替代崩溃恢复验证。计划补充少量确定性的强杀/重入恢复回归测试，但不做压力测试。超过内存预算的大数据功能和正式 ANN 验收仍属于后续正式准入检查，不以当前 20 个样例替代。

## 2. 路线与取舍

### 2.1 C++ / Python 连接方式

| 方案 | 优点 | 问题 | 决定 |
| --- | --- | --- | --- |
| Python 主进程 + pybind11 C++ 扩展 | SQLGlot 原生可用；单进程；整数和向量可以明确转换 | 需要构建扩展与约束边界类型 | 用户已确认采用 |
| Python 解析服务 + C++ 通过管道调用 | 边界容易观察 | 常驻多进程或逐请求建进程，不符合本题单进程基线 | 不采用 |
| C++ 嵌入完整 Python 解释器 | 可实现同进程 | 解释器初始化与生命周期工作更多，本轮无必要 | 不采用 |

Python 入口是数据库进程，C++ 扩展不是外部服务。正常运行不启动子进程、不监听端口。

### 2.2 存储与事务基线

采用 **不可变表文件 + 目录元数据根 + 原子 CURRENT 切换**，本轮不同时实现 WAL。

- 表文件按主键升序保存，逐行读写；不把整张表还原成 Python list 或 C++ map。
- 一条修改语句流式产生新的表文件；修改前的文件保持不变。
- 执行成功前不改变当前可见根；失败丢弃候选文件。
- 自动提交将候选根持久化并发布后才响应成功。
- BEGIN 保存已提交根，事务中的成功语句只更新进程内工作根；查询读取工作根。
- COMMIT 持久化工作根并原子发布；ROLLBACK 丢弃工作根。
- 多表事务的提交点是一个根指针，不是逐表覆盖原文件。
- 重启只使用 CURRENT 指向的完整目录元数据，不寻找“编号最大的文件”作为最新提交。

选择原因：在单事务条件下直接表达语句原子性、事务回滚和进程强杀恢复，避免初版同时维护日志重放、撤销和独立向量索引状态。

代价：每条修改可能重写整个受影响表，I/O 放大明显；不作为最终性能方案。不可达旧文件必须回收，不能靠无限累积文件维持正确性。后续是否改为增量日志/页式更新，须以测量和正确性回归为前提。

## 3. 接口先行

### 3.1 对外 JSONL

启动恢复完成后，主动输出且仅输出一次：

```json
{"type":"ready"}
```

请求及响应示例：

```json
{"id":1,"op":"execute","sql":"SELECT id FROM docs;","search_mode":"exact"}
{"id":1,"ok":true,"columns":["id"],"rows":[[1]]}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1);","search_mode":"exact"}
{"id":2,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
{"id":3,"op":"shutdown"}
{"id":3,"ok":true}
```

契约：

- `id` 原样回传，保持 JSON 类型；C++ 不负责 id。
- 每个请求恰好一个响应，单物理行、UTF-8、换行并立即 flush。
- `ok` 必须是 bool，不能是字符串或整数。
- SELECT 保留空结果的列名；修改返回精确 `affected_rows`。
- `.result` 的 `{}` 表示成功且不额外断言，不表示输出空对象。
- 错误返回非空 `error.code`；正式映射未冻结时仍使用稳定的语义名，而不是让所有错误都叫 `NOT_IMPLEMENTED`。
- shutdown 先回滚未提交事务并清理，再响应、flush、退出 0；之后不再输出。
- EOF 同样关闭并回滚，不额外发送无对应请求的响应。
- 使用严格 JSON 解析，拒绝重复键和非有限数；序列化禁止 NaN/Infinity；诊断全部写 stderr。
- `--restart`、`--mode exact`、`--mode ann` 由测试驱动处理，不进入 SQLGlot。

### 3.2 Python 接口与内部语句 IR

```python
# frontend.py
# 返回只含 dict/list/str/bool/int 的受控 IR，不返回 SQLGlot AST 给 C++。
def parse_statement(sql: str) -> dict: ...

# _minivectordb，由 pybind11 提供。
class Engine:
    def __init__(self, data_dir: str): ...  # 打开并恢复；成功后方可 ready
    def execute(self, statement: dict, search_mode: str = "exact") -> dict: ...
    def close(self) -> None: ...           # 回滚活动事务、关闭；可重复调用

# 成功返回不含 id/ok 的 payload：
# {} / {"affected_rows": 1} / {"columns": [...], "rows": [...]}
# 预计 SQL 错误抛出带 code/message 的 DatabaseError，由入口转成失败响应。
```

IR 只表示本题子集，不直接使用 SQLGlot 的 `dump()` 当长期接口：

| `op` | 必需信息 |
| --- | --- |
| `create_table` | `table`、按声明顺序的 `columns`；类型为 bigint/varchar/vector，长度与主键标记 |
| `insert` | `table`、可选目标列列表、每行字面量 |
| `select` | `table`、投影表达式列表、AND 比较条件、排序项、可选 LIMIT |
| `update` | `table`、赋值列表、AND 比较条件 |
| `delete` | `table`、AND 比较条件 |
| `create_vector_index` | `index`、`table`、`column` |
| `begin` / `commit` / `rollback` | 无数据载荷 |

标量字面量保留词法含义，例如：

```json
{"op":"insert","table":"docs","rows":[[{"kind":"integer","text":"9007199254740993"},{"kind":"string","text":"中文"}]]}
```

整数从十进制文本精确转 int64，不能经过 double。向量 SQL 字符串在绑定到 VECTOR 列/函数参数时由 C++ 严格解析并转换 float32；VARCHAR 不因内容看起来像数组就自动转换。Python 只降级语法，不代执行过滤、排序、类型检查或距离计算。

AST 必须逐节点白名单检查：即使根节点是 SELECT，也不能静默丢掉 JOIN、DISTINCT、GROUP BY、子查询或其他未支持子句。每个请求只接受一条有效 SQL，不使用 `split(';')` 切语句。合法字符串中的分号不构成边界。

### 3.3 C++ 公共边界

```cpp
// engine.hpp；Statement / QueryResult / DbError 在 types.hpp 中定义。
class Engine {
 public:
  explicit Engine(const std::filesystem::path& data_dir);
  QueryResult execute(const Statement& statement, SearchMode mode);
  void close();
};
```

内部统一类型：`Cell = variant<int64_t, string, vector<float>>`；`Row` 按表列顺序保存 Cell；`QueryResult` 区分空成功、受影响行数、结果集；`DbError` 保存语义错误码和消息。`Statement` 是上表语句的 tagged variant，`SearchMode` 只有 Exact/Ann。

绑定层负责 IR 到 Statement 和结果到 Python 的精确转换，执行器不依赖 SQLGlot 类名。BIGINT 返回 Python int；float32 向量以存储值返回；距离以有限 double 返回。C++ 的 DbError 通过绑定层转换成 `minivectordb.errors.DatabaseError`，与前端使用相同的 `code`/`message` 异常契约；不以裸 RuntimeError 代替可识别的 SQL 错误。

## 4. 文件布局（下面均为实施时的新文件）

```text
minivectordb/
  CMakeLists.txt                  # 核心、扩展、CTest；不接旧 seekdb 大工程
  requirements.txt               # 固定运行/构建依赖
  requirements-dev.txt           # 固定测试依赖
  README.md                      # 构建、启动、公开用例命令、已知边界
  python/minivectordb/
    __init__.py
    __main__.py                  # JSONL 入口、id、flush、错误、关闭
    frontend.py                  # SQLGlot AST → 子集 IR
    dialect.py                   # CREATE VECTOR INDEX 的受控语法扩展
    errors.py                    # Python 前端错误结构
  cpp/
    types.hpp                    # schema、Cell、IR、结果、错误、模式
    bindings.cpp                 # pybind11 与边界转换
    engine.hpp / engine.cpp      # 会话、执行分发、事务状态
    table_file.hpp / table_file.cpp  # 二进制表文件、流式扫描/重写
    catalog.hpp / catalog.cpp    # 表定义、表文件引用、索引声明
    root_store.hpp / root_store.cpp  # 提交发布、恢复、文件回收
    executor.hpp / executor.cpp  # 绑定、标量 CRUD、过滤、投影、排序
    exact_search.hpp / exact_search.cpp # 距离与精确 TopK
  tests/
    cpp/test_table_file.cpp
    cpp/test_root_store.cpp
    cpp/test_types.cpp
    test_protocol.py
    test_frontend.py
    test_ir_boundary.py
    test_sql_semantics.py
    test_exact_search.py
    test_transactions.py
    test_persistence.py          # Task 7 的正常重启小测试
    test_recovery.py             # Task 10 的确定性故障小测试
    test_vector_index.py         # Task 9 独立测试，避免与 Task 6 同时改文件
    test_container.py            # Task 10 的镜像小交互，不读取赛题 cases
    test_public_diagnostics.py
    test_delivery.py             # Task 11 的报告校验小测试
    test_optimization_contract.py # Task 12 获批后才创建的正确性护栏
  tools/
    run_public.py                # 仅开发诊断：沿用官方 transport/assertions，继续收集失败
    check_delivery.py            # Task 11 检查提交验收报告，不生成或替代成绩
```

仓库根 `Dockerfile` 在任务 1 创建骨架、任务 10 完成真实镜像验收。`build/`、`.venv/`、`protocol-results/` 是本地产物，不写入数据库源目录。忽略规则如需调整，只追加本实现产生的路径，不覆盖用户当前改动。

## 5. 存储、原子性和精确查询设计

### 5.1 文件与可见根

```text
/data/
  CURRENT                       # 一个目录元数据文件的相对名字
  catalogs/<unique-id>.bin       # schema、索引声明、各表对象路径
  objects/<unique-id>.tbl        # 不可变表文件
  staging/                      # 正在生成的候选文件、排序临时文件
```

- 使用不复用的对象 ID；SQL 标识符不直接拼接为文件路径。
- Catalog 持有各表 schema、主键列位置、当前表文件引用和索引描述符；只把这些元数据常驻内存。
- 表文件头含格式标识、列布局；行使用长度前缀编码，BIGINT 用明确字节序的 64 位整数、VARCHAR 用 UTF-8 字节、VECTOR 用明确字节序的 IEEE float32。禁止直接 dump 含 padding 的 C++ 结构体。
- 表按主键升序，无需初版 B+ 树。INSERT 将输入行按主键排序后与表流合并；UPDATE/DELETE 流式改写，主键不可修改。
- 输入批次可在请求大小范围内缓存；数据库历史记录不全量载入。普通排序需要全排序时用受限内存 runs + 磁盘归并；TopK 使用 O(K) 堆。
- 响应集仍受协议单条响应大小限制；不把“能流式扫描大表”误写成“无限结果集也无内存限制”。

### 5.2 三层根与语句原子性

维护 `committed_root`、可选 `transaction_root`、临时 `statement_candidate`：

1. 查询读取活动事务根，否则读取 committed_root。
2. 修改先从当前可见根创建候选根，并将受影响表写入新对象。
3. 字段转换、维度检查、主键检查和文件写出全部成功后，候选根才可变为可见根。
4. 任何失败销毁候选根；已有可见根和显式事务此前成功语句不受影响。
5. 在显式事务内只切换工作根；在自动提交模式调用持久化发布流程。
6. 同一多行 INSERT 中重复的主键、与表中冲突的主键、后续行非法向量都使整句失败。
7. 索引描述符位于同一 Catalog，向量查询读取相同可见表文件，避免普通查询与向量查询采用不同状态。

这不是复制整张表到内存做 undo；复制的是目录元数据，记录新版本写磁盘。

### 5.3 持久化提交与恢复

提交顺序：

1. 完整写出所有候选表对象，关闭写入并 fsync 新对象。
2. 将新对象移入最终对象目录，fsync 对象目录。
3. 写出新 Catalog，fsync 文件及 Catalog 目录。
4. 在 `/data` 内写临时 CURRENT 文件，内容指向新 Catalog，fsync 该文件。
5. 在同一文件系统内 rename 替换 CURRENT，fsync `/data`。
6. 更新内存已提交根，结束事务，再返回成功。

CURRENT 的原子替换是可见提交切换点。多表变更不能分开发布。若发布期间出现 I/O 错误，尤其 CURRENT 已替换但目录同步失败，进程不得用旧内存根继续提供服务；不返回成功，诊断后退出，重启按 CURRENT 恢复。这类不确定提交符合“未收到成功响应可全生效或全不生效”，不能部分生效。

恢复：读取 CURRENT → 读取并验证其 Catalog → 打开引用表文件 → 建立 committed_root → 清除不可达文件 → ready。未提交根没有发布，不会恢复为可见状态。不得只在 shutdown 落盘。

清理只删除不被已提交根、活动事务根或当前候选根引用的文件；基础版在安全语句边界/回滚后及启动时清理。恢复时先确定可达集合再删除，删除中再次被杀死后重复执行仍安全。已发布 CURRENT 缺失引用文件时失败启动，不静默创建空库；首次初始化中未发布的孤立文件不作为已提交数据。

### 5.4 精确查询

- 先在当前可见记录上应用 WHERE，再进行排序或 TopK，不先取全表 K 条再过滤。
- VECTOR 列值和查询向量都转换为有限 float32，拒绝 NaN、Infinity、溢出和维度不符。
- 距离计算使用存储后的 float32 分量，先提升为 double 再做减法、平方与累加，输出平方根；不输出平方距离，不在 float 中做大数平方。
- TopK 堆比较键为 `(distance_squared, primary_key)`；最终输出按距离、主键排序，K 大于候选数时返回全部，空集合返回空 rows 和正确 columns。
- 普通 ORDER BY 按显式方向比较，显式项并列时始终主键升序；不能因 DESC 把隐含主键也反转。
- BIGINT 用 int64 精确比较；VARCHAR 长度按 Unicode 码点数，比较按码点顺序，不调用区域相关 collation。
- `CREATE VECTOR INDEX` 校验并持久化表、列、名字与索引描述符；初版后端是自行实现的 FlatScan，exact 和 ann 都使用当前可见记录精确扫描。不是简单无操作返回成功，也不宣称存在图索引。

## 6. 可执行任务清单

下述命令是实施后应运行的验收命令；除第 1.2 节记录外，本轮没有执行这些待建模块的测试。每项使用红→绿测试循环，任务完成后可形成独立提交，但实际 git commit 由执行阶段用户授权决定。

### 6.1 两层测试与任务完成门槛

**L1：任务小测试。** 每任务保留约 2–5 组短小、确定性的行为测试，以几条记录、临时数据目录或模拟会话完成。输入与 expected 独立写在开发测试中，不导入、遍历或复制整套 `mysqltest/cases`。必要的同类边界用少量参数化数据，不额外建设大型测试框架。Task 1–10 以本任务小测试、必要的直接依赖回归和构建通过为完成条件；Task 11 自身也有小测试；Task 12 获批执行时仍须有正确性护栏。

**L2：赛题集成/提交验收。** `mysqltest/manifest.json` 及 20 对 test/result 保持原样；在 Task 11 冻结/提交候选版本时，由主代理统一运行原 `protocol_check/run.py`。它不替代 L1，不要求每完成一个子任务就重跑全套；需要定位集成问题时可手动选择单个 case。最终仍必须真实达到 20/20 PASS，不能因 L1 通过而跳过 L2。该本地集成检查不发起正式平台提测。

统一完成凭证：任务编号、修改文件、固定行为、测试命令与退出码、直接依赖回归、已知未完成项。一个后续任务不能靠 skip/xfail 已有小测试来“完成”。没有 Docker 等环境能力时单列 BLOCKED，不把跳过算通过。

### 6.2 最多 3 个 subagent 的并行规则

- 主代理负责接口冻结、调度、共享文件接线、结果审阅和最终 L2 验收；最多 3 个子代理并行，评审代理也占一个名额。
- 每个可写子代理使用独立 worktree，一个 worktree 只有一个写入者；不得在当前有用户改动的工作区并发写入。建立隔离基线前保留现有改动，不自动 stash、reset 或提交用户文件。计划若尚未进入基线，应将已确认文档作为明确附件交给子代理，不能让其猜测。
- Task 1 先串行完成，冻结第 3 节 IR、公共 C++ 类型、异常和构建入口。Task 4 完成后再冻结表扫描/重写、Catalog、RootStore 接口；有依赖的子代理必须等待接口，不自行发明同名不同义的接口。
- 默认由主代理独占 `__main__.py`、`types.hpp`、`bindings.cpp`、`CMakeLists.txt`、依赖锁定文件和实现 README。并行任务需要修改共享接线时，提交具体接口/接线请求，由主代理串行集成；不能通过并行重写共享文件争抢接口。
- `engine.*` 的执行接线和事务状态变更、Catalog 定义的扩展，安排在批次集成时串行合并；任务归属文件见下表。工作目录隔离不等于可以无视语义冲突。
- 每个子代理交付实现与本任务小测试，说明修改范围、验证结果和未接线点。主代理先审 diff，再接线并重跑 L1；代理声称“通过”不自动等于验收完成。
- 不要求每批都启动 3 个；依赖未满足时等待或由主代理直接执行，不能为并行而绕过测试。Task 12 当前不得派发。

| 批次 | 可并行任务 | 写入归属与集成边界 |
| --- | --- | --- |
| A | Task 1，串行 | 主代理或 1 个子代理搭骨架；完成接口和小测试后再分流 |
| B | Task 2、Task 3、Task 4，最多 3 个 | 2：frontend/dialect 与解析测试；3：诊断工具与自建微型题包测试；4：table_file/catalog/root_store 基础与 C++ 测试；入口及公共类型由主代理接线 |
| C | Task 5、Task 7，最多 2 个 | 5：executor 与标量测试；7：root_store 提交/恢复与持久化测试；二者依赖 Task 4，engine/catalog 的共享变更由主代理串行整合 |
| D | Task 6、Task 8，最多 2 个 | 6：exact_search 与精确测试；8：engine 事务与事务测试；6 的 executor 调用接线、8 的 root_store 调用变更串行集成 |
| E | Task 9、Task 10，最多 2 个 | 9：索引描述符及 test_vector_index；10：故障测试、容器测试与 Dockerfile；Task 10 先用已有标量路径验证恢复，Task 9 合并后补模式回归 |
| F | Task 11，主代理统一验收 | 先运行报告校验小测试，再运行 L2；其他子代理不在验收期间改实现 |
| G | Task 12，暂不执行 | 功能基线冻结且用户批准性能阶段后，再按实际瓶颈拆分 |

批次 C 的 Task 7 可以先在 RootStore 层验证持久化，不依赖 Task 5 完整 SQL。批次 D 需等待 Task 5/7 及其接线完成；批次 E 需等待 Task 6/8 完成；Task 11 需等待全部实现合并。第三个空闲名额可用于已完成变更的只读评审，但不能与 3 个实现子代理叠加为第 4 个。

### Task 1：JSONL 接口与 C++ / Python 最小骨架

**Files:** 新建 `minivectordb/CMakeLists.txt`、两个 requirements 文件、Python 包入口/errors、`cpp/types.hpp`、`cpp/engine.hpp`、`cpp/engine.cpp`、`cpp/bindings.cpp`、`tests/test_protocol.py`、`tests/test_ir_boundary.py`；修改根 `Dockerfile` 为能构建扩展的骨架。

**Interfaces:** 提供第 3 节 Engine 接口；执行暂时抛 `not_implemented`，close 正常返回；Python 负责 ready、execute、shutdown。

**任务小测试（3 组）：** ready/shutdown/EOF 生命周期；id 类型与逐请求单响应/flush；连续失败 execute 不终止进程及 pybind11 数值/异常边界。使用 `test_protocol.py`、`test_ir_boundary.py` 的短会话，不要求业务 SQL 成功。

- [ ] 建立独立 venv，运行依赖固定 `sqlglot==30.31.0`、`pybind11==3.1.0`；dev requirements 引用运行 requirements 并固定 `pytest==9.2.0`。
- [ ] 先写交互测试：未发送请求即收到 ready；整数/字符串 id 保持类型；两个 execute 都返回合法错误且进程继续；shutdown 后无多余输出、退出 0；EOF 正常关闭。
- [ ] 运行测试确认因为入口/扩展不存在而失败，不接受 import 路径误配造成的假红。
- [ ] 实现单进程扩展与入口；默认数据目录 `/data`，本地允许 `--data-dir`；预期错误不打印到 stdout；Python 不缓存完整数据库。
- [ ] 固定 CMake 扩展输出目录为 `build/python`，核心测试接入 CTest；入口不得依赖测试目录中的模块才能运行。
- [ ] 构建后运行 3 组协议/绑定小测试；握手、关闭及合法失败响应通过，不要求业务 SQL 成功。原驱动检查留到集成验收。

构建/验证命令约定：

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r minivectordb/requirements-dev.txt
cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Debug \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
  -Dpybind11_DIR="$(.venv/bin/python -m pybind11 --cmakedir)"
cmake --build build -j2
export PYTHONPATH="$PWD/build/python:$PWD/minivectordb/python"
.venv/bin/python -m pytest minivectordb/tests/test_protocol.py minivectordb/tests/test_ir_boundary.py -q
```

**Expected:** 3 组任务小测试通过；业务 execute 明确失败且会话不失步。整套赛题暂时失败不阻塞本任务验收。

### Task 2：接入 SQLGlot，仅解析与降级 IR

**Files:** 新建 `python/minivectordb/frontend.py`、`dialect.py`、`tests/test_frontend.py`；修改入口，在调用核心前执行 parse_statement。

**Interfaces:** `parse_statement(sql: str) -> dict`；错误统一为 `DatabaseError(code, message)`；输出第 3.2 节 IR。

**任务小测试（4 组）：** 各语句族的代表 SQL 到 IR；BIGINT/负数/转义字符串保持词法值；CREATE VECTOR INDEX 得到结构化 IR；ALTER/JOIN/多语句等明确拒绝。输入自建，不把全部 95 条 SQL 参数化成任务门槛。

- [ ] 先用自建代表输入覆盖语句族和关键 IR 字段；正向 SQL 必须得到明确 op，ALTER 必须得到 `unsupported_sql`，不存在“非空 AST 就算成功”的断言。全部公开 SQL 的兼容性核对属于 L2，不复制为开发单测。
- [ ] 添加 BIGINT 正负边界、Unicode/转义字符串、字符串内部含分号、一个请求多条 SQL、未知函数、未支持子句的测试。
- [ ] 特别写失败测试：CREATE VECTOR INDEX 不允许退化为通用 Command 后被放行。
- [ ] 以 SQLGlot MySQL 方言的 parser 扩展或 token 驱动的窄入口识别 `CREATE VECTOR INDEX name ON table(column)`，标识符沿用 SQLGlot token 规则；不得对原 SQL 做不区分字符串/注释的 replace/正则截断。
- [ ] 普通 SQL 坚持使用 SQLGlot AST；解析降级为 Command 的其他语句一律拒绝。
- [ ] 将支持的 AST 逐项转换到受控 IR；保留列顺序、别名、ASC/DESC、LIMIT 和 AND 条件，不做执行。
- [ ] 运行解析与边界测试；核心仍可对已解析语句返回 `not_implemented`。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_frontend.py minivectordb/tests/test_ir_boundary.py -q
```

**Expected:** 4 组前端小测试与直接边界回归通过，所有代表语句得到受控 IR/错误。L2 时再核对公开 94 条支持语句与 1 条 ALTER；不要与原生 SQLGlot 的“94 个结构化 AST + 1 个 Command”探测数字混淆。

### Task 3：建立全量诊断循环，允许暂时全部 FAIL

**Files:** 新建 `minivectordb/tools/run_public.py`、`tests/test_public_diagnostics.py`；修改 `minivectordb/README.md`。保持 `protocol_check/` 与公开输入不变。

**Interfaces:** 诊断 CLI 接受原驱动同义的 `--command`、`--case`、`--output`，增加 `--keep-going`；复用原 `Session`、`strict_json`、`assert_response`，不复制另一套宽松比较器。

**任务小测试（3 组）：** 自建 2 个微型 case、合计 6 条 SQL 验证继续收集与非零退出；restart/mode 不占断言位置且目录正确复用；超时/失步后标记未执行、不伪造通过。微型 manifest/result 在 `tmp_path` 生成，不依赖赛题答案。

- [ ] 先写诊断测试：用自建 2-case/6-SQL 微型题包与 always-fail execute 骨架验证 `--keep-going`，处理 restart/mode 指令；不得用整套赛题作为唯一测试夹具。
- [ ] 对齐每条 SQL 与 result，下标只在 SQL 处递增；预期错误也调用原 assert_response。
- [ ] 普通断言失败时记录并继续；通信失步、崩溃、超时则停止当前会话，剩余语句标为未执行，不伪造结果；遵循原驱动首次超时停止后续用例的安全口径。
- [ ] 每用例独立空目录；restart 复用该目录；报告记录请求 SQL、模式、实际响应、预期断言、失败类别和行号。
- [ ] 将统计字段分成 `transport_ok`、`sql_passed`、`sql_failed`、`not_run`；SQL 未全通过时退出非零。不得用“链路成功”覆盖 `passed=false`。
- [ ] 验证微型 always-fail 基线的尝试数、失败数和非零退出；工具保留诊断完整 20-case/95-SQL 的能力，需要时手动运行，但不作为本任务必跑门槛。某些预期错误语句可能被当前宽松错误码配置判为通过，必须忠实报告，不推导出语义已实现。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_public_diagnostics.py -q
```

**Expected:** 3 组诊断工具小测试 PASS；其中测试夹具返回的非零执行码被正确断言，不能使 pytest 自身失败。

原驱动每个用例在第一处断言失败就退出该用例，诊断工具的继续执行语义由微型题包固定。95 条 SQL 的实际集成运行留到提交候选版本检查。

### Task 4：类型、表定义与流式文件基础

**Files:** 新建 `cpp/table_file.*`、`catalog.*`、`root_store.*` 的基础读写部分，补齐 `types.hpp`；新建 `tests/cpp/test_types.cpp`、`test_table_file.cpp`、`test_root_store.cpp`。

**Interfaces:** TableFile 以逐行迭代读取/写出有序 Row；Catalog 提供 schema 与不可变表对象引用；RootStore 持有数据路径、创建唯一对象，并装载根。提交发布的完整语义在 Task 7 验收。

**任务小测试（4 组）：** int64/Unicode/float32 编解码与约束；空表/多列流式往返；截断文件拒绝；Catalog 引用及对象路径边界。CTest 名称统一以 `task04_` 开头，绑定精度测试复用 Task 1。

- [ ] 写红测试：int64 精确往返；Unicode 字符数与 UTF-8 编码；向量 float32 往返；多列顺序；空表；文件截断拒绝。
- [ ] 实现 VARCHAR 长度检查、固定维度向量验证与严格数字解析；用明确列类型解释 SQL 字面量。
- [ ] 实现显式字节序的文件编码与扫描器；不得用 `read_all_rows()` 作为唯一读接口。
- [ ] 实现 schema 约束：恰好一个 BIGINT 主键、最多一个 VECTOR、列名唯一、长度有效；错误不产生可见表。
- [ ] 实现目录内唯一对象名和路径边界，SQL 表名不能形成路径穿越。
- [ ] 运行 4 组文件/类型小测试及绑定精度回归，确认 JSON BIGINT 不经过 float。

```bash
cmake --build build -j2
ctest --test-dir build -R '^task04_' --output-on-failure
.venv/bin/python -m pytest minivectordb/tests/test_ir_boundary.py -q
```

**Expected:** 4 组文件/类型小测试与绑定精度回归通过；不以正常重启或整套赛题通过作为本任务门槛。

### Task 5：基础 SQL 与候选根语句原子性

**Files:** 新建 `cpp/executor.*`、`tests/test_sql_semantics.py`；修改 `cpp/engine.cpp`、`types.hpp`、bindings；使用 Task 4 文件读写。

**Interfaces:** Engine.execute 接收标量语句，返回真实 QueryResult；所有修改经过 statement_candidate，不直接覆盖可见表文件。

**任务小测试（4 组）：** 短序列 CRUD/投影/affected_rows；比较/AND/排序并列/LIMIT；重复主键和失败候选不可见；Unicode 与 BIGINT 语义。每组只用 0–4 条记录，不读取 8 个赛题 case。

- [ ] 先写 4 组自建小测试，固定 CRUD、SELECT *、无 ORDER BY 多重集合、比较边界、普通排序并列与 LIMIT 0；不复制或参数化整套赛题。
- [ ] 实现 CREATE TABLE、INSERT、SELECT、UPDATE、DELETE；保留列名、顺序和正确 affected_rows。
- [ ] 绑定列引用并实现 `= < <= > >=` 与 AND；未知表/列、主键更新、不支持语法返回稳定错误码。
- [ ] INSERT 先检查请求内重复，再与原表流合并；UPDATE/DELETE 流式产生候选对象。发生语义错误时不发布 candidate。
- [ ] 普通排序使用显式比较器；有限 LIMIT 用堆，无界 ORDER BY 用固定内存预算的磁盘 runs 与归并，临时文件放数据目录。
- [ ] 测试 UPDATE 修改多列时必须以同一原行计算，不能让赋值顺序引起半更新；失败的候选文件不可被下个查询看见。
- [ ] 运行 4 组标量小测试，并回归直接依赖的类型/文件测试。8 个赛题 SQL 用例只作为集成检查参考。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_sql_semantics.py -q
ctest --test-dir build -R '^task04_' --output-on-failure
```

**Expected:** 4 组标量小测试 PASS，依赖回归通过；尚不宣称持久化和显式事务完成。

### Task 6：精确向量查询

**Files:** 新建 `cpp/exact_search.*`、`tests/test_exact_search.py`；修改 executor 与类型转换。

**Interfaces:** 同一 SELECT 执行路径支持 L2_DISTANCE 和投影别名；读取 Task 5 的相同可见表根。

**任务小测试（4 组）：** 欧氏距离及独立暴力真值；过滤后 TopK/并列/空集与不足 K；存储及查询 float32 转换；非法向量被拒绝且不留修改。数据限制在几条二维向量。

- [ ] 先写 `[3,4]` 到 `[0,0]` 输出 5 而非 25 的测试。
- [ ] 写 float32 测试：16777217 存储后与 16777216 相同，距离为 0；查询向量也经过相同转换。
- [ ] 写过滤反例：全局最近记录均不符合过滤时，仍从满足 WHERE 的集合返回 K 条；距离并列按主键，少于 K、空集合、K=0 都正确。
- [ ] 实现流式距离计算和 TopK；按第 5.4 节规则使用 double 运算，返回有限欧氏距离。
- [ ] 对 NaN、Infinity、float32 溢出、维度错、非法向量文本写负例；错误 INSERT/UPDATE 不产生可见修改。
- [ ] 与独立小规模 Python 暴力计算结果交叉检查，不用核心自己的距离函数同时生成 expected。
- [ ] 运行 4 组精确查询小测试及标量直接回归，不依赖赛题向量 case 判断本任务完成。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_exact_search.py minivectordb/tests/test_sql_semantics.py -q
```

**Expected:** 4 组向量小测试 PASS，标量回归无退化；索引描述符维护在 Task 9。

### Task 7：自动提交与正常重启持久化

**Files:** 完成 `cpp/root_store.*`，修改 catalog/engine；扩展 `tests/cpp/test_root_store.cpp`，新建 `tests/test_persistence.py`。

**Interfaces:** RootStore 将一个完整候选 Catalog 按第 5.3 节原子发布；Engine 构造加载已发布根，自动提交执行成功必须经过发布。

**任务小测试（3 组）：** 多表 INSERT/UPDATE/DELETE 后正常重开；已发布根及引用文件完整性；少量版本的不可达文件回收。RootStore 层 CTest 名称以 `task07_` 开头，SQL 重开测试放独立 `test_persistence.py`。

- [ ] 先写 INSERT/UPDATE/DELETE/多表重启测试；提交响应收到后立即结束进程，数据仍应存在。
- [ ] 实现对象、Catalog、CURRENT 的 fsync/rename 顺序；跨表仅有一个提交根。
- [ ] 启动区分空目录初始化与损坏的已发布目录；恢复完成前不发 ready。
- [ ] 添加文件回收测试：反复更新少量记录后不可达文件被删除，可达文件不被误删。测试记录数量与文件集合，不做压力测量。
- [ ] 测试编码、列序、向量存储值在重启后不变；关闭不是唯一持久化路径。
- [ ] 运行 3 组持久化小测试及基础文件回归；Task 7 可先在 RootStore 层完成，不等待完整 SQL case。

```bash
cmake --build build -j2
ctest --test-dir build -R '^task0[47]_' --output-on-failure
.venv/bin/python -m pytest minivectordb/tests/test_persistence.py -q
```

**Expected:** 3 组持久化小测试和依赖回归 PASS；正常重启不等于强杀恢复已验收。

### Task 8：显式事务、失败语句与关闭回滚

**Files:** 修改 engine/root_store/executor；新建 `tests/test_transactions.py`。

**Interfaces:** BEGIN 建立工作根；COMMIT 发布工作根；ROLLBACK 丢弃工作根；close 自动回滚。所有查询始终读取当前事务可见根。

**任务小测试（4 组）：** 事务内读自己修改及提交；标量/向量回滚；失败多行语句保留事务此前成功修改；shutdown/EOF 回滚与事务状态边界。每组使用短序列、最多几条记录。

- [ ] 写事务内读自己修改、COMMIT 后重启、ROLLBACK 后标量与向量查询都恢复的失败测试。
- [ ] 写“先 UPDATE 成功，再多行 INSERT 后行重复”的测试：INSERT 全部撤销，先前 UPDATE 可继续提交。
- [ ] 写自动提交多行 INSERT 后行维度错误的测试：前行不能留下，重启结果不变。
- [ ] 实现三个根的状态切换；语句失败不把显式事务整体标成不可继续。
- [ ] 实现 shutdown/EOF 的未提交回滚；清理时保护已提交根引用文件。
- [ ] 对嵌套 BEGIN、无活动事务的 COMMIT/ROLLBACK 写明确状态测试，不靠异常穿透终止进程；若题包未另定，采用 COMMIT/ROLLBACK 无事务时成功无操作，嵌套 BEGIN 拒绝。
- [ ] 运行 4 组事务小测试及持久化直接回归；赛题事务集合不作为每次修改必跑门槛。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_transactions.py minivectordb/tests/test_persistence.py -q
```

**Expected:** 4 组事务小测试 PASS，失败语句没有部分修改，先前成功语句保留。

### Task 9：向量索引声明、维护与 ann 精确回退

**Files:** 修改 catalog/executor/engine/exact_search 的索引接线（共享变更由主代理串行集成）；新建 `tests/test_vector_index.py`。

**Interfaces:** CREATE VECTOR INDEX 产生 Catalog 中 FlatScan 索引描述符；search_mode 在 bindings 与核心完整传递，Ann 初版显式调用精确后端。

**任务小测试（3 组）：** 索引定义校验；建索引后增删改与 exact/ann 一致；重开及事务回滚后描述符/可见向量一致。放 `test_vector_index.py`，避免与 Task 6 并发写同一测试文件。

- [ ] 先写建索引后 INSERT/UPDATE/DELETE、重启、exact/ann 模式切换的失败测试。
- [ ] 校验表存在、列为 VECTOR、索引名冲突和列引用；描述符与表 schema 一起持久化。
- [ ] 让两种模式读取同一可见根，不维护第二份会过期的向量数组。
- [ ] 增加事务向量更新后回滚、事务删除后回滚的两种模式对照；输出 ID 必须有效、无重复且满足过滤。
- [ ] 文档标明当前为 FlatScan，ann 精确回退，没有构建 HNSW 或其他近似结构。
- [ ] 运行 3 组索引小测试及精确查询/事务直接回归；全量赛题检查由 Task 11 统一执行。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_vector_index.py \
  minivectordb/tests/test_exact_search.py minivectordb/tests/test_transactions.py -q
```

**Expected:** 3 组索引小测试 PASS，直接回归无退化；Task 1–9 的小测试全部通过才达到本地功能里程碑，尚未取得赛题集成验收结论。

### Task 10：强杀恢复与真实容器验收

**Files:** 新建 `tests/test_recovery.py`、`tests/test_container.py`，补充 `tests/cpp/test_root_store.cpp` 的故障测试；完成根 `Dockerfile`，实现 README 由主代理更新。

**Interfaces:** 保持外部协议不变；故障注入只属于测试构建/测试夹具，生产运行不依赖它。

**任务小测试（4 组）：** 已确认提交/未提交的 SIGKILL；发布关键边界的旧或新完整状态；恢复清理重入与发布故障后停止服务；真实镜像 ready/短 CRUD/shutdown/只读根目录。故障点只做小规模参数化，不跑压力负载。

- [ ] 加入确定性恢复测试：已确认自动提交/COMMIT 后 SIGKILL，已提交修改保留；未发 COMMIT 的事务 SIGKILL 后完全回滚。
- [ ] 在新对象完成、Catalog 完成、CURRENT 切换前后设置测试构建故障点，分别终止；未确认提交可旧可新，但不能混合多表状态。
- [ ] 在恢复清理不可达文件时再强杀，第三次启动仍恢复相同已提交根；不能仅做没有实际恢复工作的两次普通重启。
- [ ] 测试 publish I/O 故障后不会继续用旧内存根响应后续请求；CURRENT 引用文件缺失时不能报告 ready。
- [ ] Docker 使用构建阶段编译扩展、运行阶段包含 Python 与锁定依赖；禁止运行时联网安装。
- [ ] Python 关闭字节码写入，日志写 stderr，临时数据写 `/data`，容器只读根文件系统下仍能运行。
- [ ] 默认入口直接是 JSONL 模式，使用 `-i`，不使用 TTY、不暴露端口；不依赖仓库测试文件随镜像提供。
- [ ] 在真实 Docker 镜像上用自建短交互验证 ready/CRUD/shutdown 与只读根目录；本机若只有 Podman，镜像小测试标为 BLOCKED，不把本地进程测试替代 Docker。赛题全量验收留到 Task 11。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_recovery.py -q
cmake --build build -j2
ctest --test-dir build -R '^task07_' --output-on-failure
docker build -t minivectordb:round2 .
VECTORDB_IMAGE=minivectordb:round2 .venv/bin/python -m pytest minivectordb/tests/test_container.py -q
```

**Expected:** 4 组故障/容器小测试通过，关闭无额外 stdout。当前主机发现 Podman、未发现 Docker；缺少环境时本任务不能标记完整完成，须如实报告故障测试结果和容器 BLOCKED。

### Task 11：冻结功能基线与交付记录

**Files:** 新建 `minivectordb/tools/check_delivery.py`、`tests/test_delivery.py`，完成 `minivectordb/README.md`；生成新的验证记录 `docs/superpowers/experiments/<执行日期>-minivectordb-public-baseline.md`。涉及学习材料时先核对实际读取路径，不覆盖根目录现有空文件来假造完成。

**任务小测试（2 组）：** 完整有效验收报告被接受；含 FAIL/SKIPPED/BLOCKED、缺少或重复 case、passed 与条目状态不一致的报告被拒绝。输入使用自建报告，不伪造实际赛题运行结果。构建版本和环境能力由主代理核对实际运行记录，不假设原驱动报告含有这些字段。之后独立执行 L2。

- [ ] 先实现 `check_delivery.validate_delivery_report(report: dict, expected_ids: list[str]) -> None`，以两组自建报告固定“只接受 passed=true、ID 完整唯一且全 PASS”的行为；校验器只能接受或拒绝，不能改写失败结果。CLI 接受 `--report` 与 `--manifest`，拒绝时退出非零。
- [ ] 运行 2 组交付校验小测试后，再从新空目录重跑 CTest、pytest 及第 7.1 节原驱动本地进程/实际 Docker 检查，保留命令、版本、退出码和报告路径。
- [ ] 逐一对照 20 个 manifest ID，确认无 SKIPPED、无仅修改断言得到的通过；校验公开 test/result 文件未被实现改动。
- [ ] 记录精确扫描、整表重写、结果响应大小、ANN 精确回退等限制；将“公开功能完成”与“正式功能准入”分栏。
- [ ] 将强杀恢复证据与正常重启证据分开，不使用平台日志替代没有执行过的实验。
- [ ] 如整理 conversation.json，仅纳入真实用户贡献与实际验证结果，合法 UTF-8 JSON、不超过 128 KiB、脱敏；路径按正式入口配置确定，本任务不编造对话或实验。
- [ ] 提交分支、源码、Dockerfile 与材料版本的锁定由正式提测阶段处理，本轮不发起提测。

```bash
.venv/bin/python -m pytest minivectordb/tests/test_delivery.py -q
```

**Expected:** 2 组报告校验小测试 PASS；随后第 7.1 节 L2 检查也真实通过，才可冻结为可提交的公开功能基线。不报告尚未执行的性能成绩。

### Task 12：性能优化阶段，当前不执行

**Files:** 在功能基线冻结且优化阶段获批后另建优化计划，并创建 `minivectordb/tests/test_optimization_contract.py`；本轮不创建这些测试或实现基准框架/索引算法。

**任务小测试（3 组，性能阶段获批后才创建）：** 优化路径与精确基线在小数据上等价；过滤/并列/事务可见性不变；提交持久性不退化。使用 `test_optimization_contract.py`；不以压测、速度阈值或排名作为正确性小测试。

- [ ] 先批准性能阶段范围，再测解析、绑定、扫描、整表重写、同步、序列化的占比。
- [ ] 只有测量确认瓶颈后，选择减少重复解析、批量写入开销、增量存储或向量计算优化中的一项。
- [ ] 若需要 ANN，再比较自实现 IVF/HNSW 与精确扫描，检查过滤召回率、动态更新与事务一致性。
- [ ] 每次优化先回归 3 组优化正确性护栏与受影响任务的小测试；形成新的提交候选版本时再统一运行原 20 个赛题用例。不得通过省略同步、延后确认或放宽精确语义换性能。

性能阶段获批后的小测试命令：

```bash
.venv/bin/python -m pytest minivectordb/tests/test_optimization_contract.py -q
```

**Expected:** 当前仅保留任务和 3 组测试设计，不创建或运行优化测试、不执行性能改动或压测；获批实施后必须先固定正确性再测性能。

## 7. 里程碑与完成判据

| 里程碑 | 任务 | 必须看到的证据 | 不代表什么 |
| --- | --- | --- | --- |
| M0 测试链路 | 1–3 | 协议、解析、微型诊断小测试通过，业务 SQL 允许失败 | 赛题 95 条 SQL 均已执行或已通过 |
| M1 查询基线 | 4–6 | 文件/类型、标量与精确向量小测试及直接回归通过 | 持久化、显式事务完成 |
| M2 本地功能 | 7–9 | 持久化、事务、索引小测试通过；Task 1–9 小测试全绿 | 已取得赛题集成或正式准入结论 |
| M3 可复现交付 | 10–11 | 恢复/容器/报告小测试通过，再由原驱动本地与实际 Docker 20/20 PASS | 隐藏用例、性能评分或压力测试完成 |
| M4 优化 | 12 | 经确认的单独优化计划及之后的测量 | 当前已授权执行 |

本轮实施计划的优先完成目标是 **M2 的任务级功能稳定，随后达到 M3 的恢复、容器与赛题提交验收**。性能优化排在最后。任何阶段不能因为返回了格式正确的 false、没有崩溃或产生了报告，就把失败断言标成通过。

### 7.1 统一赛题集成/提交前检查（Task 11 执行）

以下检查针对已合并、冻结的提交候选版本，不塞进每个子任务。先跑全体开发小测试，再运行题包原驱动；不修改 test/result 或资源配置。

```bash
export PYTHONPATH="$PWD/build/python:$PWD/minivectordb/python"
cmake --build build -j2
ctest --test-dir build --output-on-failure
VECTORDB_IMAGE=minivectordb:round2 .venv/bin/python -m pytest minivectordb/tests -q
out=$(mktemp -d /tmp/minivectordb-submission-check-XXXXXX)
.venv/bin/python protocol_check/run.py \
  --command "[\"$PWD/.venv/bin/python\",\"-m\",\"minivectordb\",\"--data-dir\",\"{data_dir}\"]" \
  --output "$out/local"
python3 protocol_check/run.py --image minivectordb:round2 \
  --handshake-only --output "$out/handshake"
python3 protocol_check/run.py --image minivectordb:round2 --output "$out/container"
.venv/bin/python minivectordb/tools/check_delivery.py \
  --report "$out/local/result.json" --manifest mysqltest/manifest.json
.venv/bin/python minivectordb/tools/check_delivery.py \
  --report "$out/container/result.json" --manifest mysqltest/manifest.json
```

预期：本地和实际 Docker 两份报告都为 `passed=true`、20 个 case 全 PASS、无 SKIPPED，所有命令退出 0。报告校验小测试和校验脚本不替代真实运行。检查期间实现不可变；修复任何集成失败后重跑受影响 L1，并对新的候选版本重新完成 L2。

## 8. 执行前确认与风险

1. **同进程边界：** 用户已确认 Python + pybind11，而不是 Python 解析子进程。
2. **存储基线：** 接受初版整表流式重写的 I/O 代价，换取简单的事务与恢复；如果不同意，应在编码前替换设计，而不是半途中混用 snapshot 与 WAL。
3. **正式语义：** 当前错误码配置为空，但实现仍保留语义码；未来映射冻结时只调整协议映射，不重写核心语义。
4. **未冻结细节：** 本计划不扩展完整 MySQL；无事务 COMMIT/ROLLBACK 等未由公开样例约束的行为已给出默认决定，正式题包另定时服从题包并补回归。
5. **公开样例边界：** 正式说明另有 9 个保留用例及超过内存的数据场景。当前只以 20 个公开样例作为阶段目标，不省略其背后的类型、原子性和恢复规则；大数据正式资源准入需另行验证。
6. **材料边界：** prompt 分不通过堆 AI 对话自动获得；本计划只提醒保留真实过程，不把材料生成当作数据库功能。
7. **验证环境：** 当前可运行本地 Python 诊断，未检测到 Docker；实际容器验收必须使用真实运行环境，不能在文档中提前勾选。

## 9. 参考资料

- 本仓库协议与测试是第一依据：`STDIO_PROTOCOL.md`、`mysqltest/README.md`、`protocol_check/README.md`。
- SQLGlot：<https://github.com/tobymao/sqlglot>，仅 parser/AST；本计划固定版本并对全部公开 SQL 实测。
- pybind11：<https://github.com/pybind/pybind11>，同进程 C++/Python 连接。
- BusTub：<https://github.com/cmu-db/bustub>，参考执行与存储层边界，不搬用课程答案。
- Mini-LSM：<https://skyzh.github.io/mini-lsm/>，参考持久化与恢复原理；本轮不实现完整 LSM。

## 10. 首批红→绿测试样例

以下是实施时写入指定测试文件的最小测试内容；不是本轮已经创建或运行的测试。所有测试共享第 3 节接口，不调用未约定的引擎私有方法。

### `minivectordb/tests/test_frontend.py`

```python
import pytest
from minivectordb.frontend import parse_statement
from minivectordb.errors import DatabaseError


def test_vector_index_has_structured_ir():
    ir = parse_statement("CREATE VECTOR INDEX docs_vec ON docs(embedding);")
    assert ir == {
        "op": "create_vector_index", "index": "docs_vec",
        "table": "docs", "column": "embedding",
    }


def test_bigint_literal_never_passes_through_float():
    ir = parse_statement("INSERT INTO docs VALUES (9007199254740993);")
    assert ir["rows"][0][0] == {
        "kind": "integer", "text": "9007199254740993",
    }


def test_alter_is_explicitly_unsupported():
    with pytest.raises(DatabaseError) as exc:
        parse_statement("ALTER TABLE docs ADD extra BIGINT;")
    assert exc.value.code == "unsupported_sql"
```

### `minivectordb/tests/test_exact_search.py`

```python
from _minivectordb import Engine
from minivectordb.frontend import parse_statement


def test_filter_before_topk_and_primary_key_tie(tmp_path):
    engine = Engine(str(tmp_path))
    try:
        engine.execute(parse_statement(
            "CREATE TABLE docs (id BIGINT PRIMARY KEY, "
            "category VARCHAR(8), embedding VECTOR(2));"))
        engine.execute(parse_statement(
            "INSERT INTO docs VALUES (8,'keep','[1,0]'),"
            "(3,'keep','[-1,0]'),(1,'skip','[0,0]');"))
        result = engine.execute(parse_statement(
            "SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance "
            "FROM docs WHERE category = 'keep' ORDER BY distance LIMIT 1;"))
        assert result == {"columns": ["id", "distance"], "rows": [[3, 1.0]]}
    finally:
        engine.close()
```

### `minivectordb/tests/test_transactions.py`

```python
import pytest
from _minivectordb import Engine
from minivectordb.frontend import parse_statement
from minivectordb.errors import DatabaseError


def test_failed_statement_keeps_previous_transaction_work(tmp_path):
    engine = Engine(str(tmp_path))
    try:
        for sql in (
            "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));",
            "INSERT INTO docs VALUES (1,'old');",
            "BEGIN;",
            "UPDATE docs SET content = 'kept' WHERE id = 1;",
        ):
            engine.execute(parse_statement(sql))
        with pytest.raises(DatabaseError) as exc:
            engine.execute(parse_statement(
                "INSERT INTO docs VALUES (2,'new'),(1,'duplicate');"))
        assert exc.value.code == "duplicate_key"
        assert engine.execute(parse_statement(
            "SELECT id, content FROM docs ORDER BY id;"))["rows"] == [[1, "kept"]]
        engine.execute(parse_statement("COMMIT;"))
    finally:
        engine.close()
    reopened = Engine(str(tmp_path))
    try:
        assert reopened.execute(parse_statement(
            "SELECT id, content FROM docs ORDER BY id;"))["rows"] == [[1, "kept"]]
    finally:
        reopened.close()
```

这些样例不替代各任务列出的其余边界与故障测试。执行者必须先看到对应测试因缺少行为而失败，再实现到通过；不能仅粘贴 expected 返回值。

## 11. 文档自检

- [x] 已区分 MySQL 风格 SQL、JSONL 协议与 MySQL 网络协议。
- [x] 已核对本地 manifest、20 对 test/result 与 95 条 SQL。
- [x] 已验证 SQLGlot 对 CREATE VECTOR INDEX 的 Command 退化并安排显式处理。
- [x] 已区分“执行链路跑通”“断言通过”“正式准入通过”。
- [x] 已定义 Python/C++ 的职责、接口与单进程约束。
- [x] 已选择单一持久化基线，说明提交点、回滚、清理和恢复重入。
- [x] 已覆盖公开测试中的 BIGINT、float32、过滤并列、正常重启与失败语句原子性。
- [x] 已将压测和性能优化留到功能基线之后。
- [x] 已为 12 个任务逐项定义少量独立小测试，赛题 cases 单列为提交候选版本的集成验收。
- [x] 已确认 pybind11，并写入最多 3 个 subagent、隔离写入、依赖批次及主代理统一验收规则。
- [x] 本轮没有改动数据库实现、公开测试和既有用户修改。
