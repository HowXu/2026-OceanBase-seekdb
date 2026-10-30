# FTS 后续优化实施计划：A 收尾 → A.5 IK 编码复用 → B 构建排序 → C 查询

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. 用户已明确要求在当前分支/会话执行，不创建 worktree、不调用子代理。用户已于 2026-10-30 授权 continue，允许持续单线执行、编译最高 -j11；按下述关卡执行构建、评测、提交及必要的 revert。

**Goal:** 先重新核实并妥善留档当前 A 实验，再以不改变分词及 MATCH 语义为前提，推进 IK 字符编码复用、建索引排序和查询批处理优化。

**Architecture:** 每次只保留一个待评估补丁，关闭 profiler 后与其直接基线做一对完整 A/B。近乎无收益或倒退的实验先提交留档，再定向 revert；回归测试、测量记录和本路线图独立保存。Friso 仅提供实现思路，不替换 IK/BEng 算法或词典。

**Tech Stack:** seekdb C++20、现有 DAT/哈希表/encoded-sort-key/DAAT/BMW、SQL 回归、原始 FTS benchmark、Git commit/revert。

## 1. 用户要求与执行顺序

将“放在 A 之后、B 之前”解释为插入上轮 Friso 调研得出的 **IK 字符到字典 code 的查询复用**，命名为 A.5。

```text
A：当前词频 map 实验复测
  ├─ 有明确收益 → 提交保留
  └─ 几乎无收益 / 倒退 → 提交实验与真实结果 → revert 优化代码
        ↓
A.5：Friso 借鉴的 IK 字符编码复用，仅一个小实验
        ↓
B：建索引排序 / collation 比较，主攻方向
        ↓
C：查询批内归并 / 批量过滤，主攻方向
```

| 阶段 | 定位 | 工作预算与退出条件 |
|---|---|---|
| A | 必须先收尾 | 只追加一对完整 A/B；不继续展开字典 loader、容器或 allocator 优化 |
| A.5 | 指定插入的小实验 | 最多一个候选补丁、一对完整 A/B；若字典身份与生命周期无法安全约束，记录原因并进入 B |
| B | 高优先级主线 | 优先复用现有编码排序键，先解释未启用/回退原因，再决定补丁 |
| C | 高优先级主线 | B 的最小候选得出结论后立即推进；B 若必须升级为大型重构，则先推进 C 的小补丁 |

**优先级提升的含义：** B/C 获得主要调研和实现投入，不再因分词引擎移植、泛化框架或反复微优化而推迟；不改变用户指定的 A.5 插入位置。

## 2. Global Constraints

- 当前分支：`2026-competition-fts-clean-base`，从 `3ced67e17e01d9c86ad9474f9028af23451a12bf` 创建，仅 cherry-pick `bc6dfc916440a274a2364b56b6f7ade7a99064c1`，得到 `1bf3c8435460d67d736bb5d218f6c5e453c49721`。已验证两者 tree 都是 `57c4df55d00fada77381f3ba9cb6b2e9f8890df9`；原已保存二进制仍对应同一源码。旧分支不修改。
- 所有编译并行度 **最多 `-j11`**（2026-10-30 用户更新）。用户已编译且二进制对应当前源代码时，直接测试，不重复启动构建。
- 不创建 worktree、不调用子代理；保留用户的所有其他改动。
- 不执行 `telemetry_test.py`。
- 不修改 `tools/benchmark/` 下的 benchmark、输入生成器、评分脚本或基线文件。
- 不降低输入规模、不省略必要处理、不改变词典/分词规则、不缓存固定测试文本或查询结果、不硬编码答案。
- 保持 UTF-8 校验、四字节字符、内嵌 NUL、长词、collation、停用词、词频、doc_len 和词条生命周期语义。
- MATCH 保留 BM25、过滤、重复词处理与相关度语义；不能因为是 COUNT 就无条件跳过评分，不能因为有 LIMIT 就提前截断必要 posting。
- 每个候选默认只跑 **一对完整 A/B**：基线一次、候选一次。benchmark 自带 `SAMPLES=3` 保持不变，不自动扩为多轮全量测试。
- 性能验收不挂 profiler、不带临时热路径插桩；优先使用已有的一份完整 profile，不重新批量采集/展开 DWARF 内联栈。
- 不以降低全局锁保护替代所有权证明；不重做已有小 K 归并、BMW 或字符类型缓存。
- 不执行 `git reset --hard`、清理用户文件或自动 push。失败实验使用指定 SHA 的 `git revert`，不是清空工作区。

