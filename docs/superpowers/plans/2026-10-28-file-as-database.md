# File as Database Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在比赛分支上迁移并完成赛题二的八项功能及 COPY，保留原版公开测试，提供实际构建和回归证据。

**Architecture:** 从固定协作者提交提取格式扫描库与原生 SQL 接入代码，按比赛分支接口适配。Parser/Resolver 生成动态文件表，原生优化器执行关系运算，物理扫描算子进行有界流式读取，COPY 输出端负责 CSV 编码和不覆盖发布。安全文件访问与内存预算由扫描和导出共同遵循，不依赖外部查询引擎。

**Tech Stack:** C++17、GTest、seekdb 原生 Parser/Resolver/优化器/执行器、CMake/CTest、mysqltest、Linux 文件句柄接口。

## Global Constraints

- 设计依据：`docs/superpowers/specs/2026-10-28-file-as-database-design.md`，用户已确认。
- 比赛功能基线 `3bdb27f8404691d899a9747d64e04b7362174749`；设计文档提交 `1c8f9fb25`。
- 协作者功能来源固定为 `a7c03b804..3381bcbfddef3619c4d2512328c7354938dcd6bb`；不跟随远端分支移动，不整分支 merge。
- 只迁移直接功能差量，不引入新版构建/存储体系。
- 用户追加约束：只用 CMake 构建与 CTest 测试，暂不适配或验证 Bazel；不新建 BUILD.bazel，不修改 Bazel 规则/测试接口。仅保留 CMake 当前实际消费的 `.bzl` 生产源码清单输入。
- 输入只读，功能默认关闭，每次实际访问均受当前 `secure_file_priv` 约束。
- 单条 CSV/JSONL 记录最多 16 MiB，最多 4096 列；单个 Parquet Footer、压缩页及解压页分别最多 64 MiB；glob 最多 100000 个结果。
- 单个扫描器的动态工作区设 256 MiB 上限，涵盖其 Schema、记录、页、字典和批次等动态缓冲；与查询内存约束共同生效。
- CSV/JSONL 全量推断，`sample_size` 仅支持 `-1`；不缓存全文件或完整 Row Group 数据。
- 保留比赛分支公开 file_sql 测试、结果、夹具和清理脚本，专项测试另加。
- 不修改或提交 `docs/ai-function-enhancement-progress.md` 的现有用户变更。
- 新头文件使用 `#pragma once`；不为移除前缀重命名既有类型；不写独立 tools 测试程序。
- 每个修正先验证失败测试，再最小实现、复跑和独立提交。迁移任务的红灯可为明确的缺头文件/未支持语法，后续缺陷测试必须实际触发错误行为。
- 实施前使用 using-git-worktrees 技能建立隔离工作区。构建缓存和已有实例不随意复制、清理或复用；最终如何整合回比赛分支由用户决定，不自动推送。

---

## 0. 文件职责与验证入口

### 文件地图

| 文件 | 动作与职责 |
| --- | --- |
| `src/sql/engine/basic/ob_file_scan_utils.{h,cpp}` | 从协作者迁移：CSV/JSONL、类型、Schema、Scan、CsvWriter |
| `src/sql/engine/basic/ob_file_parquet_scan.{h,cpp}` | 从协作者迁移：Footer、页、编码、Snappy、列裁剪 |
| `src/sql/engine/basic/ob_file_table_op.{h,cpp}` | 从协作者迁移：SQL 行/批量、列映射、生命周期 |
| `src/sql/optimizer/ob_log_file_table.{h,cpp}` | 从协作者迁移：原生逻辑文件表 |
| `src/sql/resolver/dml/ob_file_table_resolver.cpp` | 从协作者迁移：选项、推断、动态列绑定 |
| `src/sql/engine/basic/file_sql_file_access.{h,cpp}` | 新增：授权目录内的句柄访问、身份检查、临时文件发布 |
| `src/sql/engine/basic/file_sql_memory.h` | 新增：预算计数和供内部容器使用的有界分配器 |
| `src/sql/parser/sql_parser_mysql_mode.y`、`non_reserved_keywords_mysql_mode.c` | 迁移语法；补 COPY 分派，不改变普通标识符语义 |
| `src/query/api/query/parser/ob_item_type.h`、`src/objit/include/objit/common/ob_item_type.h` | 保持文件节点枚举一致 |
| `src/query/api/query/engine/ob_operator_reg.h`、`ob_phy_operator_type.h`、`src/query/api/query/ob_sql_define.h` | 文件算子注册 |
| `src/sql/engine/ob_operator_factory.cpp` | 包含物理算子定义 |
| `src/sql/resolver/dml/ob_dml_resolver.{h,cpp}`、`ob_dml_stmt.{h,cpp}`、`ob_select_resolver.{h,cpp}` | 文件表类型、列命名空间、输出展开及 COPY 源 |
| `src/sql/resolver/dml/ob_column_namespace_checker.cpp`、`src/sql/resolver/ob_resolver_utils.{h,cpp}` | 动态列查找和重复引用 |
| `src/sql/optimizer/ob_join_order.{h,cpp}`、`ob_log_plan.{h,cpp}`、`ob_log_operator_factory.{h,cpp}` | 文件表路径、成本、JOIN 集成 |
| `src/sql/code_generator/ob_static_engine_cg.{h,cpp}` | 文件规格及列映射，COPY 规格生成 |
| `src/sql/resolver/ddl/ob_explain_resolver.{h,cpp}` | DESCRIBE 输出元数据，不拼接不安全 SQL |
| `src/sql/resolver/ob_resolver.cpp`、`src/sql/resolver/dml/ob_select_stmt.{h,cpp}` | COPY 到原生 SELECT/INTO 的解析与状态传递 |
| `src/sql/optimizer/ob_log_select_into.{h,cpp}`、`src/sql/engine/basic/ob_select_into_op.{h,cpp}` | COPY 独立模式，流式导出、发布与清理 |
| `src/share/system_variable/ob_sys_var_class_type.h`、`ob_sys_var_meta.{h,cpp}`、`ob_system_variable_alias.h`、`ob_system_variable_init.{cpp,json}` | 默认关闭开关及精确计数 |
| `src/sql/session/ob_system_variable_factory.{h,cpp}` | 系统变量工厂 |
| `src/sql/sql_source_inventory.bzl` | CMake 现有清单转换器读取的生产源码输入；登记新增 cpp，不扩展到 Bazel 规则适配 |
| `tools/module_check/sql_source_ownership_check.py` | 新增文件后更新精确分类计数，不放宽源码清单完整性检查 |
| `unittest/sql/engine/basic/test_file_scan_utils.cpp`、`unittest/sql/parser/test_file_sql_parser.cpp` | 迁移并扩充聚焦单测 |
| `unittest/CMakeLists.txt`、`unittest/sql/CMakeLists.txt` | 新建最小 CMake 测试入口，接到根 CMake 已有条件式 add_subdirectory |
| `unittest/sql/engine/basic/CMakeLists.txt`、`unittest/sql/parser/CMakeLists.txt` | 新建扫描库独立 CMake 测试与依赖原生 oceanbase 的 Parser 测试 |
| `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_regression.test`、`r/mysql/file_sql_regression.result` | 新增 SQL 专项回归，不改基础用例 |
| `docs/file-sql.md`、`docs/file-sql-progress.md` | 实际行为、限制、已跑命令与剩余风险 |

