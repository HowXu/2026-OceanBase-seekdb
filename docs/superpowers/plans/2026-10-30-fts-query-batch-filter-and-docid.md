# FTS 查询 C1 批量过滤与 C2 批次内 doc-ID 浅引用实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. 用户已批准同时验证 C1 和 C2；在当前分支/会话单线执行，不创建 worktree、不调用子代理。本文仅为计划，不在编写计划时修改生产代码或启动跑分。

**Goal:** 在不改变 BM25、MATCH 过滤、归并顺序及 LIMIT/BMW 语义的前提下，减少三条 COUNT 查询的逐文档过滤开销，以及 posting 批次内冗余的 doc-ID 深拷贝。

**Architecture:** C1 在归并完成一批候选后调用现有 `ObExpr::eval_batch`，再按原谓词结果紧凑化；C2 在 `ObTextRetrievalDaaTTokenIter` 当前批次内保存浅 `ObDatum`，输出缓存仍持有深拷贝。两项分别实现、分别回归和评测；C1 失败并撤回后仍继续 C2。

**Tech Stack:** seekdb C++20、DAAT/BMW、ObExpr batch evaluation、ObDatum/ObDocIdExt、SQL 回归、原始 FTS benchmark、Git commit/revert。

## 1. 当前证据与目标

当前生产版本为保留 A+B 的最终程序，HEAD 文档提交为 `ae5c8f0df`，生产代码等价于 `964f20c92`。最终基线二进制为：

```text
run_temp/fts-clean-base-final-seekdb
SHA256 6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c
```

十轮 A/B 已证明 A+B 的构建/分词收益，但查询类别改善中位数为 **-0.8975%**。原始指标中位数为：

| 指标 | 原基线 | A+B |
|---|---:|---:|
| query_cn_avg_ms | 1.3894 | 1.4324 |
| query_beng_avg_ms | 1.9615 | 1.9730 |
| query_mixed_avg_ms | 1.5753 | 1.5589 |
| query_limit_avg_ms | 1.8698 | 1.8561 |

已有查询 profile 指向以下工作，比例是 CPU 样本且可能重叠，不相加、不直接换算墙钟收益：

- `filter_on_demand()`、`calc_bool_expr_for_double_type()`、单行表达式求值和 bit-vector 状态维护；
- `ObSRMergeCmp::cmp()`、字符串 doc-ID batch decode；
- `save_relevances_and_docids()`、`ObDocIdExt::from_datum()` 和输出缓存复制。

`T_OP_BOOL` 未提供专用 `eval_batch_func_`，会使用通用默认 batch 回退；该回退仍可能逐行调用 `eval_func_`。因此 C1 首先减少表达式调度、重复单行求值及状态维护，不能声称一次 `eval_batch` 就把布尔数学计算完全向量化。

## 2. Global Constraints

- 当前分支 `2026-competition-fts-clean-base`，不修改旧分支，不查看其他分支。
- 编译最多 `-j11`；评测期间不编译、不挂 profiler、不启用临时热路径计数。
- 不修改 benchmark、评分脚本、输入规模、baseline 文件，不执行 `telemetry_test.py`。
- 不省略 BM25、词频、doc_len、filter、布尔归并或必要 posting；COUNT 不等于可跳过评分。
- 不改变浮点累加顺序、doc-ID 排序、NULL/零/负数/NaN/Inf 等谓词语义。
- LIMIT 已有 BMW；C1 首版不改 LIMIT/OFFSET，C2 不改 pivot 或动态剪枝。
- 不为复用引用而引入跨 posting 批次裸指针；不改编码器或持久化格式。
- 每项只做一对完整 A/B；失败不反复跑到出现正收益。局部放大诊断不是评分证据。
- 测试、结果文档与生产补丁分开提交。未达标候选提交后定向 revert，保留证据，不自动 push。

## 3. 文件边界

### C1

- Modify: `src/storage/retrieval/ob_sparse_daat_iter.h`
- Modify: `src/storage/retrieval/ob_sparse_daat_iter.cpp`
- Test: `unittest/storage/fts_query_batch_regression.sql`
- Test: `unittest/storage/fts_query_batch_regression.result`
- Test runner: `unittest/storage/fts_parser_regression.sh` 已包含该测试，不为 C1 另建脚本
- Results: `docs/superpowers/experiments/2026-10-30-fts-query-c1-results.md`

