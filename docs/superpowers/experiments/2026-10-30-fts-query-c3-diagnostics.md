# C3：LIMIT/BMW 批内 doc-ID 查找诊断

## 结论

**停止 C3，不创建生产候选，不执行正式性能 A/B。**

唯一一次完整诊断中，原算法有 **10,160,362** 次 doc-ID 比较。六个模型的最佳值是 B2（前缀 2，然后 galloping/bracketed lower_bound），共 **9,676,385** 次，减少 **4.7634%**，未达到事先批准的 **20%** 门槛。A4 为 **9,742,640** 次，减少 **4.1113%**；它也未过线，不能借用“与最佳差距不超过 5% 时优先简单 A”的规则降低门槛。

这只是比较次数筛选，不是耗时收益测量。没有 C3 生产实现，没有正式 A/B，也不自动转做 C1b。保留已有 A+B 生产状态和先前冻结的 SQL 回归。

## 执行边界与证据

- 设计：`docs/superpowers/specs/2026-10-30-fts-limit-inbatch-seek-design.md`。
- 计划：`docs/superpowers/plans/2026-10-30-fts-limit-inbatch-seek-diagnostics.md`。
- 起始 HEAD：`79b03e328`，分支 `2026-competition-fts-clean-base`。
- 生产源码和 benchmark 基准状态：`ae5c8f0df` 的 A+B。
- 单会话执行，无子代理、无 worktree、无其他分支检查、无 push。
- 编译使用 `cmake --build build_release --target seekdb -j11`，不与测试、benchmark 或 perf 并行。
- 原 benchmark、评分脚本、历史 baseline、输入规模和 expected answers 均未修改；未执行 `telemetry_test.py`。
- 临时代码只在 `src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}`：保留原线性查找和存储推进，仅增加模型、计数和批次验证。
- 诊断二进制 SHA256：`54f1ba177d899855e6492f0606196eaf8ca6ac44d424560a740805afd3af8d46`。

## 安全范围：源码契约而非运行时猜测

以下行号指插桩前的 A+B 源码。证明仅覆盖常规非 function-lookup FTS BMW、现有 BM25、单 token 前向扫描、标准生成的 binary VARCHAR doc-ID、行数大于 1 的向量批次。**不覆盖**任意 sparse iterator、反向扫描、自定义 doc-ID 表达式或 skip 空洞。

### 顺序与比较器

- `src/sql/resolver/ddl/ob_fts_index_builder_util.cpp:962–1032` 的 `generate_doc_id_column()` 生成非 NULL、`ObVarcharType`、`CS_TYPE_BINARY` doc-ID。
- 同文件 `734–806` 的 `set_fts_index_table_columns()` 设置 token/doc-ID rowkey，并保留列排序信息；`921–959` 的 `inner_adjust_fts_arg()` 使用默认 `ObColumnSortItem`。
- `src/share/ob_rpc_struct.h:1496–1501` 中 `ObColumnSortItem` 默认 ASC。
- `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp:145–176` 在本次 BM25 对应的 `need_inv_idx_agg()` 路径强制 Forward；`638–668` 的 `gen_inv_idx_scan_default_range()` 固定 token，doc-ID 范围为 min..max。由索引 rowkey 和前向扫描契约，批内 doc-ID 按 binary 顺序非递减。
- `src/storage/retrieval/ob_text_retrieval_token_iter.cpp:420–449` 按实际 ID 类型选择 `CS_TYPE_BINARY` 的 `null_first_cmp_`。模型候选不得改用整数解释或另一比较器。

### 有效槽位、skip 与所有权

- token iterator 的 `init():54–88` 在该 BM25 路径分配私有 skip 并清零；扫描 API 不接收该 skip 指针。
- `token_iter.h:81` 的 `get_skip()` 虽返回可变指针，但本调用链的用户是 `save_relevances_and_docids()/save_docids():499–535`，仅检查 skip。
- `token_iter.cpp:231–264` 以 const 输入传给 `eval_batch`；`src/query/api/query/engine/expr/ob_expr.h:1025–1039,1072–1093` 延续该输入契约。
- `src/sql/engine/expr/ob_expr.cpp:660–710` 操作独立的 evaluated flags 和表达式 datums；`ob_expr_bm25.cpp:100–143` 读取 skip、写结果 datums 和独立 flags，并将 const skip 传入参数表达式。本链没有将该输入 const_cast 后改写的路径。
- 成功保存批次时，对每个未跳过 ID 深拷贝到私有 `ObDocIdExt`；ID 和 score 采用同一索引。不得恢复 C2 datum 缓存或不安全浅引用。