现有协作者类名保留；新安全和预算类型放入 `oceanbase::sql::file_sql`。扫描实现中的纯标准库源文件单独翻译，不与其他匿名命名空间合并为 Unity 文件。

### 环境与命令

已核对：有 CMake、C++ 编译器、GTest 头和静态库、`build_release/src/observer/seekdb`、obclient/mysqltest。已有 CMake 产物不等于迁移后构建结果。`unittest/CMakeLists.txt` 当前不存在，需要新增最小 SQL 测试入口；不重建全仓测试基础设施。根 CMake 已有 Linux 条件式测试入口，原生共享库目标为 `oceanbase`。

每个隔离工作区先重新确认依赖路径。以下变量仅为执行会话临时变量，不写用户 shell 配置：

```bash
export FILE_SQL_BUILD_DIR="$(mktemp -d /tmp/seekdb-file-sql-build.XXXXXX)"
export FILE_SQL_GTEST_ROOT="$PWD/deps/3rd/usr/local/oceanbase/deps/devel"
test -f "$FILE_SQL_GTEST_ROOT/include/gtest/gtest.h"
test -f "$FILE_SQL_GTEST_ROOT/lib/libgtest.a"
```

独立扫描单测构建命令（任务 1 建立源文件与 CMakeLists 后）：

```bash
cmake -S unittest/sql/engine/basic -B "$FILE_SQL_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Debug -DFILE_SQL_GTEST_ROOT="$FILE_SQL_GTEST_ROOT"
cmake --build "$FILE_SQL_BUILD_DIR" --target test_file_scan_utils -j3
ctest --test-dir "$FILE_SQL_BUILD_DIR" --output-on-failure
```

任务 3 新增文件访问实现后，CMake 的扫描源列表加入 `src/sql/engine/basic/file_sql_file_access.cpp`。不要把测试 main 放入测试源码，使用已有 GTest main 静态库。

服务端与 Parser 验证：在依赖已配置的目标工作区执行 `./build.sh release` 重新生成，然后执行：

```bash
cmake --build build_release --target seekdb test_file_sql_parser -j3
ctest --test-dir build_release -R '^file_sql_parser$' --output-on-failure
```

若隔离工作区尚无依赖，先依仓库工具链说明准备依赖；不得把基础设施缺失记为功能测试失败或成功。不运行 Bazel，不为缺少 Bazel 报告任务阻塞，不擅自安装系统工具链。

### 原版夹具完整性检查

每个里程碑执行以下检查；新增专项用例不影响这些原版文件：

```bash
python3 - <<'PY'
import subprocess
from pathlib import Path
baseline = '3bdb27f8404691d899a9747d64e04b7362174749'
prefix = 'tools/deploy/mysql_test/test_suite/file_sql'
paths = subprocess.check_output(
    ['git', 'ls-tree', '-r', '--name-only', baseline, prefix], text=True
).splitlines()
for name in paths:
    expected = subprocess.check_output(['git', 'show', f'{baseline}:{name}'])
    assert Path(name).read_bytes() == expected, name
print(f'original file_sql fixtures unchanged: {len(paths)} files')
PY
```

## Task 1: 迁移独立扫描库，建立红绿测试与比赛构建接入

**Files:** 创建地图中的扫描/Parquet 四个文件、扫描单测、`unittest/CMakeLists.txt`、`unittest/sql/CMakeLists.txt`、扫描目录 CMakeLists；修改 CMake 所用 SQL 生产源码清单及精确源码计数。Parser 测试目录在任务 5 接入，不引用尚不存在的目录。

**Interfaces:** 沿用协作者 `Definition infer(const std::string &, const std::string &, const Options &)`、`Scan(const Definition &, const std::string &, const std::vector<int> &)`、`bool Scan::next(std::vector<Cell> &)`、`void Scan::rescan()`、`CsvWriter` 和类型函数；后续任务不改变调用方的基本使用方式。

