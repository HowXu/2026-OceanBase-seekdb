# 赛题三：AI 列实现进度

## 基线和当前边界

- 分支：`kernel/2026-competition`。
- 设计：`docs/superpowers/specs/2026-10-29-ai-column-design.md`。
- 计划：`docs/superpowers/plans/2026-10-29-ai-column.md`。
- 用户选择当前会话内执行，不使用子代理，不创建额外 worktree。后续已授权代理执行构建，并行度最高 `-j10`。
- 保留赛题一、二的已有实现和原始竞赛用例。

**最新验收范围：用户要求只以原版官方五套 AI Column 测试全部通过为目标。不再扩展额外 SHOW、故障恢复或性能专项；只为定位官方失败点补必要的小测试。官方 test/result/Mock 不修改，也不以硬编码模型行为或绕过内核语义换取通过。**

Parser 和 Strict Read 系统变量注册已验证并提交为 `86aa5207e`。Schema/DDL、事务 DML 维护及后台 Worker 已提交为 `4f41681b9`。条件 RETRY 与 strict read 执行检查已完成首轮实现并成功构建，但最后一项官方测试尚未通过，不能把编译成功当作功能验收。

**当前官方验证为 4/5（分别运行，不冒充一次完整五套串行验收）：** `ai_column_ddl` 已在先前重建实例完整通过；最新 Worker 二进制上 `ai_column_t1_dml`、`ai_column_worker`、`ai_column_vector_index` 均原样完整通过。最新证据目录为 `/var/tmp/seekdb-task3-evidence/rebuilt-official-20261029-171151` 和 `rebuilt-official-20261029-171216`。`ai_column_retry_strict` 仍待实现，因此不能声称五套验收完成。

本阶段已实现业务事务内任务创建/失效/删除、独立 AI 嵌套 SQL 标记、受控隐藏列访问、提交后的持久化 Worker、HTTP 外事务调用、按 request 组发布、失败重试、JSON/整数/向量转换及旧 request 防覆盖。官方 Worker 的 HTTP 400 后成功、慢旧请求不覆盖新结果、最终失败和向量索引链路均通过原版断言。转换器独立测试 6/6 通过；`git diff --check` 通过，官方夹具未修改。

后续 RETRY 阶段补充了集合 SQL 构造测试，DML 辅助测试为 **12/12**。官方 RETRY/strict 已推进到首个预期 11120 检查点；物理计划现在保存实际引用的 AI 列，执行前按当前隐藏状态检查并返回新生成的 11120 错误。该阶段仍有初始结果发布完整性及 strict 端到端行为待验证，最后一套不标记 GREEN。

## 已实现并验证：Parser

- CREATE / ADD / MODIFY 的 `AI COLUMN(expr) [WITH(...)]`。
- AI 表达式、选项和选项值使用独立 AST，不把定义塞入注释。
- `ALTER TABLE ... RETRY AI COLUMN ... [WHERE ...]`。
- `SHOW AI COLUMNS FROM ...`。
- `retry` 保持非保留关键字。
- RETRY 条件保留 `status` 的引用标记，反引号引用与未引用可在 Resolver 中区分。
- 混合 RETRY 和其他 ALTER 动作保留完整 AST，后续 Resolver 返回官方要求的 1235，而不是 Parser 1064。此处修正了原实施计划中过早在 Parser 拒绝的约定。

新增单测：`unittest/sql/parser/test_ai_column_parser.cpp`，8 个测试。

### RED 证据

1. 使用改动前二进制运行原始 `ai_column_ddl.test`，首个 `CREATE TABLE ... AI COLUMN` 返回 MySQL 1064。
2. 初版 Parser 单测中的 CREATE、ADD/MODIFY、RETRY 和 SHOW 因不支持语法而失败（内部错误 -5001）。最初测试还错误使用 -5006 作为语法返回值；发现后改为让混合 ALTER 语句进入 Resolver，以符合官方 1235。

