# 已通过正确性基准 b410e23 与我们 c4a54eb 的差分研究

日期：2026-11-12。

## 0. 比较前提与结论

按用户确认，将 `origin/correct-test` 的 **b410e23ac01bcbf1df342faa6030034ab1d05e6c 已通过正式正确性**作为已知事实，不再用提交数、分支名或公开自测推测其是否通过。

本轮对象固定为：

| 对象 | 提交 |
|---|---|
| 已通过基准，Go 自写实现 | b410e23ac01bcbf1df342faa6030034ab1d05e6c |
| 我们的 Python + C++ 实现 | c4a54ebef06bf7ab5686b2c4866b53c658782b62 |

参考源码：https://github.com/Dline666/seekdb-2026/tree/b410e23ac01bcbf1df342faa6030034ab1d05e6c/minivectordb 。用户后来收束了任务范围，其他分支不再作为功能判断基准。

**主要结论：不是发现了另一套必须改用的通信协议，而是找到了我们真实存在的 SQL 语义/兼容缺口，以及可观测的写入成本差距。**

1. `ORDER BY` 序号被我们当作常量，返回成功但顺序错误；无效序号也错误成功。这违反题面“不支持的语句不能静默执行为其他含义”，不只是没有实现额外语法。
2. 列名/别名大小写、表级主键、限定列名等常规写法，基准支持而我们拒绝；大小写重复列定义则相反，基准拒绝、我们接受。
3. 向量转换对下溢和数字词法的处理不同；我们此前对“非法向量”的部分分类过窄，须撤回相应合规结论。
4. JSONL 结构相同，但不带 AS 的 L2_DISTANCE 投影有实际列名元数据差异；原驱动精确比较 columns，不能只检查 rows。
5. 在同一约 5 MB 数据集上，64 次单行 UPDATE，基准设备写入约 296 KiB，我们约 298 MiB，相差约 **1,031 倍**。两边都返回正确结果并通过本轮强杀恢复；这是成本差异，不是已经复现的正式超时，但与资源型功能准入直接相关。

未取得正式失败 SQL，因此不把某一个差异冒称为平台 0 分的唯一原因。现在已有可复现反例，应从这些反例推进，不再只以公开 20/20 结束调查。

## 1. 版本与证据可信度

证据根目录：

```text
.superpowers/sdd/teammate-comparison/run-qgnKSUGu/
```

源代码临时副本：

```text
/tmp/minivectordb-teammate-C7UZAjMZ/correct-b410e23/
/tmp/minivectordb-teammate-C7UZAjMZ/ours/
```

工作区曾由用户切换到 correct-test 再切回。分析全程不替用户切换分支，不覆盖当前源码，使用上述固定副本。

最初构建的参考镜像来自 846d98e，后来用户明确了 b410e23 基准。已验证两个提交下构建输入的 Git 对象完全相同：

```text
Dockerfile blob:  e1250a00224d37cd4691bb194f77ebe227baf256
minivectordb tree: 2d2499abfcb42f0be0fd4f26a2bab42409d8d2db
```

且 `git diff b410e23 846d98e -- Dockerfile minivectordb STDIO_PROTOCOL.md protocol_check mysqltest` 为空。因此本轮镜像是 b410e23 相同构建源码的本地构建，不是以之后 perf 实现代替基准；不声称它就是平台锁定的那个镜像。

```text
参考镜像：sha256:2b87b723ea767130c8967a1c477be9f6c4f361baffdd241ccd88108fbb6b45f3
我们的镜像：sha256:325ba39630db368a05046eb9ae2fecafb6c9a6f76834dac00765b7746db44998
```

参考版本的 STDIO_PROTOCOL.md、protocol_check、mysqltest 与官方 f4d4e26 完全相同。所有验收 SQL 均经实际 Docker stdin/stdout；只用 docker exec 读取 cgroup、文件大小等诊断信息，不通过它执行 SQL。没有改题包、配置、断言或默认入口。

### 实测范围

- 初期三个参考分支均从源码构建，并经原始驱动公开 20/20；主结论仅用与 b410e23 源码相同的参考镜像。
- 53 项小型差分探针，41 项出现我们与基准不同的预期结果状态。
- 另做 7 条投影/尾注释等查询及一条多步 DDL/事务观察序列。
- 8,000 行 × 128 维、64 次单行 UPDATE 的有界存储诊断，两边使用同一数据、公开 2 CPU/2 GiB 限额、只读根、禁网、独立卷，最后实际强杀重启。

