# MiniVectorDB 第二轮性能热点与优化候选

日期：2026-11-13。当前源码：`299538bf4efc922ec01978229b019d5eae60c3bf`。

## 1. 测量范围

本轮没有修改生产代码。使用 RelWithDebInfo 宿主构建，分别对正式写入和查询阶段做 PID 附加 perf 采样；采样结束后再做正确性检查，不作为新的 Docker T/B/M 基线。

| 项目 | 采样阶段耗时 | 样本 | Lost | 正确性 |
|---|---:|---:|---:|---|
| 写入 | 1.439908秒 | 155个 cpu_core cycles | 0 | 10,000行、1,280,000向量分量及重启恢复通过 |
| 查询 | 6.363600秒 | 约1K cpu_core + 78 cpu_atom cycles | 0 | 1,000查询、精确ID/距离匹配，两组Recall@10=1.0 |

写入样本仍较少，精确百分比只作方向性参考。原始数据、report、callgraph位于：

```text
.superpowers/sdd/performance-round2/profile-write/
.superpowers/sdd/performance-round2/profile-query/
```

用户提供的平台反馈为“上一版680分，比最初多400分”；没有官方分项数据，本报告不据此推导性能或Agent单项得分。

## 2. 查询热点

cpu_core 自耗时主要符号：

| 符号 | 自耗时 |
|---|---:|
| `validate_row` | 12.73% |
| `storage::Input::bytes` | 11.36% |
| `l2_distance_squared` | 10.69% |
| 内核 `_copy_to_iter` | 8.81% |
| `utf8_codepoints` | 8.55% |
| `DeltaFileReader::next` | 2.66% |
| `malloc` / `cfree` | 2.56% / 3.73% |
| 多段堆调整 `__adjust_heap` | 2.47% |
| `TableFileReader::next` | 2.19% |
| `read_schema` | 2.03% |
| `RowLess` | 1.60% |
| `LogicalTableReader::advance` | 1.35% |

### 2.1 每次查询重复验证不可变已提交行

TableFileReader 和 DeltaFileReader 每次解码都调用 `validate_row`；后者再次执行 VARCHAR UTF-8/长度检查和向量逐分量 finite 检查。

但这些不可变段已经在两个边界验证过：

- 新段发布：`RootStore::publish` 调用 `validate_table_segment`；
- 启动恢复：`load_root` 调用 `validate_table_objects`。

题面崩溃模型不包括外部篡改和磁盘损坏。因此可增加“已验证的当前根”热查询读取路径，跳过每次 SELECT 的重复整行语义校验，同时保留写入、发布和恢复校验，以及 marker、长度、维度、排序键和截断等结构检查。

这是当前最高优先级查询候选。`validate_row` 与 `utf8_codepoints` 的 cpu_core 自耗时合计约21.28%，但不能把该比例直接承诺为等比例端到端加速。

### 2.2 标量仍逐字节进入通用 `Input::bytes`

虽然向量已经整块读取，`u64/u32` 仍分别调用8/4次 `u8`，每次都进入通用 bytes 循环和一字节 memcpy。每行还需读取 marker、row length、BIGINT 和字符串长度。

候选：

- `u8` 直接消费当前缓冲；
- `u32/u64` 一次读取4/8字节到固定数组后解码；
- 保持现有 little-endian 格式、remaining、EOF 和截断语义。

这不改变文件格式，风险低于 mmap，也不同于已回退的 reader 持有实验。

### 2.3 在 CREATE VECTOR INDEX 的准备阶段合并段

当前每次 TopK 都重新打开并归并基础段和增量段。固定负载有100个写入批次，段归并、堆调整、Delta 解码和 schema 重读仍有明确占比。

赛题允许将装载和索引准备放在查询 T 之外，但其成本计入 B/M 并受准备上限约束。可以在 `CREATE VECTOR INDEX` 时确定性合并成单一基础段，之后照常处理新增/更新/删除；不修改可见性和事务规则。