证据：`/var/tmp/seekdb-task3-evidence/ddl-red.log`、`ddl-red.json`；Parser 初始日志在 `/tmp/seekdb-ai-column-parser-red.log`。

### GREEN 证据（2026-10-29）

用户手动完成本阶段编译；实际使用 `build_file_sql_server`，其中二进制比 `build_release` 更新，并且已启用 GoogleTest。代理随后执行：

```bash
ctest --test-dir build_file_sql_server \
  -R '^(ai_column_parser|file_sql_parser)$' --output-on-failure

./build_file_sql_server/unittest/sql/parser/test_ai_column_parser \
  --gtest_filter='AIColumnParser*.*'
```

结果：两个 CTest 目标全部通过，AI Column Parser 8 tests、0 failures；文件 SQL Parser 回归通过。日志：

- `/var/tmp/seekdb-task3-evidence/parser-tests-after-manual-build.log`
- `/var/tmp/seekdb-task3-evidence/parser-eight-tests.log`

使用仓库固定 Bison 2.4.1 执行 `src/sql/parser/gen_parser.sh` 成功，无 grammar conflict。生成文件属于仓库既有忽略项，提交 grammar、非保留关键字表和 item type 源文件，不强行提交生成文件。

## 已实现并验证：Strict Read 系统变量注册

注册 `ai_column_strict_read`：BOOL、默认 OFF、SESSION、MYSQL_ONLY、NEED_SERIALIZE。此处仅注册开关，尚不实现非 READY 行检查。

新增 SQL 回归：

- `unittest/sql/session/mysqltest/ai_column_system_variable.test`
- `unittest/sql/session/mysqltest/ai_column_system_variable.result`

覆盖默认 OFF、开关、独立会话、非法值保持原设置，以及拒绝 SET GLOBAL。

旧二进制运行该测试在第一条 SET 返回 1193（未知系统变量），保存在 `/var/tmp/seekdb-task3-evidence/sysvar-red.log`。

用户增量编译完成后，代理于 2026-10-29 在全新实例 `/var/tmp/seekdb-task3-evidence/sysvar-instance`（18883）运行该原样用例，退出码 0，预期结果完全匹配；日志为 `/var/tmp/seekdb-task3-evidence/sysvar-green.log`。随后重新运行两个 Parser CTest，均通过。

系统变量生成器使用 Python 2 的 `.iteritems()`；本机没有 Python 2。通过临时副本仅替换为 `.items()`，分别生成变更前后代码，应用两次生成结果间的精确差量。没有修改仓库生成器，也没有覆盖原生成文件中的既有日志修正及字段顺序。

## 已验证：Schema 定义对象及列 Schema 接入（不含 DDL 执行）

新增 `src/share/schema/ob_ai_column_schema.{h,cpp}`，保存表达式、规范化表达式、重试次数、依赖列和三个隐藏列 ID。字符串及依赖列表由 Schema allocator 持有；支持深拷贝、序列化、重置和完整列 ID 重映射，映射缺失时不部分修改该定义。

`ObColumnSchemaV2` 仅为 AI 列分配定义对象，接入复制、序列化、重置、释放、列 ID 转换及所属列标识同步；普通列不内嵌完整 AI 定义。此处只保存隐藏列的 ID，尚未由 DDL 创建隐藏列或持久化到内部表。

新增测试位于既有单测根目录下的 `unittest/share/schema/`，注册目标 `test_ai_column_schema` / CTest `ai_column_schema`，共 10 项：6 项定义对象测试、4 项列 Schema 集成测试。

验证记录：

