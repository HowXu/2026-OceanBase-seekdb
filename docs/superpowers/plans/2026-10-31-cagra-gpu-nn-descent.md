# Sparse Full-GPU NN-Descent Prototype Plan

**Goal:** 在全对全路线超时后，用稀疏二跳候选更新降低构图计算量，同时保持 top-256 候选质量。

**Architecture:** 随机唯一 top-256 初始化；每轮从当前邻居的邻居生成局部候选，FP32 计算距离，GPU 哈希去重后与原 top-256 做 radix 合并。所有工作驻留 GPU，不接查询、不调用 BruteForce Search。

## Gates

- [ ] 小 N 与 CPU exact top-256 比较 Recall 和 ID 合法性。
- [ ] 完整 250K×128 候选更新时间必须明显低于 18.36s，才有继续价值。
- [ ] 加入剪枝/反向边后验证高斯与图像 Recall；候选更新时间不是最终图质量证据。
- [ ] 仅微基准胜出才接 `IndexFromArgs` 和 SQL。
