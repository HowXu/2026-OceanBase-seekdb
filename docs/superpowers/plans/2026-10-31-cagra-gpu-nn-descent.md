# Sparse Full-GPU NN-Descent Prototype Plan

**Goal:** 在全对全路线超时后，用稀疏二跳候选更新降低构图计算量，同时保持 top-256 候选质量。

**Architecture:** 随机唯一 top-256 初始化；每轮从当前邻居的邻居生成局部候选，FP32 计算距离，GPU 哈希去重后与原 top-256 做 radix 合并。所有工作驻留 GPU，不接查询、不调用 BruteForce Search。

## Gates

- [x] 512×32 与 CPU exact top-256 比较：12 轮 Recall=0.996628，ID 合法且无重复。
- [x] 完整 250K×128 的 12 轮候选更新为 61.448109s，明显慢于 18.36s 全对全和 22.87s 当前端到端，按门禁停止。
- [x] 未进入剪枝/反向边或 SQL；`results-gpu-nn-descent-candidates-20261031.md` 保存失败证据，不把减少迭代当作通过。