- 初始失败记录：`/var/tmp/seekdb-task3-evidence/schema-red.log`、`schema-column-integration-red.log`，分别因 AI 定义类型和列接入接口缺失而无法编译；不将这些编译失败称为 DDL 语义测试通过。
- 代理仅编译新增定义对象和其独立测试、链接现有库运行，6 项通过：`/var/tmp/seekdb-task3-evidence/schema-unit.log`。没有启动完整构建。
- 两个实现源文件和两个测试源文件均通过与 CMake 目标相同参数的 `-fsyntax-only`；这不是完整链接或集成测试通过。
- CMake 配置成功，新目标已注册：`/var/tmp/seekdb-task3-evidence/schema-configure.log`。
- 列布局已改变，因此初次验证未将新列实现与旧共享库混合执行；用户重新构建后，4 项列集成测试与 6 项定义测试全部通过，证据见下文。
- 在包含 Parser 改动的新二进制上原样重跑官方 DDL，用例仍在第一个 CREATE TABLE 失败，返回 1149：`/var/tmp/seekdb-task3-evidence/ddl-after-parser/result.json`。这是当前 Resolver/DDL 缺失的失败基线，不是 Parser 又出现 1064。
- Parser 两个 CTest 再次通过；官方 AI 列 test/result/Mock 未修改，`git diff --check` 通过。

额外运行 Share 架构检查发现当前分支已有的问题：header ownership 对 `ob_tenant_role_transition_service.h`、`ob_server_resource.h` 报错；terminal 检查在 `ob_telemetry.cpp:1080` 报隐式全局上下文访问。没有修改这些无关文件或调整规则来绕过检查；日志为 `/var/tmp/seekdb-task3-evidence/share-*-schema.log`，不声称架构检查通过。

### 用户重新构建后的验证（2026-10-29）

代理未启动完整构建，使用用户构建的二进制执行：

```bash
ctest --test-dir build_file_sql_server --output-on-failure
./build_file_sql_server/unittest/share/schema/test_ai_column_schema --gtest_color=no
```

结果：当前注册的 5 个 CTest 全部通过（`file_scan_utils`、`file_table_op`、`file_sql_parser`、`ai_column_parser`、`ai_column_schema`）；直接执行 Schema 目标得到 10 tests、0 failures，包含此前待运行的 4 项列集成测试。

随后在专用 18883 实例启动用户构建的新二进制：

- `ai_column_system_variable.test` 原样运行成功，与 result 匹配。
- 普通表冒烟覆盖 CREATE、JSON 列 INSERT、事务回滚与提交、ALTER ADD/CHANGE、普通索引查询、DELETE 和 DROP；5 个断言均为 1。临时数据库已清理。
- 原样重跑官方 `ai_column_ddl.test`，仍在第 28 行首个 CREATE TABLE 返回 1149，退出码 1；保留此未实现功能的失败结果，不计入通过项。
- 验证后已停止专用实例并确认 18883 无监听，避免后续重新链接时误用旧 executable。

证据均在 `/var/tmp/seekdb-task3-evidence/`：

- `all-ctest-after-schema-build.log`
- `schema-ten-tests-after-manual-build.log`
- `start-after-schema-build.log`、`stop-after-schema-tests.log`
- `sql-after-schema-build/sysvar-result.json`
- `sql-after-schema-build/plain-schema-smoke.sql`、`plain-schema-smoke.log`
- `sql-after-schema-build/official-ddl-result.json`

## 本轮 DDL 接入及用户重新构建后的官方验证

