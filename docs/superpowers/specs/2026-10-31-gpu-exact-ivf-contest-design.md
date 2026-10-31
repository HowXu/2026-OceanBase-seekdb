# GPU BruteForce / IVF-Flat 赛题后端设计

## 目标

在保留现有 CAGRA、SQL 路径、快照和离线 CI 的前提下，为 250K 级赛题负载增加两个可切换的 hipVS 后端：

- **BruteForce**：GPU 精确 Top-K，Recall 应为 1，尽量消除构图成本。
- **IVF-Flat**：倒排列表内保存 float32 原始向量并精确计算距离，以 `n_probes` 调节查询成本和 Recall。

先用相同 V5 基线、相同确定性数据和独立单行 SQL 比较构建、QPS、Recall 与资源，再决定最终默认算法。没有官方 Hidden250K 数据，不把本地结果表述为官方分数保证。

## 已存档证据

`validation/results-contest-pre-revert-20261031.{json,md}` 保存 `aa902de11` 候选的完整已产生报告。250K×768 的基线日志发生轮转，证据门禁正确地将总结果标为失败；已完成的低维轮次保留。`results-contest-quick-20261031.{json,md}` 保存通过的 quick 结果。

revert 流水线只完成了等待和基线完整性检查，因上述错误退出；revert 后二进制没有开始 GPU 评测。后续不再运行 768 维矩阵。

## 接口与选择方式

保持 `seekdb_cuvs_build/search/serialize/deserialize/free` ABI 不变。opaque handle 内部增加算法标签和三种索引指针，释放、搜索和序列化按标签分派。

构建时读取 `SEEKDB_CUVS_ALGO`：

- 未设置或 `cagra`：走当前 CAGRA，参数不变。
- `brute_force`：调用 `cuvsBruteForceBuild/Search`。
- `ivf_flat`：调用 `cuvsIvfFlatBuild/Search`。
- 其他值：构建失败并记录明确错误，不静默换算法。

反序列化使用同一环境变量选择对应 loader；实验期间要求构建和重载使用相同算法环境。默认 CAGRA 的快照格式和现有 CI 行为不变。

IVF-Flat 默认 `n_lists=min(512,max(1,n/128))`，`n_probes=min(32,n_lists)`，k-means 10 次迭代、训练集比例 0.25。实验可用 `SEEKDB_CUVS_IVF_N_LISTS` 和 `SEEKDB_CUVS_IVF_N_PROBES` 覆盖前两项；非法值、空列表或 `n_probes>n_lists` 直接失败。

## 数据与资源

两种算法均使用 L2Expanded、float32 和现有 host/device 拷贝及输出 ID 映射。IVF-Flat 不引入 PQ 量化误差；BruteForce 是精确检索。显存与 RSS 继续受现有资源门禁约束。

不在本任务中加入查询线程复用、快照 cut 或其他优化，避免无法归因 A/B 算法本身的收益。

## 验证

1. 扩展 mock C API 与 bridge 合同/故障注入测试，覆盖三种算法的构建、Top-10/100、序列化重载、非法配置和资源释放。
2. 保持现有 CAGRA 36 项及 benchmark/evaluator 离线回归通过。
3. 评测矩阵删除全部 768 维数据；新完整矩阵为 7 数据集、14 个配对轮次。
4. 新服务器上分别构建/选择 BruteForce 与 IVF-Flat，先 quick，再运行相同完整矩阵。
5. 只有真实 GPU trace、无 CPU fallback、结果 ID 完整且 Recall/QPS/资源门禁通过的结果才用于选择后续赛题候选。
