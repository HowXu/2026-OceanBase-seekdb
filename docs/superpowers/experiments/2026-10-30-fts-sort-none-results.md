# B0：FTS 临时排序 NONE 压缩实验

## 决定

**保留。** 编译及唯一一对完整原始 benchmark 成功，两端 hits 为 `8001 / 11000 / 7332 / 20`。三项构建均改善，构建类别平均 **+1.5457%**。按用户追加的“有改善就留”规则，不要求原 5% 门槛，不重跑。

本地复现用于候选暂留，官方 CI 待验证；未新增或运行额外功能回归、SQL/golden/单元测试、EXPLAIN 或 profiler。

## 版本与实现

- 直接生产基线：`d21e6ab23`，A+B+T0-SQL。
- 仅修改 `src/sql/code_generator/ob_static_engine_cg.cpp` 的 `fill_compress_type()`。
- 在已有 online-DDL 调用范围内，仅当排序键为 `T_REF_COLUMN` 且 `is_word_segment_column()` 时保留 `NONE_COMPRESSOR`；其他排序沿用原 `ObDDLUtil::get_temp_store_compress_type()`。
- 不修改编码排序键、AQS、持久化压缩、checksum 或 prescan/compact-format 逻辑；不声称消除了 compact read/write。

| 二进制 | SHA256 |
|---|---|
| `run_temp/score-t0-seekdb` | `c42d873c549ccf0aaa8310aa38f2f49538d1fc959a892c35473211e00933accb` |
| `run_temp/score-b0-seekdb` | `7ba0eb2792195831a682b8250c5f6c9466c02d8d562fb3fbb032448d0274fcfc` |

构建成功日志：`run_temp/score-b0-build.log`，最多 `-j11`，不与 benchmark 并行。

## 唯一完整 A/B

```bash
bash run_temp/run-fts-score-route.sh score-b0-baseline run_temp/score-t0-seekdb
bash run_temp/run-fts-score-route.sh score-b0-candidate run_temp/score-b0-seekdb
```

两端全新实例，port=12883，memory_budget=2G，datafile_size=1G，datafile_maxsize=4G，log_disk_size=2G。原始 benchmark 未修改，固定 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。

| 指标 | 直接基线 | B0 | 改善 |
|---|---:|---:|---:|
| build_ik_all_sec | 6.067 | 5.961 | +1.7472% |
| build_ik_content_sec | 4.553 | 4.440 | +2.4819% |
| build_beng_en_sec | 2.451 | 2.441 | +0.4080% |
| tokenize_ik_avg_ms | 0.0385 | 0.0361 | +6.2338% |
| tokenize_beng_avg_ms | 0.0302 | 0.0282 | +6.6225% |
| query_cn_avg_ms | 1.4904 | 1.3607 | +8.7024% |
| query_beng_avg_ms | 2.0404 | 1.9919 | +2.3770% |
| query_mixed_avg_ms | 1.4878 | 1.6031 | **−7.7497%** |
| query_limit_avg_ms | 1.8655 | 1.8941 | **−1.5331%** |

类别平均：构建 **+1.5457%**、分词 **+6.4281%**、查询 **+0.4491%**，三类别等权综合 **+2.8077%**。单对结果不能证明所有小幅变化稳定；该生产补丁只作用于 DDL，分词/查询变化不归因为排序压缩收益，负项不隐瞒、不重跑挑数，也不与 T0 的百分比相加。

原始评分脚本对仓库 CI baseline 的两端本地 score 均为 **100.0**（已经饱和），不是官方 CI 成绩。

## 证据

`run_temp/score-b0-binaries.sha256`、`score-b0-{baseline,candidate}-report.txt`、对应 `-benchmark.log`、`-score.json`、`score-b0-comparison.json`。服务已退出，实例 store 已删除，日志/配置保留。下一候选基线为 A+B+T0+B0。
