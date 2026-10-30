# 查询 C1：批次过滤实验结果（2026-10-30）

## 决定：COUNT 达标，但 LIMIT 回退，提交后撤回

唯一一对完整 A/B 中，前三条 COUNT 查询的改善率均值为 **+2.7144%**，达到该分项目标；查询类别仅为 **+0.5656%**。LIMIT 从 1.7786 ms 增至 1.8832 ms，回退 **5.8810%**，不满足计划中的“LIMIT 没有未解释的明显回退”条件，故不保留 C1 生产补丁。

LIMIT 快路径条件没有启用，但本轮 `do_one_merge_round` 增加了 defer_filter 分支；没有新 profile 证明此次回退的具体原因，也不能把它直接归为环境波动。按事先约定的单一 A/B 和门槛撤回，不追加跑分寻找有利结果。

## 实现与机制证据

- 仅向量化、有 score filter 且无有效 LIMIT/OFFSET 时使用批次过滤；其他路径保留原逐行处理。
- raw candidate 完整归并/评分后暂存，不累加输入/输出统计；只有过滤通过项累加，保持原计数语义。
- 现有 `ObExpr::eval_batch`、全零 skip 和连续 evaluated flags 用于过滤；按原 NULL/整数零判断稳定紧凑化。
- 临时诊断观察到 9 个相关 iterator、12,510 个 raw candidate、147 次批次过滤。通用 batch 回退仍可能逐行调用布尔函数，未声称数学计算完全向量化。
- 临时计数和日志在正式候选前已全部移除，`ob_expr_bool.cpp` 最终无改动。

## 版本、正确性和复现

- 直接基线：保留 A+B 的 `ae5c8f0df`（生产代码同 `964f20c92`）。
- 基线二进制 SHA256：`6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c`。
- 候选二进制 SHA256：`e873dddf0c9aae2fc6030656e44be560c9632e3852d1c8644873369c90135d61`。
- 编译最多 `-j11`；正式 A/B 无 profiler/临时计数，原 benchmark 与评分脚本未修改。
- 固定配置：ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0；端口 12883；memory_budget=2G、datafile_size=1G、datafile_maxsize=4G、log_disk_size=2G。

```bash
bash run_temp/run-fts-variant.sh query-c1-baseline run_temp/query-c1-baseline-seekdb
bash run_temp/run-fts-variant.sh query-c1-candidate run_temp/query-c1-candidate-seekdb
```

两端六组回归 diff 均为空，hits 均为 8001 / 11000 / 7332 / 20。扩展 golden 在改代码前的 A+B 基线运行两次且输出一致，覆盖全拒绝、阈值、完整 ID/相关度、重扫和 DML。

## 九项原始结果

| 指标 | 基线 | C1 候选 | 改善率 |
|---|---:|---:|---:|
| build_ik_all_sec | 5.742 | 5.643 | +1.7241% |
| build_ik_content_sec | 4.448 | 4.844 | -8.9029% |
| build_beng_en_sec | 2.444 | 2.543 | -4.0507% |
| tokenize_ik_avg_ms | 0.0682 | 0.0651 | +4.5455% |
| tokenize_beng_avg_ms | 0.0374 | 0.0344 | +8.0214% |
| query_cn_avg_ms | 1.2763 | 1.2540 | +1.7472% |
| query_beng_avg_ms | 1.8658 | 1.8507 | +0.8093% |
| query_mixed_avg_ms | 1.5125 | 1.4280 | +5.5868% |
| query_limit_avg_ms | 1.7786 | 1.8832 | -5.8810% |

构建类别 -3.7432%，分词类别 +6.2834%，查询类别 +0.5656%，三类等权综合 +1.0353%。这些未改动类别的结果全部保留，不用于掩盖 LIMIT 回退。

LIMIT 三样本：基线 1.8676 / 1.7052 / 1.7629 ms；候选 1.9213 / 1.8288 / 1.8995 ms。该数据是撤回决定的一部分，不删除或选择性忽略。

## 后续

C1 的测试和本记录独立提交，生产候选单独提交后定向 revert。随后按用户要求继续 C2，基线恢复为原 A+B 最终程序；不把 C1 的 COUNT 改善带入 C2 的收益计算。
