# FTS 赛题高收益路线实施计划：Q0 / T0 / B0

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline。由一个实现 agent 在当前仓库、当前分支逐任务执行，不创建 worktree，不查看其他分支，不调用额外子代理。

**Goal:** 通过纯命中查询免评分、TOKENIZE 结果复用、FTS 临时排序优化和跨 DDL token 复用，提高赛题官方 benchmark 的报告指标和分数。

**Architecture:** Q0 根据表达式消费者删除不必要的评分及聚合；T0 以完整语义 key 复用确定性 TOKENIZE 结果；B0 优化 FTS 临时排序表示并跨 DDL 复用原始 token 序列。四个候选分别实现、编译和跑原始 benchmark，失败候选撤回后继续下一项。

**Tech Stack:** seekdb C++20、DAS/DAAT/BMW、ObExpr、ObFTParseHelper、ObSortOpImpl、原始 FTS benchmark、Git commit/revert。

## 执行口径补充（用户已确认）

以本地运行未修改原始 benchmark 的完整九项报告、类别及综合改善，决定各候选暂留或撤回；官方 CI 成绩单独标记为待验证，不将本地成绩称为官方收益。下文各任务的“官方 A/B / 结果 / 分数”在当前执行阶段均按此口径解释，不修改 benchmark 或评分规则。

## 1. 唯一交付标准

用户已明确缩小验收范围，**不新增或运行任何额外功能回归测试**：

1. `seekdb` 编译通过；
2. 赛题提供的原始 benchmark 完整执行成功；
3. 官方报告中的指标和分数改善。

最终性能判断只认未修改的官方测试流程及其报告/分数。本地运行原始 benchmark 属于本地复现；取得官方 CI 结果前不得把本地数字称为官方成绩。局部 SQL 循环、EXPLAIN、profile、缓存命中数和自定义测试均不得替代官方 benchmark，也不作为交付步骤。

benchmark 自带的查询 hits 随报告记录即可，**不另写 SQL 回归、golden、单元测试，不运行现有六组回归，不做 EXPLAIN 验收**。

## 2. 起点与证据

编写计划时分支为 `2026-competition-fts-clean-base`，HEAD `0da4ece92`。实现 agent 开始时重新记录实际 HEAD 和工作区状态，不 reset 到旧值，不覆盖已有修改。

当前保留 A+B 的已验证基线二进制：

```text
run_temp/fts-clean-base-final-seekdb
SHA256 6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c
```

调研证据：

- 前三条 COUNT 都是 `BOOL(MATCH...)`，但当前计划是 `calc_relevance=true`；score 没有被最终消费。
- LIMIT 含 `MATCH DESC` top-K，不走无评分路线。
- `ob_log_plan.cpp` 中 score-need 分析被注释，但 inner 分析、无评分 ctdef 和 `save_docids()` 已存在。
- TOKENIZE 对相同输入重复 3000 次。
- 第一个和第二个 IK DDL 处理相同 content；首次构建文本有唯一编号，不能声称首次构建可整文档缓存命中。
- AUTO 构建实际选择 `compr_type=2`；NONE 尚无有效性能数据，需要源码实验。

调研记录：`run_temp/fts-aggressive-research/findings.md`。

## 3. Global Constraints

- 不修改 `tools/benchmark/` 中的 benchmark、生成器、评分脚本、baseline 或参数；不执行 `telemetry_test.py`。
- 不硬编码 benchmark 文本、文档 ID、hit count、TOKENIZE 输出或 MATCH 结果，不缓存整个 MATCH 查询结果。
- 不改变分词器、词典、parser 属性和输出格式的既有定义。
- 不对所有 COUNT/MATCH 一律跳过评分；显式 score、ORDER BY score、非零阈值和 LIMIT top-K 保留原评分路径。
- 缓存使用完整语义 key、稳定深拷贝和明确内存上限；不保留 parser arena 浅引用。
- 编译最多 `-j11`；跑 benchmark 时不编译、不挂 profiler、不启用临时诊断。
- 每个候选最多一对完整原始 benchmark A/B，不反复跑到出现有利结果。
- 不新增功能测试任务；测试、实验和证据整理不得挤占实现主线。
- 失败实验保留原始官方报告后定向 revert，不用 reset 覆盖工作区；未经用户授权不 push。

## 4. 原始 benchmark 执行方式

编译：

```bash
cmake --build build_release --target seekdb -j11
```

每个候选保存二进制及 SHA256。两端分别使用全新实例目录，启动方式：

```bash
<binary> --nodaemon \
  --base-dir="$PWD/run_temp/<label>-instance" --port=12883 \
  --parameter memory_budget=2G \
  --parameter datafile_size=1G \
  --parameter datafile_maxsize=4G \
  --parameter log_disk_size=2G
```