53 项是探索性探针，**不是正式 53 个用例**。其中 alias-case-ambiguity 的预期报错是我们的先验假设，两边均返回成功，缺乏冻结规范支持，排除为“未定且同表现”，不据此指称基准错误。原始记录保留，不修改断言制造全绿。

## 2. 确定的静默语义问题：ORDER BY 序号

本轮真实输入的基础数据为：

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(16),v VECTOR(2));
INSERT INTO t VALUES
 (1,20,'a','[3,4]'),(2,10,'B','[0,2]'),(3,20,'c','[1,0]');
SELECT n,id FROM t ORDER BY 1,id;
```

| 实现 | rows |
|---|---|
| b410e23 | [[10,2],[20,1],[20,3]] |
| c4a54eb | [[20,1],[10,2],[20,3]] |

我们返回 ok:true，但排序实际退化成按 PK 排；`ORDER BY 1 DESC`、`ORDER BY 2 DESC` 也有相同问题。

此外：

```sql
SELECT id FROM t ORDER BY 0;
SELECT id FROM t ORDER BY 2;
```

基准报错，我们均返回成功。后者 SELECT 只有一个输出列，序号 2 越界。

### 源码原因

- 基准 `internal/sqlparse/dml.go:153–159` 将 ORDER BY 的裸整数解析为独立 Ordinal，而非普通 IntLit。
- `internal/engine/select.go:139–175` 对 1-based 序号检查展开后的输出列数，再绑定到对应表达式。
- 我们 `frontend.py:168–175` 直接调用 expression，整数进入普通 literal IR。
- 我们 `cpp/executor.cpp:283–296` 仅特殊解析名称/别名，其余进入 bind_expression；整数成为每行相同的常量，最后按 PK 打破并列。

无论完整冻结语法是否要求序号排序，我们都不能把它接受后静默改成常量排序。**至少应明确拒绝；对照已通过基准，正确实现序号绑定是更直接的对齐方向。** 不应把所有整数表达式全局改写成序号，投影中的整数常量仍是常量。

## 3. 名称绑定不是只把 SQL 关键字转大写

以下行为已实际对照：

| 输入 | b410e23 | c4a54eb |
|---|---|---|
| SELECT ID AS out FROM t | 成功，取 id 列 | unknown_column |
| SELECT `ID` AS out FROM t | 成功 | unknown_column |
| SELECT n AS Score,id FROM t ORDER BY SCORE,id | 成功 | unknown_column |
| INSERT 列表使用 ID,N | 成功 | unknown_column |
| UPDATE 的 SET/WHERE 使用 N/ID | 成功 | unknown_column |
| DELETE 条件使用 ID | 成功 | unknown_column |
| CREATE VECTOR INDEX 使用列 V，定义为 v | 成功 | unknown_column |
| 同时声明 N BIGINT,n BIGINT | duplicate_column | 错误接受 |

源码：

- 基准 `internal/engine/table.go:81–87` 用 strings.EqualFold 查列。
- `internal/engine/exec.go:145–162` 用小写键判断重复列；表名也采用小写键。
- `internal/engine/select.go:151–157` 不区分大小写解析显式 AS 别名。
- 我们 `frontend.py:18–22` 保留原名，核心直接进行区分大小写的名称比较；定义合法性和查找均未建立匹配的规范化规则。

列名/别名是常规 SQL 语义的重要缺口；**表名大小写单独列为未冻结的兼容策略**，不能直接用 Linux MySQL 或某个实现的默认行为替代题意。

后续修复需保留显示名称、规范化查找与唯一性判断，不能简单把所有输出列名也转小写，否则会引入 columns 精确比较的新问题。

## 4. 白名单限制比“最小 SQL 子集”本身更窄

### 4.1 表级主键

```sql
CREATE TABLE tablepk (id BIGINT,n BIGINT,PRIMARY KEY(id));
```

基准成功，我们返回 `unsupported SQL node: PrimaryKey`。题面要求一个 BIGINT 主键，没有明确限制只能使用列内 PRIMARY KEY。

- 基准 `internal/sqlparse/ddl.go:113–204` 支持表级单列主键、解析列名并绑定；复合主键仍拒绝。
- 我们 `frontend.py:135–137` 把所有 Schema 子表达式都交给 column_schema；该函数只接受 ColumnDef，而非表级 PrimaryKey。

这不需要恢复 NULL、外键或任意表约束扩展。

### 4.2 限定列名与单表别名

```sql
SELECT t.id AS out FROM t ORDER BY t.id;
SELECT a.id AS out FROM t AS a ORDER BY a.id;
```

基准成功，我们拒绝 `Column clause: table`。

- 基准 `internal/engine/expr.go:31–46` 在绑定列之前验证限定名属于当前表或别名，再解析列。
- 我们 `frontend.py:25–27,45–47` 的 Table/Column 白名单只允许 this，拒绝 alias/table 字段。

这些语句没有 JOIN、多表或子查询。应区分“验证过的单表限定名”和“无条件丢弃限定名”，后者会静默接受错误引用，不能用来修补。

### 4.3 确实多做了，但不能一概定为必需的语法

基准还实际支持而我们不支持：

- SELECT 1，无 FROM 的常量查询。
- NOT NULL 声明（不是 SQL NULL 值支持）。
- BIGINT(20)、IF NOT EXISTS。
- LIMIT OFFSET、LIMIT offset,count。
- BEGIN/COMMIT/ROLLBACK WORK。
- SET autocommit=1、SET search_mode=ann。
- 浮点常量投影和普通数值比较。
- L2_DISTANCE 参数对调、VEC_FROM_TEXT 构造器。
- != / <> 等更宽的比较语法。

赛题公开能力表只列 `=、<、<=、>、>=`，所以本轮探针中的 neq-control 不应被名字误导为必需控制项。SELECT 1 可能用于连接式探测，但当前公开驱动没有这个隐式请求；不能声称正式一定发送了它。

这些内容说明基准解析器在其支持范围内更包容，**不意味着应立即把它所有扩展复制过来**。尤其不能因此重启 NULL/JOIN/聚合扩展。

## 5. 向量：拒绝溢出不等于拒绝下溢到零

已实际验证的基准接受、我们拒绝的输入：

```text
[+1,0]   [01,0]   [.5,0]   [1.,0]
[1e-46,0] [1e-50,0] [1e-320,0] [1e-999,0]
```

基准成功后的向量也已读取，对数值量化为 float32 后比较，后四种均转换为有限零；不是只看 affected_rows。

源码：

- 基准 `internal/types/value.go:38–132` 自行验证十进制数字语法，ParseFloat 后转 float32；拒绝非有限值和转为 Inf 的溢出，但允许下溢为零。
- 我们 `cpp/types.cpp:90–157` 采用更接近 JSON number 的词法，排除前导 +、前导零、省略整数/小数部分等；直接 from_chars<float>，将转换的 out-of-range 统一判错，未区分下溢到零。

题面明确拒绝 NaN、Infinity、维度不符和转换溢出；并未把上述全部词法变体明文列为非法，也没有要求 SQL 字符串里的向量数字必须采用 JSON number 词法。

**纠正此前验证结论：** 105 项拒绝检查包含 `[+1,0]`、`[01,0]` 两种格式 × 5 路径，即 10 项没有充分依据被归为正式“非法向量”。保留它们“我们确实拒绝”的原始事实，撤回“拒绝即合规”的解释。其他 NaN/Inf、维度错误等独立检查不受此分类纠正影响。

后续应分别处理词法、量化下溢、真正溢出和非有限数；不能为兼容这些数值而放开 NaN/Infinity 或维度错误。也不应无条件复制基准的所有数值转换细节而放弃独立 float32 真值。

## 6. 输入输出格式：JSON 外壳相同，列名元数据存在差异

### 相同点

基准 `cmd/minivectordb/main.go`：恢复后准确 ready、逐行 JSON、逐次 flush、ID 原样、业务错误响应后继续、shutdown 关闭后 ACK 并退出。与我们的协议设计相同。

基准未使用 MySQL TCP，也没有默认纯 SQL/TSV 协议。不能据此要求我们改成裸 SQL 输入或文本表格。

### 错误返回

Go 的 errors.go 将 MySQL 错误号/SQLSTATE 放进 message，例如：

```text
code=duplicate_key
message=ERROR 1062 (23000): Duplicate entry ...
```

非法向量则区分 invalid_vector、dimension_mismatch 等。我们使用 type_error、unknown_column 等其他非空语义码。

当前相同的公开 config 中 error_codes 为空；STDIO 明确 message 措辞不参与判题。故这些名称/前缀差异**不能单独认定是失败原因**。

真正已经复现的问题是：例如 SELECT ID 本该成功，我们却返回一个格式完全合法的错误。**错误 JSON 格式正确，不等于这条 SQL 应该报错。**

### 默认函数列名

下例两边 rows 都是 [[5],[2]]，但 columns 不同：

```sql
SELECT L2_DISTANCE(v,'[0,0]') FROM t ORDER BY id;
```

| 实现 | columns[0] |
|---|---|
| 基准 | L2_DISTANCE(v,'[0,0]') |
| 我们 | L2_DISTANCE(v, '[0,0]') |

小写 l2_distance 的输入也被我们统一改成大写列名。带显式 AS distance 时两边相同；输入本就含逗号后空格且函数大写时也相同。

- 基准 `internal/sqlparse/dml.go:85–99` 保存原始表达式文本；`internal/engine/select.go:50–57` 无别名时使用它。
- 我们 `cpp/executor.cpp:269–277` 重新拼接默认函数名称，固定大小写和逗号后的空格。

原驱动精确比较 columns，因此这是能直接影响结果比较的已复现差异；不过默认函数列名的完整冻结规则未另行取得，不能伪称知道正式隐藏断言采用哪种格式。

## 7. Dockerfile：运行方式一致，依赖面不同

| 项目 | 基准 | 我们 |
|---|---|---|
| 基镜像 | ubuntu:24.04，多阶段 | python:3.12.10-slim，多阶段 |
| 构建工具 | Ubuntu Go 1.22，CGO_ENABLED=0 | CMake/g++/make + Python venv |
| 第三方获取 | Go 标准库，GOPROXY=off | 构建阶段下载 SQLGlot、pybind11、pytest |
| 默认入口 | /app/minivectordb | /opt/venv/bin/python -m minivectordb |
| /data | 显式 VOLUME 与 CMD 参数 | 程序默认目录，显式创建；驱动照常挂载 |
| 前台/网络服务 | 前台 stdin/stdout，无网络监听 | 相同 |

基准的依赖面更少，增加了 apt 镜像回退，构建时不运行单测；历史 00811df 明确完成过这次改动。但没有平台构建日志表明它解决了哪一个具体失败。

赛题 §4.2.1 的 Ubuntu 是示例；§2.1 明确允许 SQL 解析库。**不能从参考实现使用 Ubuntu/自写解析器，推出 Python 基镜像或 SQLGlot 被禁止。** 两边的 apt/基镜像标签也都没有达到完全内容寻址的依赖锁定。

本轮三个参考镜像实际构建成功，基准本地构建经同一原始驱动公开 20/20。我们此前也有干净远端提交构建及实际 Docker 20/20，故目前不应先把入口、VOLUME 声明或语言整体推翻。平台构建的依赖可达性仍是环境变量，不由本地成功自动排除。

## 8. 存储成本：不是“有无 WAL”标签，而是实际写了多少

### 架构对照

基准：

- 一个追加日志同时充当行数据和 redo；已提交 PK → 行偏移映射在内存。
- 整行通过 mmap 按需读，不把全部数据复制到常驻堆。
- 语句先验证全部 change，再写变更行；显式事务使用 overlay。
- 追加 COMMIT、fdatasync，然后更新已提交映射和索引，再返回。
- 恢复只应用有 COMMIT 的变更，丢弃尾部未提交数据，ready 前重建派生索引。

对应源码：`internal/engine/txn.go:26–32,87–149`、`db.go:87–156`、`internal/storage/log.go:120–163`。

我们：

- INSERT 合并并重写目标表；UPDATE/DELETE 即使改一行也流式重写整张表。
- RootStore.publish 再扫描/同步候选根里的所有表，写新目录根，原子替换 CURRENT。
- 能保持语句/事务原子和低常驻数据量，但持续小批写成本随已有表大小增长。

对应源码：`cpp/executor.cpp:343–385,388–420`、`cpp/root_store.cpp:213–248`。这也影响显式事务中的每条 DML，不只是最终 COMMIT。

### 本轮小型实测

同一 8,000 行、128 维、64 字符 VARCHAR，批量加载后建索引，再做 64 次单行普通列 UPDATE。读取 cgroup 设备写入计数，之后校验数据、exact/ann filtered TopK，强杀并重启验证提交保留。

| 指标 | 基准 | 我们 |
|---|---:|---:|
| 加载阶段设备写入 | 4,952,064 B | 22,028,288 B |
| 64 次 UPDATE 设备写入 | **303,104 B** | **312,475,648 B** |
| 更新阶段比值 | 1 | **约 1,031** |
| 重启后的数据文件大小 | 4,952,788 B | 4,872,325 B |
| 数据与故障后结果 | PASS | PASS |

后两项说明不能把大 I/O 误写成“历史文件永久泄漏”：最终文件大小相近，差异是反复写入已有数据。

每个实现自设 512 MiB 写入和 120 秒诊断预算，均未超限；所有 SQL 走原 transport。跨重启后 cgroup 计数重新开始，汇总时已将启动前后两个窗口相加，没有把重置后的数值当累计值。

计时包含 docker exec 采样开销，不将它生成正式 T/B/M 或性能分。该实验不是正式资源型输入，**没有在这里复现正式超时**；但证明我们不能以“2,112 MiB 大字符串、33 个大批次通过”概括所有超内存/持续写入形状。

若数据达到 D、按固定小批追加 m 次，完整重写的累计量约为 D×m/2；已有大表上的 u 次单行更新约为 D×u，另加发布扫描。这会直接侵占功能用例的超时和 I/O 预算，不能只当排名优化。

## 9. ANN、恢复与 DDL：不要盲目模仿

基准并非在所有 ann 查询上使用近似算法：

- `topk.go:113–159` 限制在足够大、显式训练索引、无过滤、无待处理事务修改等条件下才用 IVF。
- 小数据、有过滤、写事务下走精确路径；内存预算不够时回退日志扫描。
- `vecindex.go:47–84` 检查缓存预算；入口把默认缓存限制在 256 MiB 或 cgroup 的 1/4 以内，并限制 Go 堆/GC。

这证明正确性优先、exact fallback 是可行路线，并不支持“必须立刻加 HNSW 才能过”的判断。我们已有精确扫描、过滤、PK tie 和事务可见性的实际证据，本轮没有在这些基本规则上发现新的反例。

基准有逐字节日志截断恢复测试 TestCrashAtEveryOffset，以及提交/回滚、过滤先于 TopK、压缩等自有测试。本轮阅读了这些测试，没有冒称运行了 Go 全套单测。

**DDL 边界必须单独对待：** 基准 `db.go:181–197` 在 DDL 前隐式提交显式事务。实际观察 BEGIN → INSERT → CREATE TABLE → ROLLBACK，基准保留 INSERT，我们回滚。题面没有明确冻结 DDL 事务边界，且 §3.2 明确不能直接用 MySQL 行为代替本题语义，因此只记录差异，不自动把我们的事务 DDL 改成隐式提交。

此前 ANN 探针中的“事务内建索引后回滚索引定义”应理解为我们实现额外状态的测试，不是正式要求已明文规定必须回滚 DDL 的证明。

## 10. 建议的推进顺序

1. **先处理静默错误：** 为 ORDER BY 序号、无效位置写独立 RED 用例，正确绑定或显式拒绝；这是最清晰的语义缺口。
2. **补最小 SQL 的常规表达边界：** 名称解析与定义唯一性、表级单列主键、经验证的限定名、简单括号；保留原显示名称。
3. **校正向量输入策略：** 将下溢与溢出分开；用已通过基准和独立 float32 预期验证，不再把所有严格拒绝当作正确。
4. **核对可比较的输出元数据：** 无 AS 的函数列名不能只凭手工字符串拼接；先形成明确的断言规则再改。
5. **把持续小批成本作为准入风险单独设计：** 本轮已量化约 1,031 倍写入差距；不宜再靠增加大字段单次样本宣称资源方面完整达标。是否引入增量日志/分段表需另做设计，不在本轮研究中直接重写存储。

不建议的方向：改裸 SQL、恢复 NULL 扩展、添加 JOIN/聚合、无依据复制全部 SET/DROP/显示宽度语法、仅换错误 message 前缀、为了“看起来有索引”直接迁移整套 IVF。

## 11. 复现与工件

```bash
python3 .superpowers/sdd/teammate-comparison/probe_differences.py
python3 .superpowers/sdd/teammate-comparison/probe_storage_amplification.py
```

脚本依赖记录的临时源目录和本地不可变镜像。重复运行会覆盖对应诊断结果，保留本轮证据时应先复制输出目录，或改用新目录；不要把它们当提交镜像的运行组件。

主要工件：

- `comparison-summary.json`：提交、镜像、源码对象等同性、差异分组和写入比值。
- `differential.json`、`differential-summary.json`、`differential.log`：53 项原始预期和实际响应，保留未定项。
- `metadata-and-ddl.json`：默认列名、简单括号、注释、DDL 边界的实际响应。
- `storage-amplification.json`、同名 log：两个实现各阶段 cgroup 计数、预算及强杀恢复。
- `public-base/result.json`：与 b410e23 源码相同的构建经原始公开驱动 20/20。
- 上级目录 `build-*.log`、`image-*.json`：构建与镜像元数据。

**本轮没有修改生产实现、Dockerfile、公开用例、断言或配置，没有提交/推送，也没有将队友 Agent 材料移入我们的工程或提交。所有本轮诊断容器和数据卷已清理。**