- [x] **1. 先引入测试，运行并确认缺少功能源/头文件的红灯。** 从固定提交读取测试正文到 `unittest/sql/engine/basic/test_file_scan_utils.cpp`，不迁移 `test_main.cpp`。创建下述 CMakeLists 并执行独立配置/构建命令；预期 CMake 报扫描生产源不存在，或编译器报找不到 `sql/engine/basic/ob_file_scan_utils.h`。

```bash
git show 3381bcbfd:unittest/sql/engine/basic/test_file_scan_utils.cpp
```

使用 write 工具保存该已检查正文，而不是 checkout 整个目录。配置/构建的失败须确认为本功能文件缺失，而非 GTest 或编译器等环境缺失；引入生产文件后转绿。

- [x] **2. 逐一迁移四个扫描文件。** 对下列每个路径使用 `git show 3381bcbfd:<path>` 读取并保存，不覆盖既有文件；不拷贝协作者的构建脚本或夹具：

```text
src/sql/engine/basic/ob_file_scan_utils.h
src/sql/engine/basic/ob_file_scan_utils.cpp
src/sql/engine/basic/ob_file_parquet_scan.h
src/sql/engine/basic/ob_file_parquet_scan.cpp
```

- [x] **3. 加入 CMake 构建。** 两个 `.cpp` 进入 CMake 转换器读取的 `SQL_STANDALONE_SOURCES`，不改头归属清单和 Bazel 规则。源码清单检查精确反映增加的两个 standalone 文件。

`unittest/CMakeLists.txt` 只包含 `add_subdirectory(sql)`，`unittest/sql/CMakeLists.txt` 此时只包含 `add_subdirectory(engine/basic)`。扫描目录 CMakeLists 同时支持根工程接入和独立配置：

```cmake
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(file_sql_scan_tests LANGUAGES CXX)
  enable_testing()
endif()
get_filename_component(FILE_SQL_REPO_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(FILE_SQL_GTEST_ROOT
  "${FILE_SQL_REPO_ROOT}/deps/3rd/usr/local/oceanbase/deps/devel"
  CACHE PATH "Existing GTest installation")
find_package(Threads REQUIRED)
add_library(file_sql_gtest STATIC IMPORTED GLOBAL)
set_target_properties(file_sql_gtest PROPERTIES
  IMPORTED_LOCATION "${FILE_SQL_GTEST_ROOT}/lib/libgtest.a"
  INTERFACE_INCLUDE_DIRECTORIES "${FILE_SQL_GTEST_ROOT}/include")
add_library(file_sql_gtest_main STATIC IMPORTED GLOBAL)
set_target_properties(file_sql_gtest_main PROPERTIES
  IMPORTED_LOCATION "${FILE_SQL_GTEST_ROOT}/lib/libgtest_main.a")
set_property(TARGET file_sql_gtest_main PROPERTY
  INTERFACE_LINK_LIBRARIES "file_sql_gtest;Threads::Threads")
add_executable(test_file_scan_utils
  test_file_scan_utils.cpp
  "${FILE_SQL_REPO_ROOT}/src/sql/engine/basic/ob_file_scan_utils.cpp"
  "${FILE_SQL_REPO_ROOT}/src/sql/engine/basic/ob_file_parquet_scan.cpp")
target_compile_features(test_file_scan_utils PRIVATE cxx_std_17)
target_include_directories(test_file_scan_utils PRIVATE "${FILE_SQL_REPO_ROOT}/src")
target_link_libraries(test_file_scan_utils PRIVATE file_sql_gtest_main)
add_test(NAME file_scan_utils COMMAND test_file_scan_utils)
```

不要同时再链接 oceanbase 到独立扫描测试，否则会重复引入扫描实现。

- [x] **4. 运行独立扫描单测，预期现有七个测试通过；若不通过，先使用 systematic-debugging，不改断言绕过。** 另运行：

```bash
python3 tools/module_check/sql_source_ownership_check.py .
python3 tools/cmake/emit_bazel_source_inventory.py \
  --repo . --output "$FILE_SQL_BUILD_DIR/inventory.cmake"
git diff --check
```

归属检查先保存迁移前结果；既有问题单独记录，新增缺失归属或重复归属必须清零。

- [x] **5. 提交仅本任务文件。** `feat(sql): port file scan utilities to the competition baseline`。保留失败/通过输出，记录没有进行 SQL 端验证。

**执行记录（任务 1）：** 隔离 worktree `.worktrees/file-as-database`，分支 `feat/file-as-database-cmake`。先配置 CMake，确认因扫描生产源缺失失败；逐文件迁移后配置、构建、CTest 成功，GTest 7/7 通过。源码清单检查及 CMake 清单生成成功；生成器有既存 Python AST 弃用警告。比赛原版 file_sql 的 8 个文件逐字节未变；未运行服务端 SQL。GTest 使用主工作区已有依赖，未复制/改动依赖目录。

## Task 2: 用原始 Parquet 夹具验证解码，禁止替换数据

**Files:** 修改 `ob_file_parquet_scan.cpp` 和扫描单测；新增 `unittest/sql/engine/basic/data/file_sql_parquet_fixtures.h`（固定二进制测试数据及来源说明），CMake 测试自动通过源码 include 跟踪此头。

**Interfaces:** 保持 `ParquetScanner::open`、`schema`、`row_count`、`set_projection`、`next_row`、`reset`。错误必须抛出 `file_sql::Error`，不能将缺失数据页静默补成 NULL。

- [x] **1. 给 `FileScanTest` 增加真实夹具读取测试。** 将比赛原版 base64 解码结果以字节数组放入新测试数据头；引用原始文件哈希，禁止使用协作者替换后的内容。使用现有 `put` helper 写到临时根目录，断言：