## 3. 已有证据与尚未证明的部分

### 3.1 当前 A 状态

当前优化改动尚未提交，涉及：

1. `src/data_plane/api/data_plane/fts/ob_fts_struct.h`
2. `src/storage/fts/ob_fts_stop_word.cpp`
3. `src/sql/optimizer/ob_join_order.cpp`
4. `src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp`
5. `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`

回归资产：`unittest/storage/fts_word_map_regression.sql`、对应 `.result`，以及 `fts_parser_regression.sh` 的新增测试项。

已经修正并覆盖一个真实兼容性问题：在 general_ci 下，等价词条的聚合必须保留旧实现的“最后一次出现的拼写”，不仅更新词频。不得再退回只更新 value 的回调。

已保存二进制：

- 基线：`run_temp/route-a-baseline-seekdb`，A 之前的 `bc6dfc916` 实现。
- 候选：`run_temp/route-a-candidate-seekdb`，包含上述拼写覆盖修复。

首组完整 A/B 的事实如下，仅作为复测前记录，不认定为稳定代码回退：

| 指标 | 基线 | A 候选 |
|---|---:|---:|
| build_ik_all_sec | 6.453 | 6.142 |
| build_ik_content_sec | 4.947 | 4.951 |
| build_beng_en_sec | 2.836 | 2.745 |
| tokenize_ik_avg_ms | 0.0683 | 0.2392 |
| tokenize_beng_avg_ms | 0.0367 | 0.0951 |
| query_cn_avg_ms | 1.4257 | 1.4053 |
| query_beng_avg_ms | 1.9285 | 2.0290 |
| query_mixed_avg_ms | 1.6040 | 1.4770 |
| query_limit_avg_ms | 1.7892 | 1.8997 |

四组回归输出相同，hits 为 `8001 / 11000 / 7332 / 20`。BEng 同次三样本从 `0.1667 → 0.0777 → 0.0408 ms` 回落；IK 三样本均偏慢。必须区分“本次观测慢”与“已证明优化导致稳定回退”。

### 3.2 B/C 热点证据

- 既有 profile 中构建排序调用链约占 27%–31% CPU 样本，collation 比较约 17%–20%，两者重叠；不是墙钟耗时比例，不能相加。
- 查询主要经过公共 DAAT/BMW 路径；分词不是完整查询的主要 CPU 开销。
- 阶段边界由日志接收时间粗分，短 TOKENIZE 窗口存在边界串扰，不据其百分比承诺收益。
- 模板相同代码折叠会让不同 `ObIArrayWrap::at()` 共用符号地址，不能仅凭 HashSet 模板名称判定集合运算热点。

## 4. 统一评测和去留规则

### 4.1 固定配置

```text
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200
SAMPLES=3 WARMUP=30 SKIP_LOAD=0
memory_budget=2G datafile_size=1G datafile_maxsize=4G log_disk_size=2G
独立测试实例端口：12883
```

每次必须完整建库、装载并建立三个索引；不拿 `SKIP_LOAD=1` 的诊断结果代替构建项成绩。沿用已验证的直接启动方式，不恢复已失效的 OBD 部署路径。

### 4.2 指标和判定

各耗时项改善率为 `(baseline - candidate) / baseline`；先分别平均构建 3 项、分词 2 项、查询 4 项，再平均三个类别。输出全部九项、三个类别和综合改善率，不挑最好样本。

本计划默认将 **综合改善不足 2%** 视为“接近零/尚不足以保留额外复杂度”的工程门槛，而不是赛题评分规则或统计显著性判断。保留还要求目标类别改善，且没有未解释的明显类别回退；否则记录真实结果并撤销该优化。如果单次环境扰动使结果无法解释，标为“未证明收益”，不自动追加多轮测试。

