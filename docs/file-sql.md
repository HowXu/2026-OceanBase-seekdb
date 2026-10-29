# 文件即数据库：使用与边界

文件 SQL 将本机 CSV、JSONL 和受支持的 Parquet 文件暴露为原生关系。扫描器只负责读取和转换，过滤、表达式、聚合、排序与 JOIN 使用 seekdb 的优化器和执行器，不会先导入普通表。

实际验收状态见 [验证记录](file-sql-progress.md)。官方原版 `file_sql_basic` 与七个专项已通过（8/8）；构建、单测和 SQL 验收分别记录，不把这些结果扩大为全库回归通过。

## 开启与授权

`enable_file_sql` 默认 OFF，按会话开启。管理员须先通过本机 Unix socket 将全局 `secure_file_priv` 设置为已有目录：

```sql
-- 管理员 Unix socket 连接；目录须提前创建。
SET GLOBAL secure_file_priv = '/absolute/file_sql_data';

-- 使用文件 SQL 的会话。
SET SESSION enable_file_sql = ON;
```

输入必须是授权目录内的只读普通文件。拒绝路径 NUL、符号链接及非普通文件；打开的 fd、实际目录项和文件身份都会验证。推断与扫描使用同一文件身份，文件被替换或改写时拒绝继续使用旧定义，不补 NULL 或静默跳行。

推断、扫描 open/rescan，以及 COPY 的打开和最终发布读取当前全局授权目录，不能把会话初始化时的副本当作永久授权。权限收紧后，已有准备计划也须重新通过检查。

## 查询

```sql
SELECT name, id
FROM read_csv('/absolute/file_sql_data/input.csv', header = true, sample_size = -1)
ORDER BY id;

SELECT id, name, score
FROM read_json('/absolute/file_sql_data/input.jsonl', format = 'newline_delimited');

SELECT COUNT(*), SUM(c1)
FROM read_parquet('/absolute/file_sql_data/input.parquet');

SELECT file FROM glob('/absolute/file_sql_data/*.csv') ORDER BY file;
```

- `read_json` / `read_ndjson` 读取 JSONL，即逐行对象；按字段并集推断，缺失值为 SQL NULL。数字遵循 JSON 语法，拒绝 `+1`、`01`、`1.`、`1e` 等非法形式，不因 COUNT 空投影而放行。
- CSV 支持表头、重复列名去重、带引号的分隔符、引号转义和跨行记录。闭引号后仅允许非分隔符的空格/制表符填充、配置的分隔符、换行或 EOF，不把非法尾部拆成额外行。
- CSV 推断为数值、布尔或日期时间的字段在读取时沿用推断的首尾空白裁剪；VARCHAR 原始空白、带引号的 NULL 文本与未加引号的 NULL marker 保持可区分。
- CSV/JSONL 采用全量推断，仅接受 `sample_size = -1`，不缓存整个输入文件。
- 推断类型包括 BIGINT、DOUBLE、BOOLEAN、DATE、DATETIME、VARCHAR 和全 NULL 类型；DATETIME 保留支持的小数秒精度。数值计算使用对应的原生 SQL 类型。
- 空 CSV/JSONL 可计数为零，不虚构数据列。
- Parquet 支持平面 schema、多个 Row Group、列裁剪、可空字典及已覆盖的 DataPage V1/V2 路径。不是完整的 Parquet/Arrow 实现；不支持的 schema、编码或压缩分支明确报错。
- 普通别名、重排/重复投影、COUNT 空投影、GROUP/HAVING/ORDER/LIMIT、INNER/LEFT JOIN 使用原生计划；支持单行、批量和重扫路径。

CSV 语法/列数错误及 JSONL 解析错误携带文件、记录号和记录起始字节偏移。字节偏移从 0 计数；CSV 记录号包括表头与跳过的空记录，但不被引号内换行打断；JSONL 记录号对应含空行的物理行号。错误文本上限仍为 1024 字节。超时、取消和查询内存错误继续返回原生错误码。