```cpp
Options options;
options.format = Format::Parquet;
auto definition = infer(path, root, options);
ASSERT_EQ(1U, definition.columns.size());
Scan scan(definition, root, {0});
std::vector<Cell> row;
for (int64_t expected = 0; expected < 7; ++expected) {
  ASSERT_TRUE(scan.next(row));
  ASSERT_EQ(1U, row.size());
  EXPECT_FALSE(row[0].is_null);
  EXPECT_EQ(std::to_string(expected), row[0].text);
}
EXPECT_FALSE(scan.next(row));
scan.rescan();
ASSERT_TRUE(scan.next(row));
EXPECT_EQ("0", row[0].text);
```

- [x] **2. 运行新测试并记录真实结果。** 如果已通过，不制造失败；继续增加“截断页不能补 NULL”、多 Row Group、可空列、字典页及投影空列表测试，至少为实际修改找到一个可复现红灯。
- [x] **3. 修复命中的解码路径。** 检查 Footer/页长度、dictionary index、RLE/bit-packed 数量、Snappy 输出长度、Row Group 声明行数；列流早于声明行数结束时抛出明确错误，不能在 `next_row` 中当作合法 NULL。保留按页读取，禁止完整 Row Group 解码。
- [x] **4. 运行所有扫描测试、夹具完整性检查；增加两列夹具，其中未引用列使用无法解码的数据页，确认只读取目标列。** 元数据仍必须完整且合法；畸形 Footer 不能通过裁剪逃避验证。
- [x] **5. 提交。** `fix(sql): decode original parquet fixtures without weakening validation`。

**执行记录（任务 2）：** 原版 Snappy/DataPageV2 夹具直接通过，解码后 SHA256 为 `6320823b8bd1f4309f6584f5d6e64d98b60dc731af039b6db3b5a51677bba13d`。新增多 Row Group、空投影、可空字典列和真实页裁剪测试。复现并修复两类错误：列块/Row Group 行数不一致导致补 NULL 或丢行；截断 Footer 泄漏内部 Truncated 异常而不是 Error。两类测试均先失败后通过，独立 GTest 13/13 通过，原版 8 个文件不变。未运行服务端或 SQL 端验证。

## Task 3: 统一安全文件句柄与变化检测

**Files:** 创建 `file_sql_file_access.{h,cpp}`；修改扫描/Parquet 实现和定义序列化、扫描单测、CMake 源列表、生产源码清单及精确计数。

**Interfaces:** 新增 `FileIdentity`（device、inode、size、mtime_ns、ctime_ns），以及 move-only `LocalFile`：

```cpp
static LocalFile open_read(const std::string &path, const std::string &root);
size_t read_at(uint64_t offset, void *buffer, size_t size);
FileIdentity identity() const;
void validate_unchanged(const FileIdentity &expected) const;
```

`LocalFile` 析构关闭其唯一拥有的 fd；禁止复制。ByteSource 和 Parquet FileReader 持有同一授权打开的文件对象/共享所有权，不能校验一个 fd 后重新按路径打开另一个文件。

- [x] **1. 先加失败测试。** 在现有 `mutation_and_security` 外单列测试：推断后用相同长度、恢复 mtime 的新 inode 替换文件；含嵌入 NUL 的路径；符号链接目录逃逸；打开后原路径被换为指向外部的链接；EOF 前后截断。预期明确失败，外部文件字节不变。替换测试使用 `rename`，不要把普通覆写当成 inode 替换。
- [x] **2. 运行新测试确认现有大小/mtime 检查的不足。** 命令为扫描单测配合 `--gtest_filter='FileScanTest.*security*:FileScanTest.*replacement*:FileScanTest.*mutation*'`。
- [x] **3. 实现句柄路径。** 拒绝 NUL；固定授权 root 的目录句柄；逐组件解析路径，目录使用 `openat(..., O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)`；文件使用 `openat(..., O_RDONLY | O_NOFOLLOW | O_CLOEXEC)` 并 `fstat` 要求普通文件。拒绝 `..` 逃逸；不因新封装改变现有 secure_file_priv 禁用语义。允许的内部符号链接需在授权 root 内安全解析并限制跳转次数，不能不加区分地接受 realpath 后重开。
- [x] **4. 连接推断、扫描、重扫和 EOF 的身份检查。** Definition 增加完整身份信息并更新其当前版本内的序列化/反序列化；读句柄身份与目录项身份均需验证，检测已打开后原路径替换。JSONL/CSV/Parquet 全部走相同检查。格式辅助库不读取 SQL 会话，当前权限复核留给任务 5/6 的 SQL 边界。
- [x] **5. 完整跑扫描测试；循环失败路径并比较 `/proc/self/fd` 数量；更新独立编译列表后提交。** `fix(sql): bind file scans to authorized descriptors and identities`。

**执行记录（任务 3）：** 四个新增安全用例先失败，证明旧逻辑不能识别同大小/mtime 的替换及恢复 mtime 的改写，也未拒绝路径 NUL。新增 move-only LocalFile，按授权 root 及规范化路径逐组件 openat/O_NOFOLLOW；CSV/JSONL/Parquet 推断和读取共享同一打开句柄，不按裸路径重开读取。持有句柄与当前目录项均校验 device/inode/size/mtime_ns/ctime_ns，推断完成、扫描前后及重扫复核。glob 以受约束目录句柄枚举，异常用 RAII 关闭 DIR。新增所有权、只读/CLOEXEC、边界、FIFO、目录替换及 128 次失败路径的 fd 回收测试。普通 GTest 21/21 通过，独立 CMake ASan/UBSan CTest 通过；源码清单与原版 8 个文件完整性检查通过，用户文档哈希不变。尚未接入当前 SQL 会话权限复核（任务 5/6），COPY 发布仍待任务 9；无服务端 SQL 验证。

