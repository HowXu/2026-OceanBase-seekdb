# CAGRA 线程与单查询优化设计

## 目标与边界

在不改变固定 hipVS/cuVS 和 CAGRA Search 的前提下，先测 OpenMP 并行度、SINGLE_CTA/MULTI_CTA/AUTO 和常驻大栈 worker。暂不启动自定义 GPU 建图，也不恢复 IVF 或 BruteForce 查询。

最终二进制默认走 CAGRA；实验环境变量只用于归因，优胜配置必须固化为默认值。所有查询仍使用 4096 条独立单行 SQL、Top-10/100 和 FP32+L2。

## 已完成基础

- 纯 CAGRA 默认路径、查询/结果设备缓冲与 SearchParams 复用。
- graph-only CAGRA 快照及外部 device base 重挂接。
- 真实图像 d128/d256 V5 四轮冻结基线。

## 控制变量

OpenMP、搜索模式和 worker 第一轮均固定 graph_degree=128、intermediate_degree=256、nn_descent_niter=100、itopk=512。每轮只改变一个因素：

1. `OMP_NUM_THREADS=8/16/24`，与不设置该项对照。
2. `SEEKDB_CUVS_CAGRA_SEARCH_ALGO=single/multi/auto`，默认保持现有 SINGLE_CTA。
3. `SEEKDB_CUVS_QUERY_WORKER=0/1`，实现初期默认 0，避免改变正在排队的实验含义。

worker 复用一个 32 MiB 大栈 pthread；并发占用时退回原独立大栈线程。失败不得转成 CPU VSAG 查询。

## 验证与证据

每轮必须包含完整 ID 校验、Recall、QPS、资源采样、GPU serve、snapshot build 和零 CPU fallback。候选逐轮计算 Recall 下限、相对冻结 V5 的 Recall delta、QPS≥0.90、VRAM/RSS gates；`COMPLETE` 不能代替 gates PASS。

冻结均值用于开发筛选，最终候选仍需正式口径的配对复测。真实图像数据是 surrogate，不代表官方 SIFT250K/Hidden250K。

## 后续顺序

先用优胜线程、搜索模式和 worker 配置测 niter=50/25、intermediate_degree=192 和 ITERATIVE/IVF_PQ 构图。只有原生构图无法接近目标时，才另行评审自定义全 GPU 建图。构建综合加速达到 3×、查询达到 1.5×并保留 Recall/资源余量后停止复杂化。