## DESCRIBE

```sql
DESCRIBE SELECT name AS label, id
FROM read_csv('/absolute/file_sql_data/input.csv');
```

返回 `column_name,column_type,null,key,default,extra` 六列。描述来自实际 SELECT 的输出项，保留投影顺序、别名、表达式类型与 JOIN 的 NULL 扩展，不用第一张文件的所有列代替查询结果。列名作为值构造，不拼接回 SQL，因此不受列名中的引号或 `NO_BACKSLASH_ESCAPES` 影响。

文件身份变化后，缓存旧定义会明确报错。`DEALLOCATE PREPARE` 不保证清除共享计划缓存；释放并重新准备同一 SQL 不是强制刷新文件定义的接口。

## COPY 到 CSV

```sql
COPY (SELECT id, name FROM read_csv('/absolute/file_sql_data/input.csv') ORDER BY id)
TO '/absolute/file_sql_data/output.csv'
(FORMAT CSV, HEADER, NULLSTR 'NULL');

COPY my_table TO '/absolute/file_sql_data/table.csv';
```

支持普通表（含 schema 限定）及原生 SELECT 源。输出选项包括 `format csv`、`header` / `header true|false`、`delimiter` / `delim`、`quote`、`escape`、`nullstr` / `null_string`。重复选项（含同义名）会拒绝。COPY 仅输出 CSV；LOB 和 collection 值明确不支持。

- NULL、空字符串和与 NULL marker 同名的普通文本可区分；支持分隔符、引号、嵌入换行及日期时间精度的读回。
- 空结果在开启 header 时仍生成表头；`ROW_COUNT()` 反映写出的数据行数。
- **不覆盖目标**，包括普通文件、目录和悬空符号链接。
- 在持有的授权父目录 fd 下排他创建 0600 临时文件，从持有的 inode 经 `linkat` 排他发布；不以可被替换的临时路径作为发布内容来源。
- 失败、取消、写错误或发布前撤权时清理属于本次写入的临时 inode，不删除其他 writer 或攻击者替换的目录项。
- 保留原有 `SELECT INTO OUTFILE` 路径，不把它改造成 COPY。

## 资源限制

| 资源 | 上限 |
| --- | ---: |
| 单条 CSV/JSONL 记录 | 16 MiB |
| CSV 编码后的单条输出记录 | 16 MiB，含编码开销 |
| 列数 | 4096 |
| Parquet Footer、压缩页、解压页 | 分别 64 MiB |
| glob 匹配数 | 100000 |
| 单扫描器动态工作区 | 256 MiB |

多个扫描器和 COPY 的外部工作区汇总到查询级账本，并与原生查询 MemoryContext 一起接受 `query_memory_limit_percentage` 检查。分配前检查预算，失败回滚计费。长推断、I/O 和解码循环检查超时与取消；超限或取消返回原生错误，不返回截断结果。

这些是工程上限，不是每种输入均能达到的最低可用容量。嵌套容器、字符串扩容、公开容器复制及查询其他算子的内存也会消耗预算；查询限额可以先于单扫描器上限生效。

## 构建与测试

仅使用 CMake/CTest，构建并行度限制为 `-j3`：

```sh
cmake -S . -B build_file_sql_server \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFILE_SQL_GTEST_ROOT="$PWD/deps/3rd/usr/local/oceanbase/deps/devel"
cmake --build build_file_sql_server \
  --target seekdb test_file_table_op test_file_sql_parser test_file_scan_utils -j3
ctest --test-dir build_file_sql_server \
  -R '^file_(table_op|scan_utils|sql_parser)$' --output-on-failure
```

重建正在运行的二进制前，先停止使用该二进制的本轮专用实例。SQL 测试只连接专用实例；官方 `file_sql_basic` 的原版夹具安装/清理依赖实例 `host_ip()` 的免交互 SSH/SCP，不应修改脚本跳过。