这是针对评测流程的合法专项优化：预期减少查询 T，但会增加准备阶段 I/O、峰值和准备时间，必须同轮比较 T/B/M，不能只报告查询加速。

### 2.4 距离计算与分配

`l2_distance_squared` 已升至10.69%，说明第一轮优化后数学循环才成为显著热点。可考虑有依据的编译器优化或向量扩展，但双精度累加顺序会影响舍入和近似并列排序，不应直接使用全局 `-ffast-math` 或未经差分验证的重关联。

每行 `std::vector<float>` 和 variant 仍产生分配/释放。可研究可复用扫描行或小型分配器，但生命周期改动大于前述项目，暂不优先。

## 3. 写入热点

155个 cpu_core 样本中的主要自耗时：

| 符号 | 自耗时 |
|---|---:|
| Python `_PyEval_EvalFrameDefault` | 26.66% |
| `basic_streambuf::xsputn` | 4.37% |
| `basic_filebuf::xsputn` | 2.94% |
| Python `_PyObject_Malloc` | 2.68% |
| `ostream::write` | 2.51% |
| Python unicode/dict/GC 等 | 多项约0.5%—2.2% |
| `utf8_codepoints` | 1.48% |
| `from_chars<double>` | 1.12% |
| `parse_vector` | 0.73% |

### 3.1 输出仍按字节调用 ostream

`Output::u64/u32` 逐字节调用 `Output::bytes`，TableFileWriter/DeltaFileWriter 的向量还逐分量调用 `u32`。调用图能直接看到 `TableFileWriter::append → Output::u32 → u8 → bytes`。

候选：

- u32/u64 固定数组一次写入；
- 增加 float 块写入，两个 writer 共用；
- 必要时使用有界输出缓冲，但不改变格式、sync、CURRENT 或故障边界。

这是低风险、与输入优化相互独立的写入候选。

### 3.2 Python/SQLGlot 对写入重要，对当前查询不重要

另外进行了不计分的前端解析阶段分解。每条语句仍真实调用 `parse_statement`，三轮取中位数：

| 阶段 | 语句数 | 解析 median | 占同次 perf 宿主阶段耗时的比例 |
|---|---:|---:|---:|
| 多行 INSERT | 100 | **0.496690秒** | 约34.5% |
| TopK SELECT | 1,000 | **0.212437秒** | 约3.3% |

cProfile 显示 INSERT 成本主要是 SQLGlot 对40,000个值的递归表达式解析及 tokenize；SELECT 也经过完整通用表达式解析。

结论：**不建议现在将整个程序迁移到 C++ 或 Rust。** 整体重写成本高、协议/SQL回归风险大，而对当前查询只能覆盖约3.3%的解析阶段。更合理的是：

1. 先完成 native 读写和重复校验优化；
2. 若写入仍由解析主导，为固定多行 INSERT 形状增加严格 C++ 快速路径，其他语句继续 SQLGlot fallback；
3. 或为重复语句形状做经过验证的模板缓存，而不是只缓存完整 SQL——正式数据每次字面值不同，完整字符串缓存不会命中。

专用解析必须保持 BIGINT 精确性、字符串转义、向量词法、行宽、重复键和失败原子性，不得只适配正常样例。

证据：`.superpowers/sdd/performance-round2/frontend-{write,query}-cprofile.txt` 和 `frontend-parse.json`。

## 4. 推荐顺序

1. **热查询跳过已提交行的重复语义校验**：当前最大且边界明确。
2. **标量输入与 writer 块输出**：格式不变，分别做 RED/GREEN 和单轮复测。
3. **CREATE INDEX 准备阶段合并段**：专项降低查询 T，同时观察 B/M 和准备上限。
4. **多行 INSERT 的严格快速解析**：只在写入仍显示解析瓶颈时实施。
5. 距离重关联/SIMD、扫描行复用和整体语言迁移暂缓。

继续遵循每项独立测量；reader 持有一轮退化并已回退，不重复作为本轮首选。以上仅为优化候选，尚未修改实现或宣称新的性能提升。
