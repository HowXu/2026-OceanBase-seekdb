# MiniVectorDB 首轮性能优化结果

日期：2026-11-12。

## 0. 最终结论

在固定本地负载上，缓冲/向量块读取和 COW 主键集合保留；reader 持有实验因单轮无收益已回退。

| 项目 | 指标 | 基线 median | 优化后 median | 变化 |
|---|---|---:|---:|---:|
| 写入 | T | 6.732231秒 | **4.089392秒** | **减少39.26%** |
| 写入 | B | 11,284,480 B | 11,280,384 B | 减少0.036% |
| 写入 | M | 42,348,544 B | 46,022,656 B | **增加8.68%** |
| 查询 | T | 58.799768秒 | **7.600248秒** | **减少87.07%** |
| 查询 | B | 11,288,576 B | 11,288,576 B | 相同 |
| 查询 | M | 42,373,120 B | 45,780,992 B | **增加8.04%** |

等价地，写入 T 约1.65倍速度，查询 T 约7.74倍速度。没有把本地结果换算成官方 E、排名或分数。

最终迭代镜像：`minivectordb:opt-pkset`，ID `sha256:7284de5e057f66a9fb5ae51c30ef7f5bd748f4e2bb7c106452e141e2dd360801`。

## 1. 保留：有界缓冲和向量块读取

`storage::Input` 使用64 KiB有界缓冲，逻辑 remaining 包含缓冲中未消费字节；大目标在缓冲为空时直接读入目标。TableFileReader 与 DeltaFileReader 的向量均通过 `Input::floats` 整块读取。

文件 magic、记录格式、CURRENT、故障注入和恢复顺序不变。

新增 RED 覆盖70,000字符 VARCHAR、65,536个 float bits、负零和完整 EOF；最初因 `Input::floats` 不存在而编译失败。GREEN 后原11 CTest、188 pytest通过。

单轮结果：

| 指标 | 基线 median（仅作参照） | buffer 单轮 |
|---|---:|---:|
| 写入 T | 6.732231 | **4.064210** |
| 查询 T | 58.799768 | **6.931859** |
| 写入 B | 11,284,480 | 16,396,288 |
| 写入 M | 42,348,544 | 52,535,296 |
| 查询 B | 11,288,576 | 11,288,576 |
| 查询 M | 42,373,120 | 46,465,024 |

单轮 B/M 的上升没有隐藏；最终三轮回落到第0节所列值。物理读会受宿主/镜像缓存影响，不能把 buffer 单轮的 B 上升全部归因为代码。

## 2. 回退：持有并重置 reader

实现了 Input/TableFileReader/DeltaFileReader/LogicalTableReader reset，以及 TableEntry 共享持有 reader。CTest 和188 pytest通过，但同口径单轮：

| 指标 | buffer 单轮 | reader-hold 单轮 |
|---|---:|---:|
| 写入 T | 4.064210 | 4.146668 |
| 查询 T | 6.931859 | **7.581458** |
| 写入 M | 52,535,296 | 48,218,112 |
| 查询 M | 46,465,024 | 46,534,656 |

查询和写入均无 T 收益。按已确认规则，不利结果保存在 `readers-round1/`，该实验的生产改动已全部回退；最终版本不包含持有 reader。

## 3. 保留：COW 主键集合

TableEntry 惰性持有 `unordered_set<int64_t>`。INSERT 用它检查已存在键，成功后复制集合并加入新键；旧根、失败语句和回滚不被原地污染。

首次全量测试发现 DELETE 后重插入误报 duplicate_key。根因是 DELETE 不能保留已删除键。修复为 tombstone 成功后 COW 删除键，随后188 pytest通过。该不利结果和修复日志均保留。

PK-set 单轮写入 T=3.872694秒；最终三轮写入 T median=4.089392秒。单轮最快值不作为最终结果。

## 4. 最终正确性与资源

- 11/11 CTest；
- 188/188 pytest，容器测试使用最终 opt-pkset 镜像；
- 原公开驱动 handshake、20/20 case、95 SQL通过；
- 三轮每轮100批写入和1,000查询，500带过滤；
- 每轮重启后核对全部10,000行及1,280,000个向量 float bits；
- 三轮 exact ID/距离均匹配独立 oracle，两组 Recall@10 均为1.0；
- oom/oom_kill均为0，压测容器和卷清理完成。

2 GiB 超内存复测：

- 2,112 MiB payload，数据文件2,214,701,020 B；
- 总耗时33.138294秒，较上一版47.396518秒缩短；
- 完整内容、ANN、两次恢复及 ready 前强杀通过；
- 无 OOM，容器/卷清理完成。

## 5. 最终 perf 热点

使用同源码 RelWithDebInfo 宿主构建，PID 附加仅采样正式阶段，停止后再校验。Total Lost Samples均为0。

### 查询

最终查询采样约1K cpu_core cycles样本：

| 符号 | 自耗时 |
|---|---:|
| `storage::Input::bytes` | 16.84% |
| `validate_row` | 13.05% |
| `l2_distance_squared` | 12.90% |
| `TableFileReader::next` 自身 | 5.86% |
| `DeltaFileReader::next` 自身 | 5.07% |
| `utf8_codepoints` | 4.86% |

原基线中四个 iostream 读取函数合计72.36%，距离计算仅1.33%。优化后热点已明显从逐字节 iostream 调用移到缓冲复制、行校验和实际距离计算。

### 写入

最终写入只有137个 cpu_core 样本，统计分辨率有限。Python 帧处理/属性访问和输出写入成为较明显的剩余开销；不再把少量样本的精确占比当成稳定结论。

## 6. 构建与证据边界

Docker Hub 在迭代期间发生连接超时，本地未保留 python 标签。为完成测量，opt-* 镜像使用固定的 five-fixes 镜像作为本地基底，在临时上下文中用 Debian GCC12编译并替换 extension/Python；生产 Dockerfile 未修改。

因此本地迭代镜像证明了当前源码的运行与性能，但**正式提测前仍应从原 Dockerfile 做一次干净构建**。临时上下文、包信息和构建日志均保留。

证据目录：

```text
.superpowers/sdd/performance-optimization/
  buffer-round1/
  readers-round1/
  pkset-round1/
  final-rounds/
  final-profile-write/
  final-profile-query/
  resource/
```

报告未把单轮当中位数、未隐藏 reader 退化或 M 上升、未计算无官方基线的赛事得分。本轮未 commit/push。
