# Q0：纯 MATCH 命中免评分实验

## 决定

**失败，撤回；不重跑本候选 A/B。** 编译通过，但候选原始 benchmark 在第一条 MATCH 查询时发生 seekdb 段错误，客户端报告 `ERROR 2013 ... Lost connection to MySQL server during query`，未生成完整九项报告。未证明性能收益或根因，不将构建/分词阶段的局部数据作为保留依据。

本地原始 benchmark 用于候选暂留/撤回；官方 CI 成绩待验证。按用户指定，未新增或运行额外 SQL 回归、golden、单元测试、EXPLAIN 或 profiler。

## 版本与二进制

- 起始生产 HEAD：`0da4ece92`（A+B）；计划提交：`c6d174b2f`。
- 基线：`run_temp/fts-clean-base-final-seekdb`。
  SHA256：`6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c`。
- 候选：上述源码加 Q0 补丁，`run_temp/score-q0-seekdb`。
  SHA256：`c4114d810964566c87df54dbb29900fb79b5136e7b486da5bd8dbab3229ede6f`。
- 生产改动为消费者分析、无评分 ctdef/projection/collector 常量；保守保留有 LIMIT 语句的评分。无评分 child 使用实际分配的 inverted scan，兼顾 function-lookup 分支。BM25 本体未修改。
- 构建：`cmake --build build_release --target seekdb -j11`；最终成功日志：`run_temp/score-q0-build-final.log`。首次编译遇到新接口误放 private 的访问错误，移至 public 后编译通过；该修正发生在唯一 A/B 开始前。

## 唯一 A/B

```bash
bash run_temp/run-fts-score-route.sh score-q0-baseline run_temp/fts-clean-base-final-seekdb
bash run_temp/run-fts-score-route.sh score-q0-candidate run_temp/score-q0-seekdb
```

两端各使用全新实例，port=12883，memory_budget=2G，datafile_size=1G，datafile_maxsize=4G，log_disk_size=2G。原始脚本未改；`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。构建与 benchmark 不并行。

| 指标 | A+B 基线 | Q0 候选 |
|---|---:|---:|
| build_ik_all_sec | 6.065 | 6.069（未完成 benchmark 的原始阶段输出） |
| build_ik_content_sec | 4.550 | 4.643（同上） |
| build_beng_en_sec | 2.443 | 2.453（同上） |
| tokenize_ik_avg_ms | 0.0648 | N/A：无完整报告 |
| tokenize_beng_avg_ms | 0.0327 | N/A：无完整报告 |
| query_cn_avg_ms | 1.3564 | N/A：此处崩溃 |
| query_beng_avg_ms | 1.9408 | N/A |
| query_mixed_avg_ms | 1.4636 | N/A |
| query_limit_avg_ms | 1.8621 | N/A |

基线 hits：`8001 / 11000 / 7332 / 20`；候选未取得查询 hits。候选类别、综合改善和分数均为 N/A，不能补零或估算。

原始评分脚本对基线报告的输出：`score=100.0`、`mean_improvement=0.8877308932811668`（相对仓库自带 CI baseline，已触及评分上限）。这是本地复现输出，**不是当前官方 CI 成绩，也不是相对 A+B 的增益**。

## 证据与后续

- `run_temp/score-q0-binaries.sha256`
- `run_temp/score-q0-baseline-report.txt`、`score-q0-baseline-score.json`
- `run_temp/score-q0-{baseline,candidate}-benchmark.log`
- `run_temp/score-q0-{baseline,candidate}-wrapper.log`
- `run_temp/score-q0-{baseline,candidate}-instance/log/` 和配置保留；停止后仅删除可重建的 store。

实验补丁独立提交后定向 revert，证据文档保留；Task 2 的直接基线恢复为 A+B，不包含 Q0。