- 正确性不一致：停止性能判定，先修正；无法保留语义则撤销。
- A 复测综合改善 ≤ 0，或接近零/无法证明收益：先留档提交 A，再 revert A。
- 有明确正收益且通过上述检查：提交并保留。
- 所有结果只相对于本机直接基线，不换算成保证可获得的官方分数。

## Task 1：只复测一次 A，并给出决定

**Files:** 读取当前五个 A 生产文件、四组回归；新产物只写 `run_temp/`。

**Interfaces:** 输入为当前候选及 `bc6dfc916` 基线二进制；输出为复测前/后报告、九项比较、保留或 revert 决定。

- [ ] 记录 `git status --short`、`git diff`、二进制 SHA256；确认候选是含拼写覆盖修复的版本，而不是最早未修正版本。
- [ ] 若当前二进制已对应代码，不编译。确需构建时仅执行：

```bash
cmake --build build_release --target seekdb -j11 \
  > run_temp/route-a-recheck-build.log 2>&1
```

- [ ] 确认没有残留构建/profiler/12883 测试服务，避免编译与评测并行；不修改系统调度、缓存或内核参数来美化结果。
- [ ] 使用现有临时启动助手，只执行下列一对完整评测；它包含四组回归和原始 benchmark：

```bash
bash run_temp/run-fts-variant.sh route-a-recheck-baseline run_temp/route-a-baseline-seekdb
bash run_temp/run-fts-variant.sh route-a-recheck-candidate run_temp/route-a-candidate-seekdb
```

- [ ] 检查两次完整结束、四组回归无差异、hits 正确，并报告各样本分布及 `SELECT 1` 基础耗时；不把第一轮或本轮任一有利结果藏掉。
- [ ] 使用第 4 节公式生成比较结果，作出一次明确的保留/撤回决定，不循环跑到出现正收益为止。

## Task 2：提交证据，并按结果保留或 revert A

**Files:** 创建 `docs/superpowers/experiments/2026-10-29-fts-route-a-results.md`；保存回归测试；A 生产补丁单独成 commit。

**Interfaces:** 消费 Task 1 的实测结果；输出持久化证据、A 实验 SHA、必要时的 revert SHA，以及下一阶段干净的直接基线。

- [ ] 在结果文档写入两组测量的全部九项、固定配置、二进制 SHA256、综合改善率、正确性结论、环境疑点和最终决定。负收益如实记录，不命名为已成功的性能提升。
- [ ] 先将独立于优化的回归测试、结果文档、本路线图和 A 原计划提交留档。回归 golden 来源是 A 之前已验证的输出，撤销优化后仍可运行。只 stage 这些已检查文件，不使用 `git add .`。
- [ ] 再只提交下列五个 A 生产文件，记录实际 SHA：

```bash
git add src/data_plane/api/data_plane/fts/ob_fts_struct.h \
  src/storage/fts/ob_fts_stop_word.cpp \
  src/sql/optimizer/ob_join_order.cpp \
  src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp \
  src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp
git diff --cached --check
git diff --cached --stat
git commit -m "experiment(fts): record private word-map aggregation candidate"
A_COMMIT=$(git rev-parse HEAD)
```

- [ ] 若 Task 1 判定接近零、倒退或未证明收益，立即撤销该生产补丁：

```bash
git revert --no-edit "$A_COMMIT"
git diff --exit-code bc6dfc916 -- \
  src/data_plane/api/data_plane/fts/ob_fts_struct.h \
  src/storage/fts/ob_fts_stop_word.cpp \
  src/sql/optimizer/ob_join_order.cpp \
  src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp \
  src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp
```

- [ ] 若有冲突，停下检查，不用 reset 或强制覆盖。若保留 A，则不执行 revert，并将该已验证提交作为下一阶段的直接基线。
- [ ] revert 后若继续使用源构建产物，先 `-j11` 增量构建并运行四组回归；不能把仍含 A 的旧候选二进制当作已恢复基线。只保存一次新基线快照，不额外跑一对完整 benchmark。

