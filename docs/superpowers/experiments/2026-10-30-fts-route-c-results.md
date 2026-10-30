# Route C：归并数组访问小实验（2026-10-30）

## 决定：提交实验后 revert，保留测试

本机唯一一对完整 A/B 的查询类别改善 **-0.3354%**，接近零；类别等权综合为 -2.2486%，低于计划中的 2% 门槛。没有证据支持保留该微调，按约定提交再定向 revert，不额外重复跑分。分词也出现小幅回退，但生产改动不涉及分词，不能将该波动直接解释为代码因果。

## 补丁及边界

直接基线为保留 A+B 的 `964f20c92`。仅修改 `ObSRDaaTIterImpl::fill_merge_heap()`，每轮取得不会被子迭代器推进改变形状的数组视图，保留有效下标断言，用数组视图代替重复 at/operator[]。不跨调用缓存数组地址，因此 reuse/重扫重新取得当前存储。

源码核实 `ObIArrayWrap::at()` 为非虚 inline 访问；本候选只针对重复检查/访问，**并未减少 get_next_row/get_curr_score/get_curr_id 的虚调用**。不能再将折叠后的模板符号等同于独立昂贵虚调用。相关度乘法仍按原 `score * field_boost * dim_weight` 顺序，归并及加分顺序、filter evaluated flags、BM25、doc-ID、LIMIT/BMW 均未改变。

真正批量过滤需要把逐行过滤移动到候选批次暂存之后，重做过滤后紧凑投影、错误返回和表达式状态处理；不适合混进此处的小补丁。本轮未实现该后续方向，保留为独立设计/实验，不声称整个查询优化空间已穷尽。

## 复现条件

- 基线二进制 SHA256：`6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c`。
- 候选二进制 SHA256：`9a631bacce2e77c19676f98b2dfe0931c9411308c310b2c477f65d1f33579f74`。
- 两个全新实例目录 `run_temp/route-c-baseline-instance` / `run_temp/route-c-candidate-instance`，相同启动配置、相同回归序列。
- ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0；端口 12883；memory_budget=2G、datafile_size=1G、datafile_maxsize=4G、log_disk_size=2G。
- -j11 增量构建完成后再评测，无 profiler，原始 benchmark/评分/输入未改。

```bash
bash run_temp/run-fts-variant.sh route-c-baseline run_temp/route-c-baseline-seekdb
bash run_temp/run-fts-variant.sh route-c-candidate run_temp/route-c-candidate-seekdb
```

## 九项数据

| 指标 | 基线 | 候选 | 改善率 |
|---|---:|---:|---:|
| build_ik_all_sec | 5.749 | 5.646 | 1.7916% |
| build_ik_content_sec | 4.446 | 4.45 | -0.0900% |
| build_beng_en_sec | 2.45 | 2.445 | 0.2041% |
| tokenize_ik_avg_ms | 0.0621 | 0.0654 | -5.3140% |
| tokenize_beng_avg_ms | 0.0319 | 0.0347 | -8.7774% |
| query_cn_avg_ms | 1.4046 | 1.3681 | 2.5986% |
| query_beng_avg_ms | 1.8881 | 1.9575 | -3.6757% |
| query_mixed_avg_ms | 1.5073 | 1.4468 | 4.0138% |
| query_limit_avg_ms | 1.8231 | 1.9011 | -4.2784% |

构建类别 0.6352%；分词类别 -7.0457%；查询类别 -0.3354%。构建总时间 12.659 → 12.556 秒，不额外计入类别平均。

## 正确性证据

六组回归逐字节相同，十二个 diff 均为空；hits 均为 8001 / 11000 / 7332 / 20。新增 `unittest/storage/fts_query_batch_regression.sql/.result`，在改代码前捕获基线，再在候选对比：

- 独立生成 2051 篇文档，含 NULL 文本、不同词频、多字段 IK 和 BEng 索引。
- 单词/多词/重复词/布尔模式；相关度阈值；无命中/空/停用词查询。
- 按文档 ID 排序拼接完整英文结果的 ID:score，不仅检查 hit count；中文同时比较计数、ID 和、相关度总和及排序结果。
- 明确 ORDER BY 的 score tie、LIMIT/OFFSET、跨批量规模的完整消费与 EOF。
- SQL PREPARE/EXECUTE 的不同词数及空查询重用；索引后更新、删除、插入。

性能和测试产物为 `run_temp/route-c-*`。新增测试独立于生产候选提交，因此 revert 不删除证据。