**任务 3 性能复核：** 一次性 CMake Release 对比（临时工程 `/tmp/seekdb-task2-io-benchmark`，同一份 10000 行 CSV，三次执行）发现逐行两次完整目录遍历使扫描耗时由协作者约 4–7 ms 增至约 90–97 ms。已修正：Linux 支持时以 openat2 的 NO_SYMLINKS/NO_MAGICLINKS 一次性核验规范化目录项，仅检查身份，内容仍由原持有 fd 读取；不支持或容器过滤时保留已测试的逐组件安全路径。实际 I/O 前后校验，缓冲行及 EOF 则在发布前校验一次，避免冗余检查。相同数据实测约 8.8–9.1 ms；这是本机小规模测量，不代表 SQL 或大文件性能结论。增加 EOF 后修改测试，普通及 ASan/UBSan 共 22 个 GTest 均通过。

## Task 4: 对记录、元数据、解压和容器分配施加预算

**Files:** 创建 `file_sql_memory.h`；修改扫描/Parquet/文件访问实现及 CMake 单测。

**Interfaces:** 新增共享 `MemoryBudget` 和标准容器 `BudgetAllocator<T>`；分配前检查且成功分配后记账，deallocate 释放。独立格式库仍不依赖 SQL；所有内部可增长容器使用同一扫描预算。公开 std 容器的复制/返回也需计入峰值，不只计算裸页字节。

预算加法必须采用减法形式避免溢出：

```cpp
void charge(size_t bytes)
{
  if (bytes > limit_ - used_) {
    throw Error("file SQL memory budget exceeded");
  }
  used_ += bytes;
}
```

- [x] **1. 先写边界与回收测试。** 16 MiB 记录可处理、16 MiB+1 失败；4096/4097 列；构造 Footer 或 Snappy 长度声明超过 64 MiB 的小文件，要求分配前失败；预算测试分别 charge 到边界、再加 1、释放后重用、模拟底层分配失败并核对未泄漏计费。
- [x] **2. 跑新测试，记录缺少限制的红灯。** 大规模 glob 用预算 helper/小测试预算验证分配路径，同时对固定 100000 上限建立边界断言；不在单测中创建百万文件。
- [x] **3. 实现常量、记录长度检查、元数据数量与偏移校验、深层容器记账。** Footer list/map、字典字符串、解压缓冲、CSV/JSONL 单元格及批次均须覆盖；长度相乘先检查溢出，禁止按恶意声明 resize 后才发现超限。超限抛出 Error，不截断或补 NULL。
- [x] **4. 跑扫描测试；用 Sanitizer 的独立 CMake 扫描构建检查越界、UB 和泄漏。** 另建 mktemp 构建目录，配置时传 `-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` 和 `-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'`，再 cmake --build/ctest，不覆盖普通构建。不能把环境不支持 Sanitizer 记作通过。
- [x] **5. 提交。** `fix(sql): bound file inference and parquet decoding memory`。

**执行记录（任务 4）：** 新增共享 MemoryBudget、BudgetAllocator 和公开 std 边界的峰值预留；嵌套容器与字符串共用预算，扫描器在调用作用域结束后仍持有预算。记录/列数、Footer/页声明、Snappy 输出、集合长度、偏移、整数及混合编码宽度在分配或位移前校验；Parquet 列流限制在各自 ColumnChunk 内并校验解压长度。六个资源测试先暴露记录、列数及 Footer 限制缺失；修正 Snappy 夹具偏移后，临时移除长度 guard 的负对照确认测试能失败。新增多页、页/列块计数、解压长度及 Footer 嵌套回归。普通及 ASan/UBSan CTest 各 36 个 GTest 全通过；源码与头文件归属检查通过（补登记此前新增/迁移的四个头文件）。原版 file_sql 8 文件与用户文档哈希不变。未运行服务端或 SQL 端验证。

## Task 5: 迁移语法、系统开关与文件表原生计划

**Files:** 迁移文件地图中 Parser、节点枚举、系统变量、Resolver 文件表、逻辑/物理文件表、优化器、CG 和注册的直接功能差量；新增 Parser 单测/CMakeLists；修改 CMake 生产源码清单及计数。

**Interfaces:** 保留协作者 `TableItem::FILE_TABLE`、`ObFileTableDef`、`ObLogFileTable`、`ObFileTableSpec/Op`。`SYS_VAR_ENABLE_FILE_SQL` 默认 false。文件表推断调用任务 1–4 的库。COPY 语法可先解析但执行必须明确拒绝，不能成功返回却不导出。

- [ ] **1. 先迁移 Parser 测试正文，不带其 main 或新版 BUILD。** 在 `unittest/sql/CMakeLists.txt` 增加 `add_subdirectory(parser)`。Parser 目录 CMakeLists 为：

```cmake
add_executable(test_file_sql_parser test_file_sql_parser.cpp)
target_compile_features(test_file_sql_parser PRIVATE cxx_std_17)
target_link_libraries(test_file_sql_parser PRIVATE
  oceanbase ob_base file_sql_gtest_main)
set_property(TARGET test_file_sql_parser APPEND PROPERTY
  BUILD_RPATH "$<TARGET_FILE_DIR:oceanbase>")
add_test(NAME file_sql_parser COMMAND test_file_sql_parser)
```

此测试只从根工程配置，不单独复制服务端 include/链接依赖。增加 read_csv/read_json/read_ndjson/read_parquet/glob 作为普通标识符的测试，以及未知/重复命名参数测试；构建 Parser 测试记录尚无新节点/语法的红灯。
- [ ] **2. 读取并提取固定功能 patch。** 执行下列只读命令查看完整差量，再用 edit/write 逐文件适配。禁止 patch 整个 `src` 后保留未审查修改。

