# C1 撤回、C2 取消后的查询路线复核

## 范围与状态

本轮根据用户提供的 A+B 新 profile 重新研究；不实施新优化、不重新采样、不构建、不跑 benchmark。当前生产基线仍是 A+B：源码等价于 `ae5c8f0df` / `964f20c92`，固定二进制为 `run_temp/fts-clean-base-final-seekdb`，SHA256 `6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c`。

研究任务：

- [x] 核对当前分支、C1 实验及撤回记录。
- [x] 撤销且保留 C2 未提交补丁，停止原计划。
- [x] 阅读新 profile 证据和现有查找/过滤调用链。
- [x] 离线拆分现有 LIMIT 栈，区分查找与换批子路径。
- [x] 对比三个窄范围方案及正确性边界。
- [ ] 用户确认下一轮设计方向。
- [ ] 确认后编写设计及实施计划，另行执行。

无可视化问题，无需浏览器。未调用子代理，未创建 worktree，未修改 benchmark。

### 已有实验不能被新 profile 覆盖

C1 的独立完整 A/B：前三条 COUNT 均值 +2.7144%，查询类别 +0.5656%，LIMIT -5.8810%。已提交实验 `a8966b495`，通过 `d68412028` 撤回；测试和结果保留在 `89e83c8a9`。新 profile 是 A+B 而非 C1 候选，不能用于证明 C1 后布尔函数依然热点，也不能解释其 LIMIT 回退。

C2 在用户取消前已编译成功，但初次 query-batch 回归 ID/相关度不一致。未做完整 A/B，不认定根因，不认定正确。取消后两处生产文件已恢复至 HEAD；临时补丁保存在 `run_temp/query-c2-cancelled.patch`，回归差异在 `run_temp/query-c2-initial-regression.diff`。**`build_release/src/observer/seekdb` 仍是取消前的实验构建，不能用于最终运行或充当基线。**

## 1. 新 profile 的证据强度

输入：`run_temp/fts-current-profiling/findings.md`、`query-phase-summary.txt`、`phase-summary.json`、`stacks-noinline.txt` 和 `summarize-current.py`。采样为固定 A+B 程序、完整原 benchmark、cpu-clock:u / 399Hz / frame pointer。

查询窗口只有 293 / 641 / 624 / 782 个样本，阶段按日志接收时间划分，可能有边界串扰；存在 unknown/未完整展开的帧。所有 inclusive 比例可能重叠，不相加、不换算墙钟收益。profile 下的耗时不作验收成绩。

COUNT 的 `filter_on_demand()` 为 10.61%–13.31%，bool double 为 3.21%–6.14%；bit-vector 总路径为 9.36%–10.42%，但其中并非全由过滤产生。`ObDocIdExt::from_datum()` 为 0.77%–1.02%，支持降低复制路线优先级，不能推出 C2 收益上限的精确数值。

### LIMIT 的进一步离线拆分

沿用原 phase-summary 的 LIMIT 窗口，逐条解析现有 leaf-to-root 栈，不追加采样：

| 栈集合 | 样本数 | 占全部 LIMIT 样本 |
|---|---:|---:|
| 全窗口 | 782 | 100% |
| 含 DaaT token `advance_to()` | 290 | 37.08% |
| 上述集合内还能识别 DaaT token `get_next_row()` 子路径 | 143 | 18.29% |
| 未识别到该 get-next 或命名 storage-advance 子帧 | 147 | 18.80% |
| `advance_to()` 本身为 leaf | 9 | 1.15% |
| `advance_to()` 下能识别字符串比较路径 | 106 | 13.55% |

前两个子集合 143/147 是按命名帧划分，不是严格的 inline/self-time 分解。147 中仍有 36 个 unknown leaf，以及可识别的 save-ID/score 等工作，不能全部视为线性比较；storage-advance 命名帧未被识别不意味着调用成本为零。比较集合也可能与 get-next 子路径重叠。

另识别到该 advance 栈下 save-ID/score 25、字符串解码 32、BM25 35 个样本，这些集合互相重叠。详细命名帧分区见 `run_temp/fts-current-profiling/advance-partition.txt`。

**结论：有依据优先研究批内查找，但 37.08% 不是该算法可以消除的成本。**

## 2. 源码确认的边界

### 批内定位

`src/storage/retrieval/ob_text_retrieval_token_iter.cpp`：

- `ObTextRetrievalDaaTTokenIter::init()` 第 420 行起，为 doc-ID 类型选择现有 `CS_TYPE_BINARY` 的 `null_first_cmp_`。
- `advance_to()` 第 543 行起，在 `[cur_idx_, count_)` 逐项比较 `target` 与缓存 doc-ID；`result <= 0` 即停止，等价于寻找第一个 doc-ID >= target。
- 批耗尽后调用底层 `ObTextRetrievalTokenIter::advance_to()`，再 `get_next_row()` 加载；保持原错误/EOF 和加载后首项检查。
- `save_relevances_and_docids()` 和 `save_docids()` 保留 `ObDocIdExt` 深拷贝；doc-ID 与 relevance 数组用同一索引，不改存储或生命周期。