### C2

- Modify: `src/storage/retrieval/ob_text_retrieval_token_iter.h`
- Modify: `src/storage/retrieval/ob_text_retrieval_token_iter.cpp`
- Test: 同一 `fts_query_batch_regression`，必要时只补生命周期/批次边界用例
- Results: `docs/superpowers/experiments/2026-10-30-fts-query-c2-results.md`

不修改 `ObTextRetrievalTokenIter` 的存储扫描/解码实现，不修改 `ObSRMergeCmp` 的二进制比较逻辑，不修改 `ObDocIdExt` 通用实现。

## Task 1：先扩展并冻结查询回归

**Files:** `unittest/storage/fts_query_batch_regression.sql` 及 `.result`；临时输出只写 `run_temp/`。

**Interfaces:** 使用当前 A+B 基线捕获行为，不能用候选输出反向修改 golden。现有 2051 文档、NULL 文本、多字段 IK/BEng、布尔模式、相关度阈值、排序、LIMIT/OFFSET、PREPARE 重用和 DML 用例继续保留。

- [ ] **Step 1：补 C1 的全通过/全拒绝/部分通过与重扫用例**

在现有 DML 前加入以下 SQL，保持测试文本及索引不变：

```sql
select count(*) from fts_query_batch
where match(en) against('alpha beta');
select count(*) from fts_query_batch
where match(en) against('alpha beta') > 1.0e300;
select id, round(match(en) against('alpha beta'), 8) as score
from fts_query_batch
where match(en) against('alpha beta') > 0.5
order by id limit 17;
select count(*) from fts_query_batch
where match(title, body) against('数据库') > 0;
prepare fts_query_filter_reuse from
  'select count(*) from fts_query_batch where match(en) against(?) > 0';
set @query_words='alpha';
execute fts_query_filter_reuse using @query_words;
set @query_words='no_such_word_zzzz';
execute fts_query_filter_reuse using @query_words;
deallocate prepare fts_query_filter_reuse;
```

这些用例分别覆盖普通 MATCH 布尔转换、全部拒绝、带 LIMIT 的原路径、显式数值谓词及重扫。不得为了通过候选而放宽相关度精度或只检查行数。

- [ ] **Step 2：补 C2 的批次边界和输出持有检查**

保留已有跨 1024 行的完整 `ID:score` 拼接、不同 batch/EOF、LIMIT/OFFSET 与 PREPARE 用例。若现有输出不足，增加一条按 ID 排序的完整拼接，而不是只取首行：

```sql
select group_concat(concat(id, ':', round(match(en) against('alpha beta gamma'), 8))
                    order by id separator ',')
from fts_query_batch where match(en) against('alpha beta gamma');
```

这用于发现跨 posting 批次后浅引用被覆盖，不能只依赖 hits。

- [ ] **Step 3：在改生产代码前捕获基线**

用现有专用实例启动方式执行测试两次；两次输出必须一致，将结果保存为 `.result`，并在当前六组回归中验证：

```bash
bash run_temp/run-fts-sql.sh query-c12-golden-baseline \
  run_temp/fts-clean-base-final-seekdb \
  unittest/storage/fts_query_batch_regression.sql
cp run_temp/query-c12-golden-baseline.tsv \
  unittest/storage/fts_query_batch_regression.result
```

第二次只运行同一 SQL，不与 benchmark 并行。若无法复现一致结果，先修正测试的未指定排序或环境依赖，不进入生产实现。

- [ ] **Step 4：保存基线身份和测试说明**

记录基线 SHA256、测试覆盖范围及原始输出。性能重构不制造人工行为失败；测试的职责是证明修改前后相同输入产生相同结果。

## Task 2：C1——候选批次过滤与紧凑化

**状态：已完成并撤回。** 六组回归一致；前三条 COUNT 平均 +2.7144%，查询类别 +0.5656%，但 LIMIT -5.8810%，未满足保留门槛。按约定提交生产实验后 revert，不追加评测。结果见 `docs/superpowers/experiments/2026-10-30-fts-query-c1-results.md`。C2 继续以原 A+B 为直接基线。