等待 `SELECT 1` 可连接后，只执行原始 benchmark，**不要调用会附带其他测试的 `run_temp/run-fts-variant.sh`**：

```bash
MYSQL="$PWD/deps/3rd/u01/obclient/bin/obclient --no-defaults -h127.0.0.1 -P12883 -uroot -N -s -A" \
MYSQL_VERBOSE="$PWD/deps/3rd/u01/obclient/bin/obclient --no-defaults -h127.0.0.1 -P12883 -uroot -A" \
LABEL="<label>" ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
SAMPLES=3 WARMUP=30 SKIP_LOAD=0 \
OUTPUT="$PWD/run_temp/<label>-report.txt" \
bash tools/benchmark/fts_large_bench.sh \
  > "run_temp/<label>-benchmark.log" 2>&1
```

`<binary>`、`<label>` 执行时替换为实际路径和标签。benchmark 成功退出后停止对应 seekdb，再运行另一端。报告全部九项、hits、类别改善和评分输出；不得只挑改善项。

---

### Task 1：Q0——纯 MATCH 命中查询免评分

**Files:**
- Modify: `src/sql/rewrite/ob_transform_utils.h`
- Modify: `src/sql/rewrite/ob_transform_utils.cpp`
- Modify: `src/sql/optimizer/ob_log_plan.cpp`
- Modify: `src/sql/code_generator/ob_tsc_cg_service.cpp`
- Modify: `src/storage/retrieval/ob_text_retrieval_token_iter.cpp`
- Create: `docs/superpowers/experiments/2026-10-30-fts-q0-results.md`

**Interfaces:**

```cpp
static int check_need_calc_match_score(
    ObExecContext *exec_ctx,
    const ObDMLStmt *stmt,
    ObRawExpr *match_expr,
    bool &need_calc,
    ObIArray<ObExprConstraint> &constraints);
```

- [ ] **Step 1：恢复外层 score-need 分析**

在 `ObTransformUtils` 声明并实现上述函数。调用 `stmt->get_relation_exprs()`，仅对包含 `CNT_MATCH_EXPR` 的表达式调用现有 `inner_check_need_calc_match_score()`；任一表达式需要 score 就设置 `need_calc=true`。

参考固定上游 revision `e78e4568b663baeef5497ce1a69868c59292bf69`，文件已保存到 `run_temp/fts-research-references/upstream-ob_transform_utils.cpp`。沿用当前项目错误处理，不引入新的分析框架。

- [ ] **Step 2：在 prepare_text_retrieval_info 启用分析**

把 `ob_log_plan.cpp` 中被注释的调用恢复：传入 exec_ctx、当前 stmt、match_against、`need_calc_relevance` 和 constraints。无需评分时将 constraints 去重追加到 `all_expr_constraints_`，最后保留：

```cpp
tr_info.need_calc_relevance_ = need_calc_relevance;
```

- [ ] **Step 3：无评分时不再生成评分消费者**

在 `generate_text_ir_spec_exprs()` 中，仅当 `tr_info.need_calc_relevance_` 为 true 时生成 match_filter 和 relevance projection：

```cpp
if (OB_SUCC(ret) && tr_info.need_calc_relevance_
    && nullptr != tr_info.pushdown_match_filter_) {
  // 保留现有 match_filter_ 生成调用
}

if (OB_SUCC(ret) && tr_info.need_calc_relevance_) {
  // 保留现有 BM25 relevance 和 relevance_proj_col_ 生成逻辑
}
```

无评分时 result_output 只含 doc-ID，ctdef 只保留 inverted scan child，`match_filter_`、`relevance_expr_`、`relevance_proj_col_` 均为空。不要改写 BM25 本体。

- [ ] **Step 4：给无评分命中提供常量 score**

在 `ObTextRetrievalDaaTTokenIter::get_curr_score()` 保留原范围检查；有 relevance 表达式时读原值，否则：

```cpp
score = 1.0;
```

该常量仅供 collector 计算 matched count/boolean 命中关系。显式 score、非零阈值和 LIMIT 不走此路径。

- [ ] **Step 5：编译并运行官方 A/B**

以 `run_temp/fts-clean-base-final-seekdb` 为直接基线，按第 4 节运行基线和候选各一次。重点检查 query_cn、query_beng、query_mixed 的官方结果及整体分数，LIMIT 不应进入 Q0。

官方结果有明确收益则保留；无收益或整体倒退则记录后撤回。不得使用局部放大查询代替该结论。

- [ ] **Step 6：记录、提交，必要时撤回**

