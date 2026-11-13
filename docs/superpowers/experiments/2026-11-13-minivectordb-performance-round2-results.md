# MiniVectorDB 第二轮性能优化结果

日期：2026-11-13。执行方式：用户改为当前会话直接执行；失败的 OpenAI 子代理在启动前因地区限制403退出，没有代码改动。

## 1. 最终三轮结果

最终镜像：`minivectordb:r2-task5b`，ID `sha256:3301cb1bbbc1d6c9882aa4a623e684b3069d64b5c7a5365491300cf966918d93`。

| 项目 | 指标 | 最初基线 | 上一轮提交 | 本轮最终 | 相对上一轮 |
|---|---|---:|---:|---:|---:|
| 写入 | T | 6.732231秒 | 4.089392秒 | **3.114274秒** | **减少23.85%** |
| 写入 | B | 11,284,480 B | 11,280,384 B | 11,280,384 B | 相同 |
| 写入 | M | 42,348,544 B | 46,022,656 B | **41,504,768 B** | **减少9.82%** |
| 查询 | T | 58.799768秒 | 7.600248秒 | **3.598831秒** | **减少52.65%** |
| 查询 | B | 11,288,576 B | 11,288,576 B | **17,379,328 B** | **增加53.96%** |
| 查询 | M | 42,373,120 B | 45,780,992 B | **42,250,240 B** | **减少7.71%** |

相对最初基线，写入 T 减少53.74%，查询 T 减少93.88%（约16.34倍速度）。这是固定本地负载结果，不是官方 E、排名或分数。

最终查询 p50/p95/p99 约3.240/5.104/5.240ms；三轮两组 Recall@10 均为1.0。每轮均核对全量10,000行、1,280,000个 float32 bits、恢复、精确 ID/距离和过滤。

## 2. 五项实验及保留决策

### Task 1：可信热查询读取

增加 Full/StructureOnly 读取模式。只有 SELECT 使用 StructureOnly；writer、PK构建、发布和恢复保持 Full。结构、截断、长度、维度、顺序检查保留。

- 单轮查询：7.600248秒基线中位数 → **6.675961秒**；
- B相同；M=47,841,280 B，高于此前中位数，未隐藏；
- 保留。

### Task 2：标量输入与块输出

Input u32/u64 一次读取固定数组，u8直接消费缓冲；Output 整数和 float 向量块写入。格式不变。

精确入口计数使用临时 `/tmp` 插桩副本；uprobe 因无 tracefs 权限不可用，未把 perf 样本冒充调用次数。同一装载加1,000查询：

| 函数入口 | Task 1 | Task 2 | 变化 |
|---|---:|---:|---:|
| `Input::bytes` | 382,402,628 | **64,239,610** | 减少83.2% |
| `Output::bytes` | 9,081,086 | **135,884** | 减少98.5% |
| `validate_row` | 39,200 | 39,200 | 相同 |

正常无插桩镜像单轮：写入4.169825→3.751581秒，查询6.675961→5.449149秒；保留。

### Task 3：CREATE INDEX 合并段

阈值 DML 合并和 CREATE INDEX 共用同一 `compact_table` 归并函数。索引验证先于合并，候选根、事务回滚和 GC 语义不变。

- 查询：5.449149→**4.546961秒**；
- B：11,288,576→17,383,424 B，增加54.0%；
- M基本持平，索引准备约32.74ms；
- 以额外准备 I/O 换查询 T，保留并在最终三轮继续观察。

### Task 4：严格 INSERT C++ fast path

仅识别简单表名、无目标列的多行 VALUES。其他形状回退 SQLGlot；fast 成功仍进入同一 Engine::execute。初版差分发现负号空白、注释和双引号误拒，均通过新增 RED 修复为正确 fallback。尾随逗号、多语句和非法值保持拒绝。

- 写入：3.745276→**3.201308秒**，减少14.5%；
- 写入 M：46,080,000→41,095,168 B；
- 查询 T不变，符合只优化 INSERT 的预期；
- 保留。

### Task 5A：保持顺序的距离展开

四项展开只让转换/乘法调度重叠，double 加法顺序不变；没有 fast-math 或重关联。1,000组随机128维位级检查全部一致。

两个独立单轮查询分别为4.328954和4.312640秒，相比 Task 4 的4.549281秒下降约4.8%—5.2%；因接近阈值进行了第二轮，保留。

### Task 5B：单基础段扫描与行复用

TableFileReader 原位解码到传入 Row，复用 vector capacity；只有单基础段 SELECT 绕过 LogicalTableReader 堆。TopK 移入堆的行不别名化，其他表仍走原归并。

- 相对5A第二轮：4.312640→**3.581444秒**，减少16.9%；
- B/M基本不变；
- 保留。

没有重试此前已回退的 reader 持有方案，也没有整体迁移 C++/Rust。

## 3. 正确性、资源和构建边界

- 12/12 CTest、191/191 pytest；
- 原公开 handshake、20/20 case、95 SQL通过；
- 最终三轮全部内容、精确结果、恢复和 Recall 校验通过；
- 2GiB/no-swap下2,112MiB数据、完整查询、ANN、重复恢复和 ready 前强杀通过，无 OOM；资源诊断35.05秒，上一轮33.14秒，未把该差异隐藏为收益；
- 最终 perf：查询497个 cpu_core 样本，`l2_distance_squared` 自耗时约35.91%，TableFileReader::next约10.71%，多段堆和重复 UTF-8 校验不再突出；样本较少，精确比例只作方向参考；
- 最终写入 perf 只有63个 cpu_core 样本，不据此给稳定百分比；主要剩余路径为 fast INSERT、parse_vector 和 JSON 输入；
- 所有 perf Total Lost Samples=0，停止采样后的正确性检查通过。

原 Dockerfile 无缓存构建再次在 Docker Hub `python:3.12.10-slim` 元数据请求处发生 IPv6 超时。本轮迭代使用固定镜像的临时派生构建，生产 Dockerfile 未修改。因此正式提测前仍需完成干净源码构建。

## 4. 证据与提交状态

```text
.superpowers/sdd/performance-round2/
  profile-write/                 # 本轮改动前 perf
  profile-query/
  task1-round1/
  task2-round1/
  task2-callcounts.json
  task3-round1/
  task4-round1/
  task5a-round1/
  task5a-round2/
  task5b-round1/
  final-rounds/
  final-profile-write/
  final-profile-query/
  resource/
  reports/
```

计划：`docs/superpowers/plans/2026-11-13-minivectordb-performance-round2.md`。

本轮未 commit/push；`agent/conversation.json` 的外部已有提交未被修改，公开协议、驱动和用例未修改。