```bash
git diff a7c03b804 3381bcbfd -- src/query src/objit \
  src/share/system_variable src/sql/parser src/sql/resolver \
  src/sql/optimizer src/sql/code_generator src/sql/session \
  src/sql/engine/ob_operator_factory.cpp
```

- [ ] **3. 合入扫描算子和逻辑算子的新增正文，改新头为 pragma once。** 新增生产 `.cpp` 精确登记为 standalone 或既有适当模块，禁止双重编译；不要照搬协作者将全部扫描库合为一个 Unity group。同步更新系统变量全套数组、索引、类计数和工厂；不按新版 ID 覆盖比赛已有变量。
- [ ] **4. 修复列绑定重复创建。** 单次绑定每个 `(table_id,column_id)`；SELECT * 从已存在 ColumnItem 收集而不是重新创建一套。元数据携带全 NULL 类型、nullable、小数秒 scale；`header/format/sample_size` 按 literal 类型检查；重复参数明确拒绝。
- [ ] **5. 对权限及缓存建模。** Resolver 读取当前开关和根目录；open/rescan 再查会话权限，与任务 3 句柄检查配合。文件 Schema 改变时缓存计划明确失效/报错，不用旧定义继续执行。
- [ ] **6. 构建并验证。** 运行 Parser 单测、扫描单测、服务端构建与归属检查。SQL smoke：默认关闭返回 1235，开启后 `SELECT *`/COUNT(*) 能查询；COPY 在任务 9 前明确返回不支持。记录普通 SQL 和 EXPLAIN smoke。
- [ ] **7. 提交。** `feat(sql): integrate native file tables on the competition baseline`。

## Task 6: 修复源列映射、批量输出、重扫和生命周期

**Files:** 修改 `ob_file_table_op.{h,cpp}`、`ob_static_engine_cg.cpp`、必要的 `ob_log_file_table.cpp`；新增 `file_sql_regression.test/.result`；扫描单测补裁剪和重扫。

**Interfaces:** `ObFileTableSpec` 新增 `common::ObFixedArray<int64_t, common::ObIAllocator> column_ids_`，与 `column_exprs_` 一一对应并加入当前规格序列化。`ObFileTableOp::inner_get_next_batch(int64_t)` 覆盖批量入口；公共单行转换函数供行/批量复用。

- [ ] **1. 写专项 SQL，先复现单列或重排列失败。** fixture 新建在隔离专项目录；至少包含三列和超过单个 batch 的行数，不能混入公开 glob 的原始目录。

```sql
SELECT name, id FROM read_csv('projection.csv') ORDER BY id;
SELECT name, name AS again FROM read_csv('projection.csv') ORDER BY id;
SELECT COUNT(*) AS n FROM read_csv('projection.csv');
SELECT SUM(id), MIN(name), MAX(name) FROM read_csv('projection.csv');
SELECT a.id, b.name FROM read_csv('projection.csv') a
LEFT JOIN read_csv('projection.csv') b ON b.id=a.id ORDER BY a.id;
```

mysqltest 文件使用 `eval` 和独立目录的绝对路径替换示例路径，并为确定结果编写 `.result`，不要用 `--record` 掩盖原始差异。

- [ ] **2. CG 同时生成源列序号和表达式。** 对每个实际访问列：

```cpp
const int64_t source_index = col_item->column_id_ - OB_APP_MIN_COLUMN_ID;
OZ (spec.column_ids_.push_back(source_index));
OZ (spec.column_exprs_.push_back(rt_expr));
```

检查 source_index 在 Schema 范围内；不得用遍历 ColumnItem 的位置作源列序号。Scan projection 来自 column_ids_；COUNT(*) 保持空 projection；输出循环仅遍历投影结果。

- [ ] **3. 实现批量入口和共享转换。** `batch_count=min(max_row_cnt,MY_SPEC.max_batch_size_)`；使用 `ObEvalCtx::BatchInfoScopeGuard` 设置 batch_size/index，为每一行转换到相应 datum 槽；同一批次仅在开始清 evaluated flag，保留最后非空批与 end_ 语义。字符串分配在本批有效的表达式内存中，不让后续行覆盖先前行。
- [ ] **4. 生命周期和错误。** inner_rescan 重置批次、EOF、glob 索引与扫描游标；close/destroy 幂等释放 scan 和 STL 持有的堆内存，避免 arena 对象未析构泄漏。Error、bad_alloc、I/O 和取消在 SQL 边界转为恰当 ret。每批和长推断循环周期性检查取消/超时，不只在返回首行后检查。
- [ ] **5. 跑行/批量计划的等价专项回归、扫描单测、原版 SQL 前半段；核对 EXPLAIN 的文件扫描、聚合与 JOIN 均由原生算子执行。** 不假设某种 JOIN 算法必然被选择；显式算子单测验证 rescan，SQL 测试验证实际计划结果。
- [ ] **6. 提交。** `fix(sql): map projected file columns and support batch rescans`。

## Task 7: 端到端类型、表达式和 JOIN 正确性

**Files:** 修改文件表 Resolver/Op 的类型与转换；扫描单测及专项 mysqltest。

**Interfaces:** 沿用 Type、Column；动态列 `ObObjMeta`、`ObAccuracy`、可空性与文本转换保持一致，不只让 `type_name()` 正确。

- [ ] **1. 增加红灯用例。** 全 NULL 列、BIGINT 两端及溢出、BIGINT/DOUBLE 宽化、BOOLEAN 聚合、DATE/DATETIME 混合、小数秒、非法闰日；CSV 与 JSONL 字段并集组合查询。