**Files:** `ob_sparse_daat_iter.{h,cpp}`，以及 Task 1 的回归资产。

**Interfaces:**

```cpp
// Existing public interface remains unchanged.
virtual int get_next_rows(const int64_t capacity, int64_t &count) override;

// Internal helpers; exact names/signatures introduced by C1.
int do_one_merge_round(int64_t &count, const bool defer_filter);
int filter_batch(const int64_t raw_count, int64_t &filtered_count);
```

`filter_expr_`、`relevance_proj_expr_` 及求值上下文沿用 `ObSparseRetrievalMergeParam`。无过滤、非向量化和有效 LIMIT/OFFSET 继续调用原逐行 `filter_on_demand()`。

- [ ] **Step 1：限制 C1 快路径**

在 `get_next_rows()` 中，仅当以下条件全部成立时使用批次过滤：

```cpp
iter_param_->need_filter()
    && iter_param_->eval_ctx_->is_vectorized()
    && !iter_param_->limit_param_->is_valid()
```

`limit_param_` 已由现有 `cache_result()` 视为必非空，本补丁不改变该契约。其他路径必须保持现有控制流、返回码和计数方式。

- [ ] **Step 2：保留原始归并，只延迟过滤**

将现有 `do_one_merge_round(count)` 主体接到上述 `defer_filter` 参数，保留原一参数包装调用，避免改变其他子类/调用点：

```cpp
fill_merge_heap();
collect_dims_by_id(id_datum, relevance, need_project);
if (need_project && defer_filter) {
  cache_result(count, *id_datum, relevance); // raw candidate; BM25 already complete
} else if (need_project && filter_on_demand(count, relevance, need_project)) {
  // existing error propagation
} else if (need_project) {
  cache_result(count, *id_datum, relevance);
}
```

实际实现保持现有 `OB_FAIL` 风格。`got_valid_id=false` 仍不能缓存。原始归并和打分先后顺序不变。

- [ ] **Step 3：初始化一次全零 skip vector**

在 `ObSRDaaTIterImpl` 增加私有 `sql::ObBitVector *empty_skip_`，构造时置空。初始化时使用 `iter_allocator_` 分配 `ObBitVector::memory_size(max_batch_size)`，调用 `init(max_batch_size)`，不在每批分配内存。分配失败返回 `OB_ALLOCATE_MEMORY_FAILED`；reset 只清指针，不单独释放 arena 拥有的内存。

- [ ] **Step 4：实现批次求值**

`filter_batch(raw_count, filtered_count)` 必须：

1. 拒绝负数和超过数组/表达式容量的 `raw_count`，返回 `OB_INVALID_ARGUMENT`；零输入直接令输出为零并成功返回。
2. 用 `ObEvalCtx::BatchInfoScopeGuard` 将 batch size 设为 `raw_count`。
3. 按原顺序将 `buffered_relevances_[i]` 写入 `relevance_proj_expr` 的第 i 个 datum。
4. 使用现有 `get_evaluated_flags().set_all(raw_count)` 设置连续标志，并调用一次 `set_evaluated_projected()`。
5. 清除 filter 的旧 evaluated 状态，调用：

```cpp
filter_expr->eval_batch(*eval_ctx, *empty_skip_, raw_count)
```

6. 从 `filter_expr->locate_expr_datumvector(*eval_ctx)` 读取每个结果；与原代码一致，`NULL` 或整数零拒绝，其余接受。
7. 以稳定紧凑化保留通过者：第 i 项通过时，将原始 doc-ID 和 relevance 移到 `filtered_count` 槽位，再递增。不得对相关度重新排序或重新计算。
8. 错误时返回原 `ret`，不能将失败表达式解释为全部通过或全部拒绝。

- [ ] **Step 5：形成允许部分批次的输出循环**

快路径每次先收集至多 `MIN(capacity, max_batch_size)` 个 raw candidate，再过滤。`count` 只包含通过项；`OB_ITER_END` 且仍有通过项时，沿用当前“有数据可投影”的处理，不丢失尾批。