`src/storage/retrieval/ob_sparse_bmw_iter.cpp:610` 的 `advance_dim_iters_for_next_round()` 推进 iterator 后继续取 ID、score 并 push heap。`ObTextRetrievalBlockMaxIter::advance_to()` 第 688 行起在成功后更新缓存 ID 并清除 shallow 状态。批内查找不应改变这些步骤、BMW pivot/threshold 或 BM25。

二分/galloping 的前提不能只靠“看起来有序”：设计/测试阶段须确认本路径扫描顺序与同一比较器一致，且搜索区域内没有未初始化/跳过槽位。当前 token skip 初始化为全零，保存逻辑仍按 skip 决定是否写槽；必须确认完整调用链不产生搜索空洞。若不能证明，则该状态继续原线性路径，不顺手改变 skip 语义。

### 布尔过滤

`src/storage/retrieval/ob_sparse_daat_iter.cpp:361` 当前向量路径逐文档投影 `double relevance`，设置 evaluated/projected 状态，清过滤表达式状态，再 scalar eval；判断 NULL 或整数零为拒绝。

`src/sql/engine/expr/ob_expr_bool.cpp` 的 double 分支使用 `0 == value ? 0 : 1`；EVAL_ARG 保留 NULL，`cg_expr()` 为 double/udouble 指定该 scalar 函数，未提供专用 batch 实现。

因此严格快路径不能写成 `relevance > 0`：负数、负零、NaN/Inf 等必须沿用现有 double 比较语义。也不能看到 `T_OP_BOOL` 就快走，必须确认唯一参数就是本 iterator 已投影的 relevance 表达式，且类型、cast/eval 语义符合；其他形状原路回退。

## 3. 三个待选方案

### A：短线性前缀 + 剩余区间 lower_bound

优点是实现与边界证明较简单；较长批次的大跨度跳跃由 O(d) 比较变为 O(log n)。缺点是中短距离可能比线性更贵，且若 target 超过整批末尾，仍需处理耗尽和原存储推进。可评估末项检查是否值得，但不要凭空增加多层判断。

### B：短线性前缀 + galloping + 区间二分

优点是以实际前进距离 d 为尺度，O(log d) 比较，中等跨度也可能受益。缺点是实现多于 A，需覆盖指数步进溢出、区间夹定和末项边界。不能在没有距离分布的情况下断言优于 A。

**推荐研究顺序：LIMIT 优先，先诊断跳距分布再从 A/B 中选一个候选，不同时引入两套可配置算法。** 诊断仅作为之后经批准的工作，不在本轮修改生产热路径。

### C：独立 C1b，严格 BOOL(relevance) 快路径

从原 A+B 而非已撤回 C1 出发，首先研究能否只在严格匹配形状中消除单行 bool 调度，保持通用表达式回退。它无需默认复活 raw-batch 缓冲和紧凑化；是否采用批量版应另有证据。

需明确投影 datum 和 evaluated/projected 状态的下游使用，不能仅返回一个布尔值而忽略后续可能读取的表达式结果；也不能改变错误传播。首轮可继续排除 LIMIT/OFFSET。扩展测试须证明形状确实命中以及非匹配形状回退，不能只比较 hits。

优先级放在 LIMIT 之后：C1 已有 COUNT 收益但整体未达门槛；A+B profile 只能证明该区域有成本，不能证明 C1b 一定获益。

## 4. 下一轮应先回答的问题

1. 批内实际可搜索长度与跳距：当前即命中、前进 1/2/4/8、长跳、整批耗尽分别占多少？二分/galloping 能减少多少比较？
2. 所有搜索槽位是否有效、顺序是否和原比较器一致？标量、部分批、重扫是否也满足？
3. 正确性：target 在当前项之前/相等、相邻项之间、末项/超过末项、单项/空批/EOF、跨批、比较器报错，结果与原线性参考完全一致。
4. SQL 层：中文/BEng/混合、多 token、同分排序、LIMIT/OFFSET、重扫、DML 及完整 ID:score 回归。显式确认计划确实走 BMW，不能仅凭 SQL 有 LIMIT。
5. 保留门槛须在实现前确认；建议同时约束 LIMIT 目标收益、查询类别及 COUNT 回退，不能仅看单个热点或总综合分。

仍遵守：每个生产候选一对完整 A/B、最多 -j11、评测不并行编译、不修改 benchmark/评分/输入、证据和生产提交分离、失败定向撤回、不 push。

## 本轮结束边界

只完成取消清理与研究复核。未实现 C3 或 C1b，未补跑 C1，未给任何新方案宣称性能收益。下一步等待用户确认 LIMIT-first 的设计方向及验收门槛。