### 推进与生命周期

- `token_iter.cpp:472–497` 仅在当前批次耗尽后载入下一批；`543–577` 在加载前查找，搜索期间不重写缓存。
- `267–299` 的底层推进先用 `from_datum()` 将目标复制到 `advance_doc_id_`，再改写扫描范围或推进存储。
- `688–700` 的 block-max 推进委托给 token iterator，并刷新当前 ID。
- `src/storage/retrieval/ob_text_daat_iter.cpp:84–103` 的 BMW reuse/reset 调用 block-max iterator 的 reuse/reset，后者刷新嵌入的 token 统计。
- legacy merge 的 `inner_release():799–854` 在清理 allocator 前 reset sparse retrieval。临时汇总可在私有内存失效前输出。

完整审计副本：`run_temp/query-c3-safe-scope.txt`。后述运行时验证仅佐证上述范围，不据此扩大适用条件。

## 模型与定向验证

模型严格模拟已批准的 A/B 两种搜索规则和前缀 **1/2/4**，前缀包含当前槽位。包括所有 endpoint、prefix、lower_bound/galloping 比较；资格外路径保持原成本；存储加载后的首元素比较在六个模型中全部保留。

- `run_temp/test-c3-model.py`：安装模型前按预期失败，日志 `query-c3-model-red.log`；安装后穷举模型/独立 oracle 通过，日志 `query-c3-model-test.log`。
- 该测试验证临时比较次数模型，**不等于测试任何生产搜索实现**。
- `query-c3-plan.tsv` 的 EXPLAIN 显示 TOP-N SORT / TEXT RETRIEVAL SCAN、rowset=256、relevance DESC 和 LIMIT pushdown；这本身不能证明实际执行了 BMW advance。
- `query-c3-runtime-probe.sql` 使用与 LIMIT benchmark 相同形态的无 ORDER BY 内层 LIMIT，验证返回 20；实际取得 **895** 个搜索段、**10** 次存储推进、**3,615** 次比较，空洞/逆序/验证错误/执行错误均为 0。原来按 round(score) 排序的回归不能替代这个路径探针。
- 最终诊断构建的六组回归全部通过：BEng、Unicode、IK-chain、word-map、build-sort、query-batch。日志：`run_temp/query-c3-final-diag-regressions.tsv`。

### 诊断输出链路修正

完整 benchmark 开始前，定向验证发现控制台没有统计。增加临时生命周期/stream 观察后确认：reset/reuse 确实调用，字符串非空，`fprintf` 返回成功；记录实际进入 `seekdb.log` 而非控制台，直接 stdio 写入还会与服务日志交错。随后移除这些定位标记与 `fprintf`，改为每生命周期一次的原生日志 `C3_DIAG_OUTPUT`，严格提取其完整 payload。

解析时不接受裸 `C3_DIAG` 文本，也不混入旧实例日志；日志以容错解码读取非统计二进制字节，但统计 payload 必须完整符合 ASCII 字段格式。完整 benchmark 的 **6,941** 个专用日志标记对应 **6,941** 个完整记录；不存在为过门槛而丢弃不利/残缺统计的情况。启动包装器仅用于隔离诊断，benchmark 本体未改。

## 唯一一次完整诊断

命令：

```bash
bash run_temp/run-c3-diagnostic.sh query-c3-diagnostic run_temp/query-c3-diag-seekdb
```

固定参数：

```text
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0
port=12883 memory_budget=2G datafile_size=1G datafile_maxsize=4G log_disk_size=2G
```

包装器在启动前创建 `run_temp/query-c3-diagnostic-once.guard` 防止重复执行，并在服务退出后保留完整日志。没有重跑完整诊断或丢弃完整 benchmark。四项 hits 为 **8001 / 11000 / 7332 / 20**。

原始报告：`run_temp/query-c3-diagnostic-report.txt`；benchmark 日志：`query-c3-diagnostic-benchmark.log`；服务日志：`query-c3-diagnostic-server-logs/`；提取的统计：`query-c3-diagnostic-records.txt`。

