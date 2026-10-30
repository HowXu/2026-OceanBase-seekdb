# Q0 / T0 / B0 赛题路线最终结果

## 最终状态

按用户确认的本地原始 benchmark 验收及“有改善即可保留”口径完成本轮：

| 候选 | 结果 | 生产提交 / 撤回 | 一对直接 A/B 的主要结果 |
|---|---|---|---|
| Q0：纯命中免评分 | 失败、已撤回 | `b98b05694` / `b257d0682` | 候选在第一条 MATCH 查询段错误，报告不完整，无性能结论 |
| T0-SQL：最终 TOKENIZE 结果复用 | 保留 | `d21e6ab23` | IK +36.1022%，BEng +11.1111%；综合 +8.6985% |
| B0：FTS 临时排序 NONE | 保留 | `f65055696` | 构建类别 +1.5457%；mixed −7.7497%、LIMIT −1.5331% 均披露 |
| IK 原始 token 序列共享缓存 | 暂保留 | `8f22f1710` | 综合 +4.0239%，但首次 IK −1.6877%、content −2.2022%，未证明预期 content 构建收益 |

最终生产源码是 A+B+T0-SQL+B0-NONE+IK token 序列缓存，没有 Q0，也没有历史 C1/C2/C3 临时代码。没有子代理、worktree、其他分支检查、合并或 push。

## 最终重建与唯一最终验收

生产源码 HEAD：`8f22f171079b546d2a72634611fe864d53d989e9`。

```bash
cmake --build build_release --target seekdb -j11
bash run_temp/run-fts-score-route.sh score-routes-final run_temp/score-routes-final-seekdb
python3 tools/benchmark/fts_large_bench_score.py run_temp/score-routes-final-report.txt --json
```

编译成功，原始 benchmark 完整成功；编译/benchmark/profiler 不并行。新实例 port=12883，memory_budget=2G，datafile_size=1G，datafile_maxsize=4G，log_disk_size=2G。

固定 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`；所有分词/查询原始 3 个 sample 和完整九项报告均保留，没有丢弃不利样本或追加 A/B。

最终二进制：`run_temp/score-routes-final-seekdb` 和 `build_release/src/observer/seekdb`。

SHA256：`e45cf6d49e92efae40469909dde4086c0b814b0200c5b03b164e6c71948f31bd`。

## 最终九项与本轮起点

本轮起点采用 T0 直接 A/B 的 A+B 报告 `run_temp/score-t0-baseline-report.txt`；其实际重建二进制 SHA256 为 `0b780889174f8fd185d86b774bb744a58e7716e06457da8bc5ec3fc04e05434d`。

**下表是最终单次验收与较早起点的比较，不是新增的同期 A/B，不是官方 CI 成绩，也不能证明所有差异都由补丁稳定造成。** 各阶段百分比不可相加；本表直接由两份完整报告计算。

| 指标 | 本轮起点 A+B | 最终验收 | 改善 |
|---|---:|---:|---:|
| build_ik_all_sec | 5.967 | 6.069 | **−1.7094%** |
| build_ik_content_sec | 4.459 | 4.552 | **−2.0857%** |
| build_beng_en_sec | 2.442 | 2.441 | +0.0410% |
| tokenize_ik_avg_ms | 0.0626 | 0.0396 | **+36.7412%** |
| tokenize_beng_avg_ms | 0.0342 | 0.0285 | **+16.6667%** |
| query_cn_avg_ms | 1.3835 | 1.4122 | **−2.0744%** |
| query_beng_avg_ms | 1.9930 | 2.1148 | **−6.1114%** |
| query_mixed_avg_ms | 1.5281 | 1.5324 | **−0.2814%** |
| query_limit_avg_ms | 1.8748 | 1.8225 | +2.7896% |

类内平均、再三类等权：

- 构建：**−1.2514%**；
- 分词：**+26.7039%**；
- 查询：**−1.4194%**；
- 综合：**+8.0111%**。

两份报告 hits 均为 **8001 / 11000 / 7332 / 20**。最明确的目标收益来自 T0-SQL；不能将本轮描述为构建与查询全面改善。IK 共享缓存的预期跨 DDL 构建收益没有得到本轮测量支持，按用户放宽后的口径暂保留，不隐瞒负项。

## 评分解释

未经修改的 `fts_large_bench_score.py` 对仓库自带 CI baseline 计算，本轮完整报告和最终报告的本地 score 均为 **100.0**，已经饱和。因此：

- 不能声称官方分数提高了 8.0111 分；
- 不能将本地 100 当成官方 CI 100；
- +8.0111% 是相对本轮 A+B 起点的三类别等权相对改善，不是评分脚本对其 CI baseline 的 mean_improvement。

官方 CI 成绩仍待验证；本轮没有修改评分规则或任何 baseline。

## 验收边界与证据

遵照用户限定，**没有新增或运行额外功能回归、六组历史回归、单元测试、SQL/golden、EXPLAIN 或 profiler**。验证范围只覆盖编译和原始 benchmark；不声称已经完成广泛语义、并发或异常路径验证。

- 计划：`docs/superpowers/plans/2026-10-30-fts-competition-score-routes.md`。
- 分阶段记录：同目录 `2026-10-30-fts-q0-results.md`、`2026-10-30-fts-tokenize-memo-results.md`、`2026-10-30-fts-sort-none-results.md`、`2026-10-30-fts-token-stream-reuse-results.md`。
- 最终构建：`run_temp/score-routes-final-build.log`。
- 最终原始报告/日志：`run_temp/score-routes-final-report.txt`、`score-routes-final-benchmark.log`、`score-routes-final-wrapper.log`。
- 最终评分/起点比较：`run_temp/score-routes-final-score.json`、`score-routes-final-vs-start.json`。
- 二进制/源码记录：`run_temp/score-routes-final-binaries.sha256`、`score-routes-final-source-head.txt`。

各实例已停止，包装器仅删除本轮新建实例的可重建 store，保留日志和配置；参考二进制、各候选及哈希、增量构建缓存均保留。benchmark 目录和历史 expected answers 未修改。最终仅追加结果文档/计划进度，不修改已验收的生产源码。