## Task 3：A.5——Friso 借鉴的 IK 字符编码复用短实验

**Files:** 首选定位 `src/storage/fts/dict/ob_ft_dat_dict.{h,cpp}`、`ob_ft_cache_dict.{h,cpp}`、`ob_ft_range_dict.{h,cpp}`、`src/storage/fts/ik/ob_ik_cjk_processor.{h,cpp}`；仅修改经调用链证明必需的文件。扩展 `unittest/storage/fts_ik_chain_regression.sql` 及 `.result`。

**Interfaces:** 保持现有 `ObIFTDict::match_with_hit()` 和 IK 对外输出契约。内部只复用“当前字符在特定稳定字典分片中的 code 查询结果”，不跨不受保护的 cache block 生命周期，不把不同分片的 code 当作同一编码。

- [ ] 先确认同一输入字符的多个活跃 hit 是否实际访问同一 map，以及 cache handle 能否保证这段调用期间 map 稳定。若无法建立安全复用域，本任务记录不可行后结束，直接进入 B。
- [ ] 在直接基线上记录 smart/max_word 详细词条与词频，覆盖重复中文、重叠前缀、长歧义串、四字节 CJK、emoji、NUL、混合文本，以及同一字符在不同字典分片上的匹配。
- [ ] 先用测试/局部计数证明“相同稳定 map + 相同当前字符”存在重复编码查询，再实现一个最小复用补丁。明确区分 code 未找到与合法 code，推进到下一字符或分片失效时清除复用状态。
- [ ] 保留所有候选词、字节 offset、char_cnt、输出顺序、smart/max_word 仲裁；不改 Friso 的 MMSEG/FMM、不加载 Friso 词库、不限制为 5 字词、不引入 64 字节 token 上限。
- [ ] 不把已存在的字符长度/类型缓存再次当作新优化；不复用已返回词条的可变底层缓冲区。
- [ ] 编译最多 `-j11`，通过完整回归后仅跑一对完整 A/B；比较所有九项，而不只展示 IK 内层微基准。
- [ ] 得出保留/撤回决定，保存结果并提交；失败同样按“证据独立、优化 commit 可定向 revert”处理。不继续扩大为引擎移植、全词典重编码或多个缓存层。

## Task 4：B——优先推进 FTS 构建排序快路径

**Files:** 首先检查 `src/sql/code_generator/ob_static_engine_cg.cpp`、`src/sql/engine/sort/ob_sort_op.{h,cpp}`、`src/sql/engine/sort/ob_sort_op_impl.{h,cpp}`。新增聚焦测试：`unittest/storage/fts_build_sort_regression.sql` 及 `.result`，接入现有回归 runner。

**Interfaces:** 复用现有 `ObSortSpec::enable_encode_sortkey_opt_`、`ObSortOpImpl::Compare` 和 `sort_inmem_data()` 的编码键/AQS 分支。原排序在 collation、NULL、升降序、相等项上的语义不变；不添加一个平行的通用排序引擎。

- [ ] 查清 FTS DDL 的排序键序列、collation 与编码路径：未启用，或启用后 `can_encode` 回退。将真实原因和所在条件写入 B 的结果记录，再决定代码补丁；不能先假设只需打开 bool。
- [ ] 现有 trace/plan 不能说明原因时，允许一次任务级诊断：记录是否启用、回退原因、排序键类型、行数和比较数；线程/实例私有计数、任务结束汇总一次。没有现成廉价 unique-term 计数时不另建集合统计它。
- [ ] 临时诊断不提交到正式候选、不逐词打日志，不再次启动大范围 profiler 采集。诊断完移除，再跑性能验收。
- [ ] 构建测试覆盖 general_ci 下的大小写/重音等价、binary collation、四字节字符、重复词、多字段、不同 doc-ID、空/NULL 文本，并验证索引后的完整 MATCH 结果与相关度。
- [ ] 若 FTS 排序满足编码键契约而被保守禁用，最小范围复用原快路径；若是正确性限制，保留回退，评估缓存 collation key，不直接换成 raw-byte memcmp。
- [ ] 编码键能否复用尚未验证，故本任务不预写一个凭空的开关补丁。证据要求大规模新 posting accumulator 或持久化结构调整时，记录为后续方案，先推进 C，避免在 B 内无限重构。
- [ ] 最小候选通过全部回归后，以最近一个保留的优化版本为直接基线，只跑一对完整 A/B；重点报告构建三项与另外两类是否回退。CPU 热点比例不作为预计得分。
- [ ] 保存结果、提交并作去留决定，随后立即进入 C。