```sql
SELECT id, COUNT(*), SUM(score), AVG(score), MIN(score), MAX(score)
FROM read_json('types.jsonl') GROUP BY id HAVING COUNT(*) > 1 ORDER BY id LIMIT 2;
SELECT c.id, j.name FROM read_csv('left.csv') c
INNER JOIN read_json('right.jsonl') j ON c.id=j.id ORDER BY c.id;
SELECT c.id, p.name FROM read_csv('left.csv') c
LEFT JOIN read_parquet('right.parquet') p ON c.id=p.id ORDER BY c.id;
```

各 fixture 在测试中声明确定值；LEFT JOIN 必须有不匹配行，NULL JOIN key 不匹配。加入同名列、别名限定和空输入测试。

- [ ] **2. 跑用例，针对具体失败修复。** 验证数字转换消费完整文本，DATE/DATETIME 的 session 时区与 scale 正确；NULL 映射为 SQL NULL 元数据，不默认 VARCHAR 冒充。GROUP/HAVING/JOIN 保持原生路径，禁止在扫描器内硬编码查询逻辑。
- [ ] **3. 复跑相关单测和专项 mysqltest、普通表等价 SQL。** 检查跨格式结果与手工确定结果一致，不能仅比较两个可能共享缺陷的文件读取路径。
- [ ] **4. 提交。** `fix(sql): preserve inferred types through native file queries and joins`。

## Task 8: DESCRIBE 输出实际 SELECT 元数据

**Files:** 修改 `ob_explain_resolver.{h,cpp}`、必要的 Parser 描述标志及专项回归。

**Interfaces:** DESCRIBE 文件查询返回 `column_name,column_type,null,key,default,extra`；普通 EXPLAIN 和普通表 DESCRIBE 沿用旧语义。描述来源为 SELECT 输出表达式，不扫描执行结果。

- [ ] **1. 先写失败用例。** 基础星号、重排序/别名、表达式、双文件 JOIN、全 NULL 和带单引号/反斜杠的 CSV 列名。分别在默认 sql_mode 和 NO_BACKSLASH_ESCAPES 下验证。重点断言只描述输出列，不返回第一张文件的全部列。

```sql
DESCRIBE SELECT name AS label, id FROM read_csv('projection.csv');
DESCRIBE SELECT c.id AS left_id, j.name AS right_name
FROM read_csv('left.csv') c LEFT JOIN read_json('right.jsonl') j ON c.id=j.id;
EXPLAIN SELECT id FROM read_csv('projection.csv');
```

- [ ] **2. 替换协作者的拼接 UNION SQL 路径。** 使用 SELECT 已解析 select_items 的 alias、表达式类型、可空性，直接构造常量结果表达式/行及六列输出。基础 Type::Boolean 保留 BOOLEAN 显示，派生表达式按原生 SQL 结果类型显示。列名作为 datum，不拼入 SQL 重新解析。
- [ ] **3. 跑新用例和原版基础 DESCRIBE，核对六列逐字结果；复跑普通 `EXPLAIN SELECT 1` 和普通表 DESCRIBE。** 有无 DESCRIBE 不得改变读取路径权限检查。
- [ ] **4. 提交。** `fix(sql): describe resolved file query output columns safely`。

## Task 9: COPY 原生执行、CSV 选项与不覆盖发布

**Files:** 修改 Parser、`ob_resolver.cpp`、`ob_select_resolver.{h,cpp}`、`ob_select_stmt.{h,cpp}`、`ob_log_select_into.{h,cpp}`、`ob_static_engine_cg.cpp`、`ob_select_into_op.{h,cpp}`、扫描写入库及文件访问 helper；扩充扫描单测和专项 mysqltest。

**Interfaces:** 新增明确的 COPY 模式标志和输出 options，从解析后的 SELECT 状态传到 LogSelectInto、SelectIntoSpec 和执行器；不靠猜测普通 OUTFILE 的孩子数识别 COPY。保持 CsvWriter 的构造/write/commit 公共调用，但改其内部安全协议。

- [ ] **1. 写 SQL 执行红灯。** COPY 子查询、COPY 普通表均写入新路径后读回；自定义 delimiter/quote/escape/nullstr/header；空查询仍按 header 选项输出；输出目标已存在时失败；功能关闭时失败。预期 CSV NULL 和空串不同。

```sql
COPY (SELECT id, name, score FROM read_csv('projection.csv') ORDER BY id)
TO 'copy.csv' (format csv, header, nullstr 'NULL');
SELECT id, name, score FROM read_csv('copy.csv', null_string='NULL') ORDER BY id;
```

- [ ] **2. 接通 COPY 分派。** 表名源构造原生 SELECT * 并正常做权限/列绑定；query 源复用其 SELECT；选项验证类型、重复参数及仅支持 CSV，支持省略选项时的明确默认值。禁止将顶层 T_INTO_OUTFILE 直接交给无此分派的旧 resolver。
- [ ] **3. 输出算子消费原生 child 行/批次，使用现有 datum 转文本能力，保留 DATE/DATETIME 精度及 NULL 标志；CsvWriter 只做 CSV 编码，不执行查询。** 普通 OUTFILE 分支不改变。
- [ ] **4. 改写 CsvWriter 临时文件与发布。** 授权父目录 fd 下 `openat` 排他创建临时文件；使用无数据竞争的随机唯一名；写入、flush/close 错误均传播。Linux 优先 `renameat2(RENAME_NOREPLACE)`；不支持时使用同目录 `linkat` 成功后 unlink 临时目录项，不回退到覆盖 rename。fd 和 unlink 作用域绑定，所有失败不触碰原目标。
- [ ] **5. 加并发与清理测试。** 两个 writer 同目标并发 commit，恰好一个成功、内容完整；目标为悬空符号链接时失败；查询表达式中途报错无最终/临时文件；模拟写入错误和取消后清理；临时名碰撞不截断既有文件。记录原输入/目标哈希前后相同。
- [ ] **6. 跑扫描 writer 单测、专项 SQL、原版 file_sql_basic 全流程以及原有 OUTFILE 测试。** 所有输出都放本次隔离测试目录，不写 run_temp/store。
- [ ] **7. 提交。** `feat(sql): execute COPY with safe atomic CSV publication`。