提取统计 SHA256：`3d73ff7436144bcc88df65dccbeb1c1bab58285f99844e6d88fec300bebd686b`。

### 统计与完整性

| 项目 | 总数 |
|---|---:|
| 生命周期汇总记录 | 6,941 |
| 实际含 advance 搜索段的汇总记录 | 1,893 |
| 搜索段 | 4,119,168 |
| 批内耗尽段 | 24,609 |
| 存储推进 | 24,609 |
| 已处于批尾的推进 | 0 |
| scalar/single-row 段 | 0 |
| 加载批次 | 148,916 |
| 执行错误 / EOF | 0 / 0 |
| 空洞 / 逆序 / 验证错误 | 0 / 0 / 0 |

每条记录的 histogram 之和均等于 segments；全部记录参与聚合，包括未执行 advance 的 COUNT 生命周期。三种含 advance 的生命周期形态各重复 631 次，与固定查询的 hit 检查、30 次预热和 3×200 次采样相符。每个存储推进均成功，因此 **24,609** 次加载后首项比较是六模型完全相同的固定成本；资格外 scalar/single-row 成本为 0。原算法剩余批内比较为 **10,135,753** 次。

### 跳距分布

`d` 是当前槽位到原线性算法停止槽位的距离；批内耗尽记为剩余长度。

| d | 搜索段数 |
|---|---:|
| 0 | 1,479,695 |
| 1 | 1,013,386 |
| 2–3 | 1,548,474 |
| 4–7 | 631 |
| 8–15 | 43,539 |
| 16–31 | 33,443 |
| 32–63 | 0 |
| ≥64 | 0 |

### 六模型总成本

| 模型 | 比较次数 | 相对原算法减少 | 达到 20% |
|---|---:|---:|---|
| 原线性算法 | 10,160,362 | — | — |
| A1 | 22,184,067 | −118.3393% | 否 |
| A2 | 18,070,578 | −77.8537% | 否 |
| A4 | 9,742,640 | +4.1113% | 否 |
| B1 | 10,367,330 | −2.0370% | 否 |
| B2 | 9,676,385 | +4.7634% | 否 |
| B4 | 9,829,718 | +3.2543% | 否 |

小跳占主导；更复杂搜索的探测开销抵消了少数长跳节省。这里不能把 advance 的 inclusive perf 占比当作可消除线扫成本，也不能将 4.7634% 比较减少解释为等幅查询提速。

冻结选择接口：`run_temp/query-c3-selection.json`，`decision=stop`、`algorithm=null`、`prefix=null`。使用整数执行 20% 与 5% 判定，规则未事后调整。

## 清理与最终验证

- [x] Task 1：冻结 SQL golden 和 EXPLAIN，保留测试提交 `79b03e328`。
- [x] Task 2–3：源码安全审计、模型红绿测试、原算法观察、实际推进探针及诊断构建六组回归。
- [x] Task 4：唯一一次完整诊断、完整 payload 校验、整数门槛判定；结果为 stop。
- [x] Task 5：保存补丁 `run_temp/query-c3-diagnostics.patch`，移除两份 token-iterator 文件中的全部临时模型/字段/校验/日志，恢复原 `advance_to`。
- `git diff --exit-code ae5c8f0df -- src/ tools/benchmark/` 通过；`git diff --check` 通过；诊断标记残留检查无匹配。
- 按 `-j11` 重建真正的 A+B 成功：`run_temp/query-c3-restored-build.log`。实际输出是 `build_release/src/observer/seekdb`，不额外复制大体积二进制。
- 重建程序的六组回归全部通过：`run_temp/query-c3-restored-regressions.tsv`。SHA256：`0b780889174f8fd185d86b774bb744a58e7716e06457da8bc5ec3fc04e05434d`，记录在 `run_temp/query-c3-restored-binary.sha256`。构建元信息不同，不声称该哈希与历史固定基线相同。
- 已重新验证固定 A+B 参考 SHA256 `6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c` 和 pre-A 参考 SHA256 `e390bb3367f6936a4c076b3b221f9e4910cd6346dc8b1c05762e80721e724bb0`，均未覆盖。
- 原始日志、配置、统计、诊断补丁、模型测试和二进制哈希保留。确认服务与构建进程均已退出后，删除两个本轮实例的可重建 store 及临时诊断二进制；保留当前增量构建树和实际重建的 A+B 程序。