## Task 5：C——优先推进查询批内归并和过滤

**Files:** `src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}`、`src/storage/access/ob_simple_rows_merger.h`（只有确需修改时）；必要时检查 `src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp`。新增 `unittest/storage/fts_query_batch_regression.sql` 及 `.result` 并接入 runner。

**Interfaces:** 保持 `get_next_rows(capacity, count)`、`fill_merge_heap()`、`collect_dims_by_id()`、`filter_on_demand()` 的输出/错误/结束契约；不改变 BM25 及浮点累加顺序来获取表面速度。

- [ ] 使用已有调用栈按实际调用点核对数组访问、虚调用、doc-ID 解码、归并和 filter 开销，不把 ICF 折叠后的模板符号名当作真实数据结构证据。
- [ ] 先补查询回归：单词、多词、重复词、布尔模式、多字段、无命中、NULL、LIMIT/OFFSET、显式相关度排序和 tie；对比完整结果 ID、相关度及 order-by 的结果顺序。未指定 ORDER BY 的 SQL 不要求无契约的行顺序一致。
- [ ] 第一个候选仅验证批内稳定指针/游标复用，减少重复 `at()`、虚调用或单行归并调度；不重做 Fqin 已有的小 K 分派，不扩大一个“快速路径词数阈值”冒充批处理。
- [ ] 若第一个候选不能形成有依据的小补丁，转向逐行 filter 的批量执行：先证明 evaluated flags、batch_idx、过滤后紧凑投影、错误和表达式求值语义保持一致，再落补丁。
- [ ] 若上述两个候选都值得做，拆成两个独立实验，逐个通过回归和一对 A/B，不在同一个候选中同时修改归并与过滤。
- [ ] 保留完整评分与过滤；LIMIT 已有 BMW，不另以“增加 WAND”为新方案。一般浮点 MATCH 谓词专门化不纳入第一补丁。
- [ ] 按同一九项指标与类别公式作去留决定；结果独立提交，不将查询收益误归因于 IK/BEng 字符解析。

## 5. 测试资产和产物放置

- 正确性测试留在 `unittest/storage/`，不在 `tools/` 添加测试或 benchmark 文件。
- 原始测量和二进制快照留在已忽略的 `run_temp/`；提交的结果文档必须包含关键数据及复现命令，不能只链接到可能被清理的临时文件。
- 路线结果文档统一放 `docs/superpowers/experiments/`，每份写明直接基线、候选、固定参数、九项数据、正确性结果、保留/revert 决定。
- 本路线图与原 A 计划保留在 `docs/superpowers/plans/`，不能随 A 的生产补丁一起 revert。
- 外部参考固定 revision：Friso `b1808a4716dd07948154f3c8239ed34372dd0446`；PostgreSQL `76a55702d89ba9f5d0198c2d5d3d2d62357a8880`；MySQL `06bcb0eb0ad281e08dd2b4d81b99ad25491433e7`。借鉴数据布局和批处理，不复制其他引擎的分词/比较语义。

## 6. 开始执行前的自检

- [x] 用户已指示 continue，按 A → A.5 → B → C 单线执行；采用“近零默认不足 2%”工程门槛，编译上限更新为 -j11。
- [ ] 确认不再按旧 A 计划要求自动跑三对全量测试。
- [ ] A 复测先完成，A 的提交/必要时 revert 先完成，之后才改下一条路线。
- [ ] 各实验的代码、回归和记录分离足够清楚，失败优化能定向撤回且不丢测试、证据或用户改动。
- [ ] 只用最多 `-j11`，不启动子代理/worktree，不修改 benchmark，不用局部微基准替代完整成绩。