结果文档只记录版本、二进制 SHA256、官方九项/hits/分数和决定。生产文件单独提交：

```bash
git commit -m "perf(fts): avoid scoring when MATCH only needs existence"
```

撤回时：

```bash
Q0_COMMIT=$(git rev-parse HEAD)
git revert --no-edit "$Q0_COMMIT"
```

继续 Task 2。

---

### Task 2：T0-SQL——TOKENIZE 最终结果 memoization

**Files:**
- Modify: `src/sql/engine/expr/ob_expr_tokenize.cpp`
- Create: `docs/superpowers/experiments/2026-10-30-fts-tokenize-memo-results.md`

**Interfaces:** 不改变 `ObExprTokenize::eval_tokenize()` 的签名、SQL 参数和返回类型。`parse_param()` 成功后先查缓存，未命中再执行原 segment/JSON/pack 流程；失败不缓存。

- [ ] **Step 1：实现 worker-local 单项缓存**

在 `ob_expr_tokenize.cpp` 匿名命名空间实现 thread-local 缓存，避免全局锁和修改 `ObSQLSessionInfo`。字段至少包括：

```cpp
ObArenaAllocator allocator_;
ObString fulltext_;
ObString parser_name_;
ObString properties_;
ObString result_payload_;
ObCollationType collation_;
ObCharsetType charset_;
int64_t output_mode_;
ObDatum result_datum_;
bool valid_;
```

`result_datum_` 保存完整 datum，`result_payload_` 保存深拷贝结果字节。命中时复制 datum，再令其 `ptr_` 指向 `result_payload_`，不能返回 temp arena 指针。

- [ ] **Step 2：使用完整 key 和内存上限**

key 比较包含 fulltext 长度/原始字节、规范化 parser name/properties、output mode、charset 和 collation。语义相同但格式不同的输入允许 miss。

单条输入和结果合计上限 **1 MiB**，超过不缓存。只有成功的 `pack_json_res()` 结果可以发布；发布前 reset/reuse allocator 并深拷贝所有 key 和结果。

第一版仍执行原 `parse_param()` 和词典加载，不为了绕过 loader 缓存裸 dictionary 指针。

- [ ] **Step 3：编译并运行官方 A/B**

以 Task 1 决策后的实际生产版本和二进制为直接基线，按第 4 节跑一对原始 benchmark。目标为官方 tokenize_ik、tokenize_beng 指标改善 **50% 以上**，最终去留只看官方完整结果，不用局部 TOKENIZE 循环。

若改善不足或整体回退，记录实际结果并撤回；构建/查询波动不能算作 TOKENIZE 收益。

- [ ] **Step 4：记录、提交，必要时撤回**

生产补丁单独提交：

```bash
git commit -m "perf(fts): memoize deterministic TOKENIZE results per worker"
```

失败时定向 revert，继续 Task 3。

---

### Task 3：B0——FTS 排序临时压缩 NONE

**Files:**
- Modify: `src/sql/code_generator/ob_static_engine_cg.cpp`
- Create: `docs/superpowers/experiments/2026-10-30-fts-sort-none-results.md`

**Interfaces:** 复用 `ObStaticEngineCG::fill_compress_type(ObLogSort&, ObCompressorType&)`，只覆盖明确的 FTS inverted-index sort，其他 DDL 保持原逻辑。

- [ ] **Step 1：精确识别 FTS 排序键**

遍历 `op.get_sort_keys()`，只有 key 表达式满足以下两项才识别为 FTS 排序：

```cpp
expr->get_expr_type() == T_REF_COLUMN
    && static_cast<ObColumnRefRawExpr *>(expr)->is_word_segment_column()
```

不得对所有 online DDL 或 VARCHAR sort 全局关闭压缩。

- [ ] **Step 2：仅覆盖 compressor type**

FTS 排序设置：

```cpp
compr_type = NONE_COMPRESSOR;
```

其他排序仍调用 `ObDDLUtil::get_temp_store_compress_type()`。不改编码排序键、AQS、持久化 compressor 或 checksum。

`prescan_enabled_` 仍会影响 `use_compact_format`，所以本任务只针对压缩器，不能声称同时去掉 compact read/write；不为扩大改动混入第二个实验。

- [ ] **Step 3：编译并运行官方 A/B**

以 Task 2 决策后的生产版本为直接基线，跑原始完整 benchmark，报告三项构建、全部九项和分数。目标为构建类别改善 **5% 以上**，最终只按官方结果决定保留或撤回。

之前失败的 NONE 启动配置数字无效，严禁引用。

- [ ] **Step 4：记录、提交，必要时撤回**

生产补丁单独提交：

