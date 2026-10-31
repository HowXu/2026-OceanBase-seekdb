# GPU BruteForce 快照去重与查询资源缓存设计

## 目标

在不改变 CAGRA/IVF-Flat 默认算法参数、不引入持久查询线程的前提下，削减两类确定性开销：

1. BruteForce 快照重复保存原始向量，以及不必要的 hipVS graph 序列化和重载。
2. 每次 GPU 查询重复创建 search params、分配和释放设备/主机输出缓冲。

当前没有可用远程 GPU。离线测试只能证明接口合同、资源生命周期和快照行为，不能宣称真实 QPS 或构建性能提升。最终收益必须在相同 V5 基线和 250K 数据上复测。

## 方案选择

采用“BF 快照去重 + 查询资源缓存”。只做查询缓存会保留快照双写；同时引入持久查询线程则增加销毁、并发和 fallback 风险，本轮不做。IVF 的 `n_lists/n_probes` 需要真实 Recall/QPS 校准，本轮不调整。

## BruteForce 快照去重

### 格式与写入

沿用 OBCUVS02 的外层头、IDs 和原始向量布局。对 BruteForce：

- 不调用 `cuvsBruteForceSerialize()`，不创建或复制内层 graph 临时文件。
- 外层 `graph_bytes` 写为零，表示加载时应从外层原始向量重建 GPU 索引。
- IDs 和 `buf_vecs_` 仍各写一次，保留后续增量、 freshness 判断和 CPU fallback 所需数据。
- 若 BF 尚未构建或已落后于外层数据，仍先通过现有大栈路径构建，保证快照和后续 GPU 查询使用同一完整数据集。

CAGRA、IVF-Flat 和已有的非零 graph BF 快照保持原路径。`graph_bytes=0` 是新的 BF 原始向量快照标记；不要求旧二进制读取这种实验快照。

### 加载与故障处理

读取 OBCUVS02 时允许 `graph_bytes=0`。IDs 和向量读取完整后：

- GPU 可用时，通过现有 32 MiB 大栈构建路径调用 `seekdb_cuvs_build()`。
- 非零 graph 仍复制到临时文件并调用 `seekdb_cuvs_deserialize()`。
- GPU 不可用、构建失败或加载失败时，继续将外层 IDs/向量交给 VSAG fallback，不丢失数据。
- 构建和加载仍要求使用相同的 `SEEKDB_CUVS_ALGO` 环境；不把零 graph 快照解释为跨算法格式。

bridge 提供一个小型只读判断接口，让 adaptor 区分 BF 原始向量快照与需要 graph 的算法；非 CUVS 构建的 stub 始终选择原路径。现有 build/search/free ABI 不变。

## 查询资源缓存

在 `sk_cuvs_index` 内增加以下资源及容量：

- query、neighbor IDs、distances 设备缓冲；
- BF/IVF 所需的 host int64 IDs 缓冲；
- CAGRA 和 IVF-Flat search params。

资源采用首次使用时创建、按需扩容：

- 所需字节数不超过已分配容量时直接复用。
- 容量不足时先分配新缓冲，成功后释放旧缓冲并更新容量；分配失败保留旧资源并返回搜索失败。
- host int64 缓冲使用安全的临时 `realloc` 指针，失败不覆盖原指针。
- CAGRA 每次搜索仍按 `topk/dim` 更新 `itopk_size` 和算法；IVF 每次设置 `n_probes`。
- 输入、搜索、输出拷贝和 stream sync 顺序不变。搜索结果仍转换为现有 uint32 ABI，距离和外部 ID 映射不变。
- handle 销毁前先按现有逻辑 drain stream，再销毁 params、索引和全部缓存，最后销毁 resources。
- 不改变外层线程模型和同句柄串行调用约束。缓存只延长已有查询资源的生命周期，不改变单次搜索峰值量级。

## 验证

1. 扩展 mock 的分配和 params 计数，覆盖连续 Top-10/100、相同/较小请求复用、较大请求扩容、故障注入和最终释放。
2. 扩展 adaptor 快照测试，验证 BF 不写内层 graph、base 只写一次、零 graph 重载通过 build 恢复；非 BF 和旧非零 graph 路径不变。
3. 验证非法/截断快照、GPU 加载失败和 CPU fallback 不回归。
4. 运行 bridge 合同/故障注入、adaptor component、contest evaluator 和 benchmark 离线测试，以及 `git diff --check`。
5. 新服务器可用后，对优化前后候选复用同一 V5 基线，分别比较 BF/IVF quick 和 250K 的 ready、QPS、Recall、显存及 RSS。真实 GPU trace 和完整证据仍是性能结论的必要条件。

## 非目标

- 不实现持久查询线程或改变全局锁。
- 不调整 IVF 聚类/探测默认值，不自动切换算法。
- 不删除外层 IDs/向量，不破坏 VSAG fallback。
- 不把 mock 或静态分析结果表述为真实 GPU 性能结论。
