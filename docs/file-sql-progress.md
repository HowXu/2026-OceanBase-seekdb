# 文件 SQL 实现与验证记录

## 当前状态（2026-10-29）

功能实现和本地专项验证已推进到任务 10；**官方原版 `file_sql_basic` 整流程仍未通过，当前阻塞是本机 SSH 没有监听 22 端口**。不能用下表的构建、单测或专项结果替代官方验收，也不据此估分。

| 检查 | 本轮结果 |
| --- | --- |
| seekdb 与三个测试目标的 CMake `-j3` 构建 | 退出 0 |
| `file_scan_utils` / `file_table_op` / `file_sql_parser` CTest | 3/3 通过，最近约 1.72 s |
| 扫描库 GTest | 50/50，通过三个 test cases，含 8 项内存测试 |
| 独立扫描库 ASan/UBSan CTest | 通过，开启 leak detection / halt-on-error，约 11.07 s |
| COPY 两个 writer 同目标并发发布 | 100/100 次，恰好一个赢家 |
| `file_sql_copy` 专项 | 通过，约 0.304 s |
| `file_sql_describe` 专项 | 通过，约 0.050 s |
| `file_sql_regression` 专项 | 通过，约 0.054 s |
| `file_sql_resources` 专项 | 通过，约 0.707 s |
| `file_sql_security` 专项 | 通过，约 2.106 s |
| `file_sql_types` 专项 | 通过，约 0.015 s |
| 官方原版八个文件与比赛基线的逐字节比较 | 8/8 未改 |
| SQL 源码/头归属与 `git diff --check` | 通过 |
| 官方 `file_sql_basic` 原版整流程 | 未通过：fixture include 的 SSH 返回 Connection refused |

计时只代表本机该次执行，不是性能承诺。ASan/UBSan 仅覆盖独立扫描库，不代表对整个服务端做过 Sanitizer 验证。

## 版本与修改范围

- 比赛基线：`3bdb27f8404691d899a9747d64e04b7362174749`。
- 扫描、安全、预算任务 1–4 合并：`127c7096f`。
- 原生文件表初始集成：`91259149b`。
- 任务 6–9、查询预算与取消的阶段提交：`e2de2f01f`。
- 当前全局权限复核及取消/安全回归：`f3ee378fc`。

没有整分支迁入协作者的新构建/存储体系，没有新增或修改 Bazel 规则，没有运行 Bazel 构建/测试。维护的 `.bzl` 源码/头清单由 CMake 消费。用户的 `docs/ai-function-enhancement-progress.md` 未修改、未纳入本轮提交；赛题一排查继续暂停。

## 已获得的回归证据

### 原生执行与类型

投影错位曾复现 4016，增加源列映射后转绿；批量入口单测曾返回 -4077，实现 row/batch 共享转换后转绿。2049 行用例覆盖独立字符串、COUNT DISTINCT、SUM、分组排序、INNER/LEFT JOIN、NL 重扫，以及 `_rowsets_max_rows=0/256` 的行/批等价。EXPLAIN 确认使用原生 FILE_TABLE/JOIN 路径。

DOUBLE scale 原先为 0，10.5 被显示为 10、MIN/MAX 1.5/2.5 被显示为 2/2；修正后通过。BIGINT 边界、BOOLEAN 聚合、DATE/DATETIME6、全 NULL、JSON 字段并集/缺失字段及跨格式 JOIN 有专项覆盖。空 JSONL 先失败，再允许零列零行 schema；SQL COUNT 为零。

DESCRIBE 先复现返回计划及 SQL mode 下的错误，改为根据实际 SELECT 输出构造常量结果。投影顺序、别名、表达式、全 NULL、LEFT JOIN 可空性、引号/反斜杠列名与普通 EXPLAIN/表 DESCRIBE 均有专项覆盖。

### COPY

原生执行最初返回 1235，接入专用 INTO kind 后通过。覆盖表/子查询、选项、空结果表头、NULL/空串/marker 文本、嵌入分隔符/引号/换行、日期精度、行/批、2049 行、ROW_COUNT、不覆盖和原有 OUTFILE 读回。

库层安全测试先暴露路径 NUL、已有目录项、临时权限、父目录替换、编码后长度和清理问题。修复后验证 0600、held-inode 排他发布、目录项被替换时不误删、写错误清理及 100 次竞争。SQL 超时和执行中撤权均验证最终/临时文件不遗留。LOB/collection 明确拒绝，不宣称全类型 COPY。

### 查询内存与取消

新增父子账本、兄弟扫描器汇总、分配失败回滚、无分配取消检查及返回 schema 保留计费测试。schema 返回后预算曾错误归零，失败测试修正后转绿。零字节预留曾仍触发资源轮询；新增红绿测试并复用行输出 reservation，避免空投影逐行创建 reservation 堆对象。

旧二进制对照：逻辑 memory_budget=2G、query_memory_limit_percentage=1 时，12 MiB CSV 记录仍成功返回 COUNT=1；500 万行 CSV 设置 20 ms 超时，约 1.8382 s 才返回 4012。接入后的同类实测：内存超限返回 11049（限额 21474836，预留检查值 24039656），20 ms 推断超时约 0.0319 s 返回 4012。该计时来自本轮会话中的一次性对照，不是稳定基准。