过滤后为零但尚未结束时，继续收集下一批，不能把成功的空批当作迭代结束。首版不为凑满 capacity 而重复补批；向量迭代允许返回部分批次。该限制只用于无有效 LIMIT/OFFSET 的快路径，因此原 offset、limit 提前结束语义不受影响。

- [ ] **Step 6：先用诊断证明机制，再移除诊断**

临时、任务级统计 raw candidate 数、batch filter 调用数和逐行 bool eval 调用数；每个 iterator 结束汇总一次，不逐文档打日志。预期候选不再对每个 raw candidate 进入 `eval_one_datum_of_batch()`，但不能要求 `calc_bool_expr_for_double_type()` 的调用次数降到每批一次，因为默认 batch 回退仍可逐行调用它。诊断完成后移除全部计数/日志，再构建验收二进制。

- [ ] **Step 7：构建和正确性验证**

```bash
cmake --build build_release --target seekdb -j11 \
  > run_temp/query-c1-build.log 2>&1
bash run_temp/run-fts-variant.sh query-c1-candidate \
  build_release/src/observer/seekdb
```

验收前保存候选快照。六组回归、详细 ID/相关度、NULL、阈值、全拒绝、重扫、LIMIT/OFFSET 和 DML 必须一致。若正确性不一致，停止性能判断并修复；不修改 golden 迁就实现。

- [ ] **Step 8：一对完整 A/B 与去留**

直接基线为 `run_temp/fts-clean-base-final-seekdb`。候选与基线各运行一次未修改 benchmark，使用全新实例目录，参数保持 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。

保留门槛：前三条 COUNT 指标的改善率均值至少 **2%**，查询类别综合不为负，LIMIT 没有未解释的明显回退。否则记录“收益不足/未证明”，不追加多轮测试。门槛是工程取舍，不是官方评分规则。

- [ ] **Step 9：留档、提交，必要时 revert**

结果文档写入九项指标、构建/分词/查询类别、样本分布、正确性、临时诊断结论和最终决定。测试与结果先提交；生产补丁单独提交。未达标时：

```bash
git revert --no-edit <C1_PRODUCTION_COMMIT>
```

无论 C1 保留还是撤回，都继续 C2；C2 的直接基线取 C1 决策后的实际生产 HEAD。

## Task 3：C2——仅在 posting 批次内使用浅 doc-ID

**Files:** `ob_text_retrieval_token_iter.{h,cpp}`；测试与结果资产同第 3 节。

**Interfaces:** `ObISRDaaTDimIter` 的所有虚接口、返回码与排序契约不变。只改变 `ObTextRetrievalDaaTTokenIter` 的私有数组元素类型：

```cpp
// Before: owns a copy for every buffered posting.
ObFixedArray<ObDocIdExt, ObIAllocator> doc_id_;

// After: datum and payload are valid only until the next underlying batch advance.
ObFixedArray<ObDatum, ObIAllocator> doc_id_;
```

- [ ] **Step 1：明确引用有效区间**

当前 `save_relevances_and_docids()` / `save_docids()` 在下层 `token_iter_` 返回批次后读取 `inv_scan_domain_id_col_` 的 datum vector。浅 `ObDatum` 及其 payload 只允许使用到下一次 `token_iter_->get_next_row()`、`get_next_batch()` 或 `advance_to()` 导致下层缓冲区推进之前。

归并层在子迭代器再次推进前必须完成比较或复制；`ObSRDaaTIterImpl::cache_result()` 继续通过 `ObDocIdExt::from_datum()` 持有输出值。不得将本任务的浅数组地址存入 merge item、跨 batch 缓存、全局 cache 或查询结果。

- [ ] **Step 2：将保存操作改为浅 datum 赋值**

`doc_id_` 的初始化容量和分配器不变。仅把两个保存循环中的：

```cpp
doc_id_[i].from_datum(*doc_id_datum.at(i))
```

替换为等价的浅赋值：

```cpp
doc_id_[i] = *doc_id_datum.at(i);
```

不再调用 `from_datum()`，不改变 skip 下标、`cur_idx_`、`count_` 或 relevance 保存逻辑。跳过项不得被新的“压缩下标”重新编号。

- [ ] **Step 3：更新当前批次内的读取点**

