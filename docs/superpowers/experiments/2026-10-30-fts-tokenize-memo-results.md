# T0-SQL：worker-local TOKENIZE 最终结果复用

## 决定

**保留。** 编译和唯一一对完整原始 benchmark 均成功，两端 hits 均为 `8001 / 11000 / 7332 / 20`。IK TOKENIZE 改善 **36.1022%**，BEng 改善 **11.1111%**。用户已取消原 50% 幅度门槛，明确有改善即可保留，故不撤回，也不重跑。

本地复现结果用于暂留判断，官方 CI 待验证；未新增或运行额外功能回归、SQL/golden/单元测试、EXPLAIN 或 profiler。

## 版本与实现

- 起始 HEAD：`b3a0f242f`；生产源码为 Q0 撤回后的 A+B，与 `0da4ece92` 的 `src/` 一致。
- 仅修改 `src/sql/engine/expr/ob_expr_tokenize.cpp`；不改变 SQL 签名、parser、词典、输出格式和 benchmark。
- `parse_param()` 成功后查单项 thread-local 缓存；原参数解析、属性规范化和 IK 词典加载照常执行。
- key 包括全文原始字节/长度、含版本 parser 名、规范化 properties、output mode、charset、collation，以及结果 LOB header 格式。
- 单条 key 和结果字节合计上限 1 MiB。只有原 `pack_json_res()` 成功后才发布完整 datum 和深拷贝 payload；缓存分配失败只放弃发布，不改变成功查询的结果。
- 按用户批准的所有权修正，命中时复制 payload 到表达式自己的结果内存，复制完整 datum 并修正指针；后续缓存淘汰不影响已返回的表达式结果。

| 二进制 | SHA256 |
|---|---|
| `run_temp/score-t0-baseline-seekdb`（实际重建的 A+B） | `0b780889174f8fd185d86b774bb744a58e7716e06457da8bc5ec3fc04e05434d` |
| `run_temp/score-t0-seekdb` | `c42d873c549ccf0aaa8310aa38f2f49538d1fc959a892c35473211e00933accb` |

构建：`cmake --build build_release --target seekdb -j11`；成功日志：`run_temp/score-t0-build.log`。

## 唯一完整 A/B

```bash
bash run_temp/run-fts-score-route.sh score-t0-baseline run_temp/score-t0-baseline-seekdb
bash run_temp/run-fts-score-route.sh score-t0-candidate run_temp/score-t0-seekdb
```

两端新实例，port=12883，memory_budget=2G，datafile_size=1G，datafile_maxsize=4G，log_disk_size=2G。原始 benchmark 未修改，固定 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。没有与编译或 profiler 并行。

| 指标 | 直接基线 | T0 | 改善 |
|---|---:|---:|---:|
| build_ik_all_sec | 5.967 | 5.862 | +1.7597% |
| build_ik_content_sec | 4.459 | 4.447 | +0.2691% |
| build_beng_en_sec | 2.442 | 2.445 | −0.1229% |
| tokenize_ik_avg_ms | 0.0626 | 0.0400 | **+36.1022%** |
| tokenize_beng_avg_ms | 0.0342 | 0.0304 | **+11.1111%** |
| query_cn_avg_ms | 1.3835 | 1.3456 | +2.7394% |
| query_beng_avg_ms | 1.9930 | 1.9455 | +2.3833% |
| query_mixed_avg_ms | 1.5281 | 1.5137 | +0.9423% |
| query_limit_avg_ms | 1.8748 | 1.8495 | +1.3495% |

类别内算术平均后，构建 **+0.6353%**、分词 **+23.6067%**、查询 **+1.8536%**，三类别等权综合 **+8.6985%**。这是一次配对结果，不把构建/查询的小幅变化归因为 TOKENIZE 缓存，也不与后续阶段改善率相加。

原始评分脚本相对仓库自带 CI baseline 的两端 `score` 均为 **100.0**，已经饱和；这不是官方 CI 成绩，不能声称此处分数从 100 继续提高。实际去留依据为上述直接 A/B 的目标指标改善。

## 证据

- `run_temp/score-t0-binaries.sha256`
- `run_temp/score-t0-{baseline,candidate}-report.txt`
- `run_temp/score-t0-{baseline,candidate}-benchmark.log`
- `run_temp/score-t0-{baseline,candidate}-score.json`
- `run_temp/score-t0-comparison.json`

服务已停止，实例 store 已删除，日志/配置和报告保留。下一候选的直接基线为 A+B+T0。
