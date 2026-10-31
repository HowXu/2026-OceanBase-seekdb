# FP32 Blocked Full-Neighbor CAGRA Graph Plan

**Goal:** 用分块 GPU 全近邻、GPU 图优化和固定 CAGRA Search 同时改善构建速度与跨分布 Recall。

**Architecture:** 第一关独立生成每行 top-256 候选，不改变默认 SQL 路径。第二关加入排序、多样化剪枝、反向边和 `IndexFromArgs`。第三关才接 SQL 四轮验收。查询实现和参数冻结。

## Global Constraints

- 禁止 `cuvsBruteForceSearch`，禁止用其他索引替代 CAGRA Search。
- FP32 base 一次上传；不全量物化 N×N 距离。
- 初始行块 256、列块 8192、degree 128、候选 256。
- 第一关未通过速度/显存预算就停止全对全路线，不包装慢方案。
- 远端构建 `-j48`；不修改固定 hipVS/cuVS。

### Task 1: 距离与选择微基准

**Files:** `tools/hipvs_bridge/cagra_full_graph.{h,cpp}`、`unittest/oblib/vector/test_cagra_full_graph.cpp`

- [x] 512×32 随机数据逐行对比 CPU exact top-256，排除自身并检查唯一合法 ID。
- [x] 完整 250K×128 执行上传、分块 GEMM、局部选择、全局合并、同步和首轮 ID 检查。
- [x] 候选生成 18.356173s，未含后续图优化和外层流程，超过预算；按停止条件淘汰全对全路线，不跑 d256/SQL。

### Task 2: 完整图构建

- 全对全原型未进入本阶段；下一版改用稀疏全 GPU NN-Descent 候选更新后重新评审。

### Task 3: SQL 验收

- 未接 SQL；`results-full-graph-candidates-20261031.md` 保存停止证据。
