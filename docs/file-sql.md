# 文件即数据库（File as Database）

本功能让 seekdb 直接把本地文件当作表来查询，无需预先建表或导入数据。

```sql
SELECT id, name, score FROM read_csv('/data/orders.csv');
SELECT * FROM read_json('/data/events.jsonl') WHERE day = '2026-08-02';
SELECT c.id, p.name FROM read_csv('/data/orders.csv') c
  JOIN read_parquet('/data/customers.parquet') p ON c.id = p.id;
```

## 开关与安全

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `enable_file_sql` | `OFF` | 功能开关，关闭时任何文件函数都返回 `ER_NOT_SUPPORTED_YET` (1235) |
| `secure_file_priv` | `/` | 允许访问的目录，文件路径必须落在该目录内 |

规则：

- 功能默认关闭，需显式 `SET SESSION enable_file_sql = ON` 才能使用。
- 所有路径都要经过 `secure_file_priv` 校验：路径先做 `realpath` 规范化（含符号链接解析），再要求其位于 `secure_file_priv` 目录之下；越界一律拒绝。
- 文件只读，不会被修改；导出的临时文件与目标文件同目录，成功后原子改名，失败时清理临时文件。

## 读取函数

| 函数 | 说明 |
| --- | --- |
| `read_csv(path, ...)` | 读取 CSV |
| `read_json(path, ...)` / `read_ndjson(path, ...)` | 读取 JSON Lines（扁平对象） |
| `read_parquet(path, ...)` | 读取 Parquet |
| `glob(pattern)` | 按通配符列出文件，返回一列 `file` |

### 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `header` | `true` | CSV 是否带表头；为 `false` 时列名为 `column0`、`column1` … |
| `delimiter` | `,` | CSV 字段分隔符 |
| `quote` | `"` | CSV 引用字符 |
| `escape` | `"` | CSV 转义字符；与 `quote` 相同时表示双写转义 |
| `null_string` | 空串 | 未加引号且等于该值的字段视为 NULL |
| `sample_size` | `-1` | 仅支持 `-1`，即全量推断模式 |
| `format` | 按扩展名 | `csv` / `json` / `parquet` |

CSV 读取支持 UTF-8、BOM、LF/CRLF、引号与转义、字段内换行、重复列名自动加后缀（`id`、`id__2`）。
JSON Lines 读取支持字段并集、缺失字段补 NULL、大小写不敏感的重名检查；嵌套对象与数组会被拒绝。
Parquet 读取支持 Footer Schema、多 Row Group、可空列、字典编码与 Snappy 压缩、列裁剪。

## 类型推断

推断出七种类型：`BIGINT`、`DOUBLE`、`BOOLEAN`、`VARCHAR`、`DATE`、`DATETIME`、`NULL`。

- 整数字面量超出 BIGINT 范围时推断为 `VARCHAR`；`1e20` 推断为 `DOUBLE`。
- `true`/`false`（不区分大小写）推断为 `BOOLEAN`。
- 合法的 `YYYY-MM-DD` 推断为 `DATE`；`YYYY-MM-DD` + 时间（`T` 或空格分隔）推断为 `DATETIME`，并保留小数秒精度。
- 非法日期（如 `2023-02-29`）与非法时间（如 `24:00:00`）推断为 `VARCHAR`。
- 列内出现多种类型时按规则宽化：`BIGINT` + `DOUBLE` → `DOUBLE`，`DATE` + `DATETIME` → `DATETIME`，其余组合 → `VARCHAR`；`NULL` 不参与宽化。
- 整列都是 NULL 时该列类型为 `NULL`。

## 查看结构

```sql
DESCRIBE SELECT * FROM read_csv('/data/orders.csv');
```

输出 `column_name`、`column_type`、`null`、`key`、`default`、`extra` 六列。

## 导出

```sql
COPY (SELECT id, name FROM read_csv('/data/orders.csv')) TO '/data/out.csv'
  (format csv, header, nullstr 'NULL');
COPY ordinary_table TO '/data/out.csv' (delimiter ';', quote '"', escape '"', header false);
```

- 导出的文件可以被 `read_csv` 读回。
- 目标文件已存在时导出失败。
- 导出先写同目录下的临时文件，成功后原子改名；查询失败时临时文件被清理，不会残留。

## 实现结构

| 文件 | 职责 |
| --- | --- |
| `src/sql/engine/basic/ob_file_scan_utils.{h,cpp}` | CSV/JSON 解析、类型推断、路径校验、扫描与导出 |
| `src/sql/engine/basic/ob_file_parquet_scan.{h,cpp}` | Parquet 读取（thrift compact、页编码、Snappy） |
| `src/sql/engine/basic/ob_file_table_op.{h,cpp}` | 把文件里的行变成算子的数据行 |
| `src/sql/optimizer/ob_log_file_table.{h,cpp}` | 文件表的逻辑算子 |
| `src/sql/resolver/dml/ob_file_table_resolver.cpp` | 文件表的解析：推断列结构、生成列元数据 |
| `src/sql/parser/sql_parser_mysql_mode.y` | `read_csv` 等函数在 FROM 子句中的语法、命名参数、`COPY` 语句 |
| `unittest/sql/parser/test_file_sql_parser.cpp` | 语法单元测试 |
| `unittest/sql/engine/basic/test_file_scan_utils.cpp` | 扫描、推断、安全与导出的单元测试 |
| `tools/deploy/mysql_test/test_suite/file_sql/` | mysqltest 用例 |

## 现状

已经实现并有测试覆盖：

- CSV、JSON Lines、Parquet 的读取，列结构推断，行流式返回
- 类型推断的七种类型、宽化规则、数值边界与非法日期
- `enable_file_sql` 开关与 `secure_file_priv` 校验，符号链接逃逸的拦截，文件被改写时的报错
- `read_csv`、`read_json`、`read_ndjson`、`read_parquet`、`glob` 在 FROM 子句中的语法，命名参数，`COPY` 语句的语法
- 算子与优化器路径：文件表可以参与过滤、聚合、排序与 JOIN

尚未实现：

- `DESCRIBE SELECT * FROM read_csv(...)` 目前返回的是执行计划，尚未返回列元数据
- `COPY` 的导出尚未接到执行器上，语句可以解析但不会写出文件

这两项都在计划算子与执行器一层，需要在 `ObLogSelectInto` 的旁边增加一条写入 CSV 的路径，
并把文件表解析出来的列元数据交给 `ObExplainResolver`。

