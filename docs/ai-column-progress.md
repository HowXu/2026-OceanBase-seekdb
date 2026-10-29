# 赛题三：AI 列实现进度

## 基线和当前边界

- 分支：`kernel/2026-competition`。
- 设计：`docs/superpowers/specs/2026-10-29-ai-column-design.md`。
- 计划：`docs/superpowers/plans/2026-10-29-ai-column.md`。
- 用户选择当前会话内执行，不使用子代理，不创建额外 worktree。耗时 CMake 编译由用户手动执行。
- 保留赛题一、二的已有实现和原始竞赛用例。

**当前只验证了 Parser 和 Strict Read 系统变量注册。Schema、DDL 执行、DML 任务维护、Worker、条件重试执行、严格读取检查及向量联动尚未实现。不能把语法和开关测试通过当作赛题完成。**

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

## 下一步

1. Parser 和系统变量注册阶段已完成验证，并作为一个独立实现提交。
2. 实施 Schema、DDL Resolver 和官方 DDL 用例；不把尚未实现的 SQL 入口描述为可用。

## 环境与验证边界

独立测试目录最初为 `/var/tmp/seekdb-task3-evidence/instance`，系统变量 GREEN 使用全新的 `/var/tmp/seekdb-task3-evidence/sysvar-instance`，端口 `18883`；不操作其他实例的数据。

原测试进程在二进制重链接后显示为 `(deleted)`，`sdb.py stop` 曾误判 PID 为陈旧并只删除 PID 文件。后续已通过 `/proc` 核对 executable、base-dir 和端口，仅向本次拥有的旧进程发送 SIGTERM，确认停止后才启动新实例；未改动该无关启动工具。官方 AI 列 test/result/Mock 与 `upstream/2026-competition` 无差异。当前没有运行全部 AI 列用例、故障恢复专项、向量索引或性能评测，不承诺比赛得分。