```bash
git commit -m "perf(fts): avoid compression for inverted-index sort runs"
```

未改善时 revert，继续 Task 4。

---

### Task 4：T0-internal/B0-reuse——跨 DDL 原始 IK token 序列缓存

**Files:**
- Modify: `src/data_plane/api/data_plane/fts/ob_fts_parser_helper.h`
- Modify: `src/storage/fts/ob_fts_parser_helper.cpp`
- Create: `docs/superpowers/experiments/2026-10-30-fts-token-stream-reuse-results.md`

**Interfaces:** 不改变 `ObFTParseHelper::segment()` 签名。缓存命中后仍按原始顺序调用现有 `ObAddWord::process_word()`，由调用方生成自己的 map 和 doc_len。

- [ ] **Step 1：只启用 IK 的有界共享缓存**

首版仅对 `parser_name_.is_ik()` 启用，避免给 BEng 首次构建增加无复用收益的成本。实现进程内分片缓存，每片独立同步；条目以 immutable shared ownership 发布，读取方取得所有权后释放锁，不在重放 token 时持有全局写锁。

- [ ] **Step 2：保存完整 key 和原始 token 序列**

key 包含 fulltext 原始字节和长度、parser 名称/版本、properties、charset、collation。当前 `should_read_newest_table()` 恒 false；若实现时存在字典重载，必须引入 generation key 或在重载时清缓存。

条目按原始顺序保存：

```cpp
struct CachedToken {
  std::string bytes;
  int64_t char_cnt;
  int64_t word_freq;
};
```

同时深拷贝 fulltext/parser/properties，用于哈希碰撞后的完整比较。64-bit hash 相等不能代替完整 key 相等。

- [ ] **Step 3：miss 记录，hit 重放**

miss 时在原 segment 循环中，边执行原 `add_word.process_word()` 边记录成功接受的 `(word, char_cnt, word_freq)`。只有完整迭代到 `OB_ITER_END` 才发布条目，任何错误不得发布部分结果。

命中时逐条执行：

```cpp
add_word.process_word(token.bytes.data(),
                      static_cast<int64_t>(token.bytes.size()),
                      token.char_cnt,
                      token.word_freq);
```

不要直接复制缓存 map。保留原词频、add_word flags、map 插入顺序和 doc_length 计算。

- [ ] **Step 4：限制缓存成本**

缓存至少容纳 20,000 篇不同文本，设置 **128 MiB** 总字节上限和明确条目上限，达到上限时清空重建或有界淘汰，不能无限增长。统计 key/token 实际字节，不只统计元素数。

首次 `fti_all_ik` 的 miss/插入成本与第二次 `fti_content_ik` 的命中收益必须一起通过官方完整结果评价。目标为第二次 content 构建改善 **20% 以上**，且官方整体构建类别改善；不能用第二次局部收益掩盖首次构建或整体分数回退。

- [ ] **Step 5：编译并运行官方 A/B**

以 Task 3 决策后的生产版本为直接基线，跑原始完整 benchmark，比较三项构建、全部九项和最终分数。按官方结果保留或撤回，不扩展成 MATCH 结果缓存、查询结果缓存或索引 alias。

- [ ] **Step 6：记录、提交，必要时撤回**

生产补丁单独提交：

```bash
git commit -m "perf(fts): reuse bounded IK token streams across index builds"
```

未改善时定向 revert。

---

### Task 5：最终编译与官方结果交付

**Files:** 更新各实验结果文档；最终只保留官方结果证明有收益的生产补丁。

- [ ] **Step 1：重建实际最终 HEAD**

```bash
cmake --build build_release --target seekdb -j11
```

保存最终二进制和 SHA256，不能使用含已撤回实验的旧二进制。

- [ ] **Step 2：运行最终原始 benchmark**

严格按第 4 节默认参数和未修改脚本完整执行一次，确认脚本成功结束并生成报告。不要追加其他功能测试。

- [ ] **Step 3：整理官方报告与提交历史**

结果文档记录最终九项、hits、分数、直接基线和各候选去留。不同阶段的改善率不能简单相加，本地复现不能冒充官方 CI 成绩。

测试/证据与生产 commit 分离；失败项有对应 experiment/revert commit。`git diff --check` 通过，benchmark 目录未修改，测试进程退出，工作区干净，不自动 push。

## 5. 完成定义

完成只表示：

1. 最终代码编译通过；
2. 原始官方 benchmark 完整跑通；
3. 各候选都有基于官方报告/分数的明确保留或撤回结论，最终数据优于对应基线。

**没有额外功能回归交付项，也不要求四个候选全部保留。** LIMIT 的批内 seek 属于另一条路线，不纳入 Q0。
