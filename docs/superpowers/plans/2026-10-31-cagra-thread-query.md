# CAGRA Thread And Single-Query Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用可归因的小实验释放固定 CAGRA 的构图和单查询性能，不牺牲 Recall、ID 正确性或 GPU 路径证据。

**Architecture:** bridge 增加实验期搜索模式解析；adaptor 增加可开关的常驻 32 MiB 大栈 worker。OpenMP 通过进程环境控制固定库，不修改 hipVS。每轮只改一个变量，完整 SQL gates 通过后再组合优胜项。

**Tech Stack:** C11 bridge、C++ adaptor、pthread/condition_variable、Python SQL evaluator、固定 hipVS/cuVS。

## Global Constraints

- 不修改固定 hipVS/cuVS；查询必须为 CAGRA Search。
- 最终默认零算法环境变量；远端编译严格 `-j48`。
- Top-10/100 各 4096 条独立 SQL；完整 gates、GPU 和资源证据缺一不可。
- 主会话单线执行，不使用 subagent；真实图像数据仅为 surrogate。

---

### Task 1: 冻结真实图像基线

**Files:**
- `docs/gpu-vector-index-hipvs-cuvs/validation/results-contest-v5-siftlike-four-20261031.*`
- `docs/gpu-vector-index-hipvs-cuvs/validation/results-contest-v5-imagelike-d256-four-20261031.*`

- [x] 校验两轮数据集均有四个有效轮次、8193 GPU serve、一个 snapshot build 和零 fallback。
- [x] 提交 d256 基线 `063407c71`，保留已提交的 d128 基线 `746c2b223`。

### Task 2: CAGRA 搜索模式校准入口

**Files:**
- Modify: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`
- Modify: `unittest/oblib/vector/test_hipvs_bridge.cpp`
- Modify: `unittest/oblib/vector/hipvs_mock/cuvs/neighbors/cagra.h`

**Interfaces:**
- Consumes: `SEEKDB_CUVS_CAGRA_SEARCH_ALGO=single|multi|auto`。
- Produces: handle 缓存的 `cuvsCagraSearchAlgo`，首次 SearchParams 初始化时解析。

- [x] 写失败测试：三种名称分别映射到 `SINGLE_CTA/MULTI_CTA/AUTO`，未知名称和显式 `single+itopk>512` 必须失败。
- [x] 运行 `bash unittest/oblib/vector/run_hipvs_bridge_tests.sh`，确认因模式尚未解析而失败。
- [x] 最小实现：未设置时保留现有 `itopk>512→MULTI_CTA` 行为；显式模式覆盖该自动选择。
- [x] 53 项 bridge 检查通过；提交 `4e94d0071`。

### Task 3: 可独立开关的常驻大栈 worker

**Files:**
- Modify: `src/oblib/lib/vector/ob_vsag_adaptor.cpp`
- Test: `unittest/oblib/vector/test_hipvs_adaptor_hotpath.cpp`
- Test: `unittest/oblib/vector/run_hipvs_adaptor_component.py`

**Interfaces:**
- Consumes: `SEEKDB_CUVS_QUERY_WORKER=0|1`，实现期默认 `0`。
- Produces: `ob_cuvs_run_big_stack(function, argument)`；worker 忙时返回 false，由调用方走原独立线程路径。

- [x] 增加实际 adaptor hotpath 测试，确认当前每条查询创建线程而目标要求复用。
- [x] 仅恢复 `CuvsLargeStackWorker` 与调用替换，不带回历史 snapshot mapping 或工作区改动。
- [x] worker=1 的 200 次查询新增线程为 0，worker=0 为 200；保留忙时独立线程路径。
- [x] sanitizer、真实 adaptor+VSAG component 和 `git diff --check` 通过；提交 `316178b8f`。

### Task 4: 串行远端微基准与完整 SQL gates

**Files:**
- Remote: `/work/run-cagra-thread-query-sweep.sh`
- Results: `/work/evals/cagra-{omp8,omp16,omp24,single,multi,auto,worker}-*`

- [x] 串行构建 bridge 和 `build_eval`，严格 `-j48`；修复 zombie 等待和重复实例问题。
- [x] 完成 OMP=4/6/8/16/24、single/multi/auto、worker=1 对照，OMP=8 最优。
- [x] 每轮传冻结 V5 基线并保存完整 gates；显式 MULTI_CTA 和常驻 worker 均独立通过。
- [x] ITERATIVE、短 niter 和 IVF_PQ 的失败轮次不计为通过；跨分布 Recall 与资源失败均淘汰。

### Task 5: 组合优胜项并筛选构图

**Files:**
- Modify only when a winner is measured: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`
- Modify only when worker wins: `src/oblib/lib/vector/ob_vsag_adaptor.cpp`

- [x] 固化 MULTI_CTA、常驻 worker 和 OpenMP=8；用 `kmp_set_defaults` 覆盖内部 std::thread，零配置实测 ready=22.8830s。
- [x] 完成 niter=50/25/12、intermediate=192、ITERATIVE 和 IVF_PQ 筛选；高斯 Recall 证明应保留 niter=100 和 degree=128/256。
- [x] d128/d256 各四轮零配置完整 gates 全部通过；报告 `results-contest-cagra-final-four-20261031.*`，并比较冻结 V5 与增强 10% 外部参考。
- [x] 查询已越过 1.5×封顶线；原生构图尚未达到综合构建 3×，后续构建优化交由独立工作流，本计划不再实施自定义建图。