- 增加定义解析、模型/类型/依赖检查、三个隐藏列分配及 CREATE 接入；规范化调用信息保留类型提示。
- 增加 AI 定义的十六进制 SQL 编解码、列 Schema 当前/历史元数据字段，以及创建、修改、删除的持久化钩子。
- 内部表 ID 使用 1135（定义）和 1136（任务）；1134 已被原有历史表占用。任务键为 `(table_id,column_id,request_id)`，保留官方使用的 `retry_count`、`eligible_ts` 和 `rowkey_predicate` 等字段。
- ALTER ADD/MODIFY 接入原生 DDL action，计入前序普通 ADD 的 ID，修改复用隐藏列 ID；DROP 追加三个隐藏列删除，阻止破坏存活 AI 列输入依赖的变更。
- SHOW CREATE 与 SHOW AI COLUMNS 已写入接入代码；后续不再扩展额外 SHOW 专项。
- 独立 Resolver/打印测试 **32/32** 通过：`/var/tmp/seekdb-task3-evidence/resolver-32-green.log`。Schema/内部表独立测试曾通过 **18/18**，不代表最新完整服务器通过。
- 八个受影响的接入源文件通过短语法检查，包括 ALTER、SHOW、Root DDL、Schema SQL 服务及检索实现；Schema SQL 服务检查预包含其 Unity 依赖。这不是完整构建或端到端验证。
- 两个 Parser CTest 再次通过；`git diff --check` 通过，官方用例树与上游无差异。
- 新增的补充 SQL 位于 `unittest/sql/resolver/mysqltest/`，尚未 GREEN；旧二进制对普通表的 SHOW AI COLUMNS 返回 1235，RED 证据位于 `/var/tmp/seekdb-task3-evidence/show-red/`。

### 本轮官方结果与修复

使用新目录 `/var/tmp/seekdb-task3-evidence/ddl-build-instance`、端口 18883，避免旧实例的内部表定义影响验证。使用原版初始化 SQL 和 mysqltest runner，未修改官方用例。

- `ai_column_ddl` 已完整执行，唯一 result 差异是链式 AI 列依赖的 1235 文本：实际为自定义 `invalid asynchronous AI column definition not supported`，预期为 `Not supported feature or function`。建表、元数据数量、隐藏列、类型提示、ALTER ADD、维度错误及清理结果均匹配。
- 已将 AI Resolver 的不支持错误改为仅记录服务端诊断，不再覆盖客户端标准错误。给现有依赖测试添加 warning buffer 断言，先复现 RED，再运行全部 32 项独立测试 GREEN；日志为 `ddl-error-message-{red,green}.log`。**该修复在用户本次构建之后，尚未编入服务器或确认官方 DDL GREEN。**
- `ai_column_t1_dml` 完整执行但 result 不匹配：首次插入任务不存在，后续模型结果及失效任务断言失败。
- `ai_column_worker` 在 typed-prompts 的 Mock 断言失败：请求列表为空。
- `ai_column_retry_strict` 在首个 RETRY 返回 4016；此前结果就绪断言也为 0。
- `ai_column_vector_index` 单独重跑采用 35 秒超时，退出 124；仍无向量生成结果。较早串行四套运行被外层总时限中断，不冒充一次完整五套运行；汇总合并各次真实结果。

汇总证据：`/var/tmp/seekdb-task3-evidence/rebuilt-five-summary.json`，各项保留原始 `results.json` 路径。官方夹具与上游无差异，实例及四个 Mock 均已停止，端口无监听。

下一阶段聚焦官方 T1 的业务事务内任务创建、失效和删除，再接 Worker；不为错误文本这一项单独发起耗时构建。Task 2 的既存数据初始化与完整生命周期仍未完成，不提前标记整个 DDL 阶段完成。

## 环境与验证边界

独立测试目录最初为 `/var/tmp/seekdb-task3-evidence/instance`，系统变量 GREEN 使用全新的 `/var/tmp/seekdb-task3-evidence/sysvar-instance`，端口 `18883`；不操作其他实例的数据。

原测试进程在二进制重链接后显示为 `(deleted)`，`sdb.py stop` 曾误判 PID 为陈旧并只删除 PID 文件。后续已通过 `/proc` 核对 executable、base-dir 和端口，仅向本次拥有的旧进程发送 SIGTERM，确认停止后才启动新实例；未改动该无关启动工具。官方 AI 列 test/result/Mock 与 `upstream/2026-competition` 无差异。早期旧二进制的五套首个 CREATE 失败基线保存在 `/var/tmp/seekdb-task3-evidence/official-five-20261029-130120/results.json`；本轮已使用用户新构建的服务器验证，最新结果见上节。没有把独立测试、单条成功或 SQL 已执行当作官方 result 匹配；当前仍不承诺比赛得分。