## Task 10: 原版公开测试、权限变化与回归证据

**Files:** 只扩充专项测试；更新 `docs/file-sql.md`、新增 `docs/file-sql-progress.md`；不改公开基础文件和现有 AI 交接文档。

**Interfaces:** 使用现有 `.github/script/seekdb/mysqltest_for_seekdb.py` 的 `mysqltest_environment`、`prepare_instance`、`run_case`，不新增 tools runner。该 runner 的 prepare_instance 会清理其 base_dir，必须使用本次 mkdtemp 得到的专用目录。

- [ ] **1. 验证环境。** 检查新编译二进制、GTest、obclient/mysqltest；端口选择空闲的独立端口；公开夹具 include 需要到实例 host_ip 的免交互 SSH/SCP。若无法连接，停在环境阻塞，不修改测试绕过，也不修改用户 SSH 配置。
- [ ] **2. 建立单次隔离实例，保存 base_dir。** 用 `tempfile.mkdtemp(prefix='seekdb-task2-instance-')` 创建目录，另建日志目录；通过现有 sdb.py/runner 初始化。不要指向已有 `run_temp`。连接后配置 test admin 会话/实例允许本次测试根目录；核对原版 include 的 data_for_mysqltest_file_sql 位于授权范围内。
- [ ] **3. 按名称筛选现有用例并运行，不执行整库。** 调用示例的 API 形状如下；参数值从本次隔离实例取，不硬编码已有运行目录：

```python
cases = runner.discover_cases(repo_root)
selected = [case for case in cases if case.name in {
    'file_sql.file_sql_basic', 'file_sql.file_sql_regression'
}]
assert len(selected) == 2
for case in selected:
    return_code, output = runner.run_case(args, deploy_dir, case, tmp_dir, log_dir)
    if return_code:
        raise RuntimeError(f'{case.name}: exit={return_code}\n{output}')
```

用 `importlib.util.spec_from_file_location` 在临时会话脚本加载现有 runner；不给仓库新增一次性 runner。结果日志完整保存到此次 work_dir，不发布凭据。

- [ ] **4. 跑安全专项。** 准备后关闭开关、收紧 secure_file_priv、替换同长文件、重扫及扫描期间改写、glob 逃逸、预算超限、COPY 发布竞争；期望明确错误及资源回收。增加 prepared/repeated 执行验证，不能依赖首次推断做永久授权。
- [ ] **5. 跑普通 SQL 回归与最终构建。** CMake Parser/GTest、独立扫描 CTest、服务端构建、源码清单一致性、git diff --check、原版夹具完整性。Bazel 不在当前实施与验收范围，不能报告为遗漏或阻塞。原有 AI 测试不修改，不新增其 CMake 适配；视现有运行入口可用性记录结果，不因其他赛题缺陷扩展本任务。
- [ ] **6. 按 code-review 技能检查最终 diff。** 聚焦越界、生命周期、取消、异常传播、COPY 安全、列/批次一致性和证据；发现 Major/Blocker 先补失败测试并修复，不以文档代替修复。不分析持久化升级兼容性。
- [ ] **7. 更新说明与提交。** `docs/file-sql.md` 只写实际实现功能和工程上限；`docs/file-sql-progress.md` 记录每条实际命令、退出状态、测试筛选、原版夹具哈希、未运行范围和剩余阻塞。`docs(sql): record file SQL completion and verification evidence`。

## 检查点与执行方式

- 检查点 A：任务 1–4，独立扫描、安全与预算可验证；此时不声称 SQL 完成。
- 检查点 B：任务 5–7，服务端原生文件查询、类型、批量、JOIN 可验证；COPY/DESCRIBE 缺口明确。
- 检查点 C：任务 8–10，完整公开基础用例及专项回归、最终审查和证据。
- 任务 1→2→3→4→5→6→7→8→9→10 顺序执行，避免在共享注册/清单文件上并行改动。
- 没有子代理工具时使用 executing-plans 在本会话执行；每任务完成后检查 diff、实际测试和提交内容。遇到设计不成立或基础设施阻塞，报告证据并暂停该检查点，不能静默弱化设计。

## 计划自审映射

| 设计要求 | 覆盖任务 |
| --- | --- |
| 比赛基线、不带新版无关改动、原版夹具不变 | 1、2、5、10 |
| CSV/JSONL、确定性推断、七类 SQL 元数据 | 1、4、7 |
| Parquet 编码/Snappy、多 Row Group、可空列与裁剪 | 2、4、6 |
| 命名参数、别名、glob、默认开关 | 3、4、5 |
| 原生过滤/表达式/分组/排序/HAVING/LIMIT 与 JOIN | 5、6、7 |
| 批量、重扫、生命周期和取消 | 3、4、6、10 |
| DESCRIBE、不破坏普通 EXPLAIN/DESCRIBE | 5、8、10 |
| COPY 表与查询、选项、读回、禁止覆盖、错误清理 | 3、9、10 |
| secure_file_priv、权限变化、路径逃逸、文件变化 | 3、5、6、10 |
| 记录/列/页/glob 上限与扫描总工作区 | 4、6、10 |
| 测试位置、红绿过程、构建/回归证据与最终审查 | 每任务及10 |

以上命令和期望是实施步骤，不表示已经执行或通过；写计划阶段只检查路径、现有工具接口和文档一致性。
