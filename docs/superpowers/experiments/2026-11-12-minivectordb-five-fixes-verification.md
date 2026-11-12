# MiniVectorDB 五项修复验证记录

日期：2026-11-12。实现基线为 c4a54eb 的当前工作树，未提交、未推送。

## 结论

五项问题均已实现并经过本机及实际 Docker 验证：

1. ORDER BY 序号按展开后的投影进行 1-based 绑定；0、越界和超大位置报错，负序号不再静默当常量。
2. 表/列/索引/别名采用 ASCII 大小写不敏感查找，保留原始显示拼写；支持表级单列主键、匹配的单表限定名/别名及冗余括号。
3. 向量接受已通过基准的十进制格式，先转 double 再转有限 float32；允许有限下溢及负零，继续拒绝非有限值和溢出。
4. 无 AS 表达式列名保留原始 SQL 文本；星号仍使用 Schema 原始列名，显式 AS 优先。
5. 表改为一个基础段加最多64个增量段，DML 只写变更；下一次次实际修改触发合并。RootStore 只扫描/同步新段，CURRENT 原子发布、事务和 GC 保护语义保留。

新 Catalog 为 MVDBC003，增量段为 MVDBD001。按已批准设计，不承诺读取旧格式数据卷。

## RED → GREEN 证据

证据目录：`.superpowers/sdd/five-fixes/`。

| 任务 | RED | GREEN |
|---|---|---|
| 序号/括号/显示文本 | 12 failed,105 passed：错误顺序、无效序号成功、Paren 拒绝 | 117 passed |
| 名称/表级主键 | 2 failed,118 passed | 124 passed（含 exact search） |
| 向量转换 | C++ types aborted；Python 1 failed,62 passed | C++ types PASS；67 Python passed |
| 段格式/对象列表 | 新 table_segments.cpp/接口不存在，CMake 明确失败 | 11 CTest、184 pytest passed |
| 增量 DML/发布 | 64 UPDATE 后仍单整表对象；无匹配 UPDATE 更换对象 | 定向90 pytest、11 CTest；完整188 pytest passed |

保存了 `task1-red.log` 至 `task5-red.log` 及对应 build/green 日志，未以事后测试代替 RED。

三处测试自身的独立预期在验证中被纠正并保留说明：

- `ORDER BY 2 DESC` 应按 id=3,2,1 排列，初稿第二行预期写错；与已通过参考响应核对后修正。
- 两个 BIGINT 列中任选一个作主键均合法；非 BIGINT 主键反例改用 VARCHAR。
- 初始 INSERT 已占一个 delta，第64次 UPDATE 会触发合并；纯追加测试改为63次，另由70次修改测试跨合并。

旧断言 `[+1,0]`、`[01,0]` 必须拒绝已按本轮设计撤回，合法路径另测。MVDBC001/002 读取兼容断言按明确不承担旧卷兼容的设计移除，不是为隐藏实现错误而删断言。

## 原公开驱动与 Docker

干净 `--no-cache` 构建日志：`final-docker-build.log`。

```text
image: sha256:58cd76b9a7a4998c71060f24f535b2331d8be9c39e4273ee7d4d070642a9aed0
entrypoint: ["/opt/venv/bin/python","-m","minivectordb"]
production fault injection: OFF
```

实际运行：

- handshake：PASS；
- sql_crud：PASS；
- 原始公开 20/20 case、95 SQL：PASS；
- 未修改 STDIO_PROTOCOL.md、protocol_check、mysqltest、配置或断言。

日志：`final-handshake.log`、`final-crud.log`、`final-public.log`。

## 与 b410e23 的定向差分

`differential/` 保存53项原始响应和独立预期。当前实现为37/53，参考为52/53；不能只看这个总数判断本轮范围。

与原 c4a54eb 比较，五个目标组共24项“参考符合独立预期、我们不符合”的差异已全部符合预期。`differential-target-summary.json` 列出24项名称及断言。

剩余17项不同包括：

- 自设 `id AS x,n AS X ORDER BY x` 别名歧义预期：参考成功而我们按已批准设计报歧义；该项不是题面已冻结预期。
- NOT NULL 拼写、BIGINT 显示宽度、IF NOT EXISTS、SELECT 1、浮点标量、OFFSET、WORK 后缀、SET、反向距离参数、向量构造器、<> 等明确未纳入本轮的额外语法。

默认列名另有4项实际 Docker 精确检查，覆盖小写函数、逗号空格、括号和显式 AS：`metadata.json`，全部 PASS。

## 写入成本

同一8,000行、128维、64次单行 UPDATE、2 CPU/2 GiB、只读根、禁网诊断：

| 指标 | 原 c4a54eb | 本次工作树 |
|---|---:|---:|
| UPDATE 阶段设备写入 | 312,475,648 B | **5,656,576 B** |
| 最终数据文件 | 4,872,325 B | 4,877,644 B |
| 数据、exact/ann、强杀恢复 | PASS | PASS |
| oom / oom_kill | 0 / 0 | 0 / 0 |

写入减少 **98.1898%**，旧值约为新值 **55.24倍**；验收阈值是至少减少90%（新值小于31,247,565 B）。本次包含第64次 UPDATE 触发的同步段合并，不是只对永不合并的人造路径测量。

结果：`storage/storage-amplification.json`；摘要：`storage-summary.json`。耗时包含 docker exec 采样，不作为正式性能成绩。

## 超内存与中断恢复

同一自生成资源诊断，公开2 CPU/2 GiB/no-swap/128 PID、只读根、禁网：

- 33批 × 64行 × 1 MiB VARCHAR，共2,112 MiB payload；
- 数据文件2,214,701,020 B，大于2,147,483,648 B内存限制；
- 完整 ID 多重集、1 MiB内容样本、filtered ANN、PK顺序均通过；
- 两次实际 KILL 后恢复通过；观察者确认 ready 前打开表段，并在 ready 前再次 KILL，随后恢复成功；
- oom=0，oom_kill=0；
- 总耗时47.3965秒；累计设备写入2,215,014,400 B，低于自设48 GiB预算和600秒预算；
- 容器与卷清理完成。

原始结果：`resource/resource-probe-2g.json`、trace 和 stderr；摘要：`resource-summary.json`。这是自生成诊断，不是正式隐藏规模或正式资源准入证明。

## 最终边界

- FlatScan 的 ANN exact fallback 未改成近似索引；本轮目标不是性能排名。
- 未增加 NULL、JOIN、聚合、子查询、SELECT 1、OFFSET、DROP 等额外功能。
- 64段是同步确定性合并阈值；无后台线程。单次大输入、无界最终结果和外部排序仍有既有资源边界。
- 本地公开、差分、故障和资源探针通过不等于正式29项或隐藏资源型用例通过。
- 未 stage、commit、push，也未修改官方公开材料。