当前仓库 `file_sql_resources` 还验证：已准备的扫描执行时读取新的限额、新推断同样超限、长推断超时、通过 PROCESSLIST 确认请求运行后发出 KILL QUERY 并收到 1317，以及失败后后续查询正常。测试恢复原 memory_budget 和 query_memory_limit_percentage。

### 当前授权（本轮审查发现并修复）

**Major (fixed): stale session authority.** A second connection could tighten global secure_file_priv while an existing prepared file scan kept using the session snapshot. The regression demonstrated EXECUTE succeeding when error 1227 was required. File SQL now reads and copies the current global value under a schema guard in resolution, scan open/rescan, and COPY access/publication checks.

`file_sql_security` 通过：同大小/mtime 的文件替换使旧计划报错；不同 SQL 缓存键可解析新文件；关闭会话开关使准备计划失败；全局根收紧使准备扫描、已执行过的 DESCRIBE、准备 COPY 和新 COPY 失败；恢复授权后可继续执行。异步 COPY 在临时 inode 已出现后收紧根，发布前被拒绝，目标和临时文件均不存在。

DEALLOCATE 不保证驱逐共享缓存，不能把重新准备相同 SQL 当作强制重新推断。安全/资源专项使用连接 ID 区分 fixture 目录，避免测试重跑时意外命中上一轮的旧身份。

## 实际命令与证据位置

主目录：`/home/howxu/Projects/2026-OceanBase-seekdb`。本轮只操作专用实例 `/home/howxu/seekdb-file-sql-smoke.QDaFgE`（127.0.0.1:47001），没有复用或清理用户其他实例。重建前先通过 sdb.py 停止该实例。

```sh
cmake -S . -B build_file_sql_server \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFILE_SQL_GTEST_ROOT="$PWD/deps/3rd/usr/local/oceanbase/deps/devel"
cmake --build build_file_sql_server \
  --target seekdb test_file_table_op test_file_sql_parser test_file_scan_utils -j3
ctest --test-dir build_file_sql_server \
  -R '^file_(table_op|scan_utils|sql_parser)$' --output-on-failure

python3 /tmp/seekdb-file-sql-run-regression.py \
  file_sql.file_sql_copy file_sql.file_sql_describe \
  file_sql.file_sql_regression file_sql.file_sql_resources \
  file_sql.file_sql_security file_sql.file_sql_types

python3 tools/module_check/sql_source_ownership_check.py .
python3 tools/module_check/sql_header_ownership_check.py .
git diff --check
```

临时 SQL 适配脚本动态加载现有 `.github/script/seekdb/mysqltest_for_seekdb.py` 的 discover_cases/run_case/environment，不向仓库添加 runner，不自动重试掩盖失败。实际 cwd 与环境 PWD 均为 tools/deploy，以满足原版 fixture 的 `$PWD/mysql_test/...`；没有修改原版 SSH/SCP 命令。

独立 Sanitizer：

```sh
asan_dir=$(mktemp -d /tmp/seekdb-file-sql-final-asan.XXXXXX)
cmake -S unittest/sql/engine/basic -B "$asan_dir" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFILE_SQL_GTEST_ROOT="$PWD/deps/3rd/usr/local/oceanbase/deps/devel" \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build "$asan_dir" -j3
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir "$asan_dir" --output-on-failure
```

最新运行证据保存在 `/home/howxu/seekdb-file-sql-final-evidence/`，含 CTest、完整 50 项单测、六专项、COPY 竞争、Sanitizer 日志、逐用例 JSON 和原版八文件 SHA256 清单。`/tmp` 脚本和构建日志可能随重启消失，不能当作永久测试工具。

源码归属为 1157 production + 7 separate = 1164 workspace；头归属为 1276 tracked / 151 public roots / 257 interface closure / 4 composition / 864 private。上层 include/seam 检查无违规。

## 原版夹具及剩余阻塞

原版八文件逐字节与比赛基线相同。原版 Parquet base64 解码内容的 SHA256 为：

```text
6320823b8bd1f4309f6584f5d6e64d98b60dc731af039b6db3b5a51677bba13d
```

实际尝试官方 `file_sql.file_sql_basic` 在 `copy_file_sql_data.inc` 的 SSH 安装阶段失败，未进入官方 SQL 断言。最新检查：

```sh
ssh -o BatchMode=yes -o ConnectTimeout=5 127.0.0.1 true
# ssh: connect to host 127.0.0.1 port 22: Connection refused
ss -ltn 'sport = :22'
# 没有监听项
```

仅放开防火墙端口不等于 SSH 服务已启动。需要由用户提供可用的本机 SSH 服务及免交互认证；本轮未启动服务、修改 SSH 配置、跳过 include 或替换原版夹具。环境就绪后仍须运行原版整流程，若失败再根据真实差异修复。

尚未运行完整 AI mock 回归、全库 mysqltest、整个服务端 Sanitizer，也没有独立第二审阅者的结论。当前记录不等于这些范围通过。