将 `advance_to()` 中 `doc_id_[cur_idx_].get_datum()` 改为 `doc_id_[cur_idx_]`；将 `get_curr_id()` 的返回改为：

```cpp
id_datum = &doc_id_[cur_idx_];
```

保留 `cur_idx_ >= count_` 的 `OB_ARRAY_OUT_OF_RANGE` 检查。`reuse()`/`reset()` 仍清空游标和数组；下一次保存批次覆盖数组槽位之前，不允许外部继续持有这些指针。

- [ ] **Step 4：检查归并与输出的持有边界**

确认 `iter_domain_ids_` 在每次子迭代器 `get_curr_id()` 后更新；heap 只保存 iter_idx，不长期保存浅 datum。输出仍走：

```cpp
buffered_domain_ids_[count].from_datum(id_datum)
```

不得为了进一步减少复制，同时把 `buffered_domain_ids_` 改成浅引用。BMW 的 `advance_shallow()` 和 block-max 自有 `curr_id_` 保持原实现，不把不同生命周期的指针混用。

- [ ] **Step 5：构造一次针对性的生命周期验证**

使用现有 2051 文档查询完整消费多个 posting 批次，重点执行：完整英文 `ID:score` 拼接、跨批阈值过滤、BMW `advance_to()`、PREPARE 重扫及索引后 DML。基线和候选输出逐字节一致。

若需临时机制证据，只累计批次数、`from_datum()` 调用数并在 iterator 结束汇总；不打印 doc-ID。诊断后删除，再构建正式候选。

- [ ] **Step 6：构建与全部回归**

```bash
cmake --build build_release --target seekdb -j11 \
  > run_temp/query-c2-build.log 2>&1
bash run_temp/run-fts-variant.sh query-c2-candidate \
  build_release/src/observer/seekdb
```

除六组回归外，C1 若被保留，其结果不得因 C2 改变。发现跨批覆盖、错误相关度、错误排序或 use-after-free 风险时，不通过修改测试或扩大缓存掩盖，恢复必要的深拷贝。

- [ ] **Step 7：一对完整 A/B 与去留**

以 C1 决策后的生产版本为基线，保存对应二进制和 SHA256；运行一对完整原始 benchmark，使用全新实例目录。

保留门槛：查询类别改善至少 **2%**，目标查询有实际改善，其他两类没有未解释的明显回退。只有复制次数减少而耗时无改善，不保留额外生命周期约束。失败候选同样先提交再 revert，证据和测试独立保留。

- [ ] **Step 8：最终结果与未做事项**

结果文档明确 C1/C2 各自贡献，不能把两次不同直接基线的改善率简单相加。记录未实施的专用布尔 SIMD、doc-ID 解码器改造、BMW lower_bound 和批量紧凑投影之外的通用查询重构，不宣称这些已完成。

## Task 4：最终验证与分支收尾

**Files:** 本计划、两份实验记录；最终生产源码只能包含经各自门槛决定保留的补丁。

- [ ] 用最多 `-j11` 构建实际最终 HEAD；不能用含已撤回实验的旧二进制冒充最终程序。
- [ ] 运行 BEng、Unicode、IK-chain、word-map、build-sort、query-batch 六组回归；所有 diff 为空。
- [ ] 检查完整 ID/相关度输出、全拒绝、NULL、重扫、DML、LIMIT/OFFSET、BMW advance 与批次边界，不只检查 hits。
- [ ] 确认没有临时计数、日志或 profiler 残留；测试服务与编译进程退出。
- [ ] `git diff --check`、shell runner 语法检查通过；`tools/benchmark/` 与新分支初始提交一致。
- [ ] 汇总最终保留的生产 commit、撤回的 experiment/revert commit、二进制 SHA256 和每个候选的真实测量；工作区保持干净，不自动 push。
- [ ] 不自动追加十轮测试；若用户随后要求最终稳定性测量，再使用相同原始配置进行完整配对。

## 4. 完成标准

本计划完成表示：C1 和 C2 **都已分别实现并验证，且各自有明确的保留或撤回结论**，不是要求两项都必须保留。任何一项的正确性无法保持时，必须撤回该项，不能以性能收益抵消语义变化。
