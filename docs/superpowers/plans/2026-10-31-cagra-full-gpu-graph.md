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

- [ ] 小 N 随机数据逐行对比 CPU exact top-256，排除自身并检查唯一合法 ID。
- [ ] 完整 250K×128/256 执行上传、分块 GEMM、局部选择、全局合并、同步和首轮 ID 检查。
- [ ] 记录端到端候选生成时间与显存；只测 GEMM 不算通过。

### Task 2: 完整图构建

- [ ] GPU 排序和 256→128 多样化剪枝；不得退化为直接最近 128。
- [ ] GPU 反向边、去重与容量合并，输出连续 N×128 `uint32` graph。
- [ ] `IndexFromArgs` 挂接现有 device base，验证固定 CAGRA Search、Recall 和资源。

### Task 3: SQL 验收

- [ ] 仅让微基准优胜版本进入默认候选路径。
- [ ] 图像替代集与高斯压力集完整 gates 后再跑 d128/d256 各四轮。
- [ ] 拆分构图、外层扫描传输、序列化和首查询时间，不以纯构图加速冒充端到端加速。
