# MiniVectorDB 本地性能基线与 perf 热点

日期：2026-11-12。

## 0. 范围

对象：提交 bbc17f7ac26678f8ce6ea1032b5966ed878c2a3d。
基线镜像：`sha256:58cd76b9a7a4998c71060f24f535b2331d8be9c39e4273ee7d4d070642a9aed0`。

按赛题默认形状建立本地基线：10,000行、128维、100条/批、1,000次 exact TopK、K=10、500次等值过滤。三轮使用独立 write/query 容器和数据卷，2 CPU、2 GiB、no-swap、128 PID、只读根、禁网。

**这是本地生成的基线，不是平台分数。** 缺少官方 T_base/B_base/M_base，因此不计算 E、排名或“200分”的变化。

工具：

- `tools/benchmark/minivectordb_perf.py`：生成、计时、cgroup、校验与汇总。
- `tools/benchmark/minivectordb_oracle.cpp`：不链接数据库核心的独立暴力真值。
- `tools/benchmark/profile_minivectordb.py`：独立宿主 perf 采样。
- `tools/benchmark/README.md`：复现方法和统计口径。

证据：`.superpowers/sdd/performance-baseline/`。三轮工具与 oracle 源码 SHA256 已记录在 summary，并在 rounds 目录保存对应快照。

## 1. 三轮 T/B/M

| 项目 | 轮次 | T（秒） | B（字节） | M（字节） |
|---|---:|---:|---:|---:|
| 写入 | 0 | 6.809013 | 11,284,480 | 41,959,424 |
| 写入 | 1 | 6.711824 | 11,284,480 | 44,290,048 |
| 写入 | 2 | 6.732231 | 11,280,384 | 42,348,544 |
| **写入 median** | — | **6.732231** | **11,284,480** | **42,348,544** |
| 查询 | 0 | 59.252804 | 11,288,576 | 43,651,072 |
| 查询 | 1 | 58.799768 | 11,288,576 | 42,373,120 |
| 查询 | 2 | 58.280173 | 11,288,576 | 41,639,936 |
| **查询 median** | — | **58.799768** | **11,288,576** | **42,373,120** |

T 是正式请求/响应墙钟时间；B 是最大单设备 rbytes+wbytes 增量，原始设备值均保留；M 是 memory.peak，包含页缓存。没有把不同轮次的最佳值拼成一轮。

首轮 smoke 曾出现镜像冷加载造成的额外物理读取，未混入上述三轮。三轮前镜像已预热，因此这些是本地预热镜像/新数据卷口径，不声称等价于平台统一冷缓存。

## 2. 分阶段与分位点

以下均为三轮 median。

### 写入

| 阶段/分布 | 秒 |
|---|---:|
| startup | 0.584832 |
| schema | 0.022178 |
| **100批正式写入 T** | **6.732231** |
| 最终 sync（不计入T） | 0.090455 |
| 每批 p50 / p95 / p99 | 0.065371 / 0.091857 / 0.099113 |

### 查询

| 阶段/分布 | 秒 |
|---|---:|
| startup | 0.542498 |
| schema | 0.020088 |
| 100批装载（不计入查询T） | 6.762837 |
| CREATE VECTOR INDEX | 0.020043 |
| 20次预热（不计入查询T） | 1.186206 |
| **1,000次正式 exact 查询 T** | **58.799768** |
| 最终 sync（不计入T） | 0.095801 |
| 全部查询 p50 / p95 / p99 | 0.058386 / 0.062840 / 0.065714 |
| 有过滤 p50 / p95 | 0.057963 / 0.062213 |
| 无过滤 p50 / p95 | 0.058924 / 0.063516 |

原始100批/轮和1,000查询/轮逐次纳秒计时、完整响应及 cgroup 快照均在 rounds 目录，可独立重算。

## 3. 正确性

三轮全部 PASS：

- 写入每轮100批、10,000行；重启后分10页核对全部 ID、grp、payload 和 **1,280,000个向量分量的 float32 bits**。
- 查询每轮1,000次，500有过滤；检查列、数量、顺序、ID唯一性、过滤条件和有限距离。
- 每个 exact TopK 的 ID 顺序及距离均与独立 oracle 一致。
- 三轮有过滤/无过滤平均 Recall@10 均为 **1.0**。
- oom/oom_kill 均为0，容器和卷均清理。

Oracle 先通过小型 Python `math.fsum` 独立样例，包括过滤优先、距离相同 PK tie 和空过滤集合。

## 4. perf 采样方法与真实性

宿主没有预装 perf，sudo 需要密码。本地下载并解压：

- linux-perf 7.2.15-1kali1；
- libdebuginfod1t64、libopencsd1、libpython3.14、libtraceevent1 运行库。

包 SHA256、ldd、version 和 smoke 记录均保留。未安装系统包或修改内核设置。

采样构建为独立 `build/perf-release`：RelWithDebInfo、fault injection OFF。使用同用户宿主进程，在准备完成后按 PID 附加 perf，仅采集正式写入或正式查询；停止采样后才做事后校验和 report。采样时间不用于三轮 T/B/M。

最终采样：

| 项目 | 采样阶段耗时 | perf.data | 正确性 |
|---|---:|---:|---|
| 写入 | 4.353838秒 | 6,177,908 B | 全量重启校验 PASS |
| 查询 | 58.099356秒 | 97,336,204 B | 1,000 exact/Recall PASS |

两者 Total Lost Samples 均为0。写入采样722个 cpu_core cycles 样本；查询报告分别包含17个 cpu_atom 和约11K cpu_core 样本。以下自耗时百分比明确取自各报告相应事件区段，不把不同 PMU 区段混算。

首次写入采样 `profile-write/` 因 perf 以 SIGINT 终止被工具误判 FAIL；实际日志有726个样本且数据完整。修正接受正常 `-SIGINT` 后重新运行得到上述 `profile-write2/`。首次记录保留，未删除不利证据。

## 5. 实际热点

### 查询

cpu_core cycles 自耗时：

| 符号 | 占比 |
|---|---:|
| `std::basic_filebuf::xsgetn` | 21.78% |
| `std::basic_streambuf::xsgetn` | 19.91% |
| `std::istream::sentry` | 17.14% |
| `std::istream::read` | 13.53% |
| **上述 iostream 读取合计** | **72.36%** |
| `TableFileReader::next` 自身 | 3.81% |
| `DeltaFileReader::next` 自身 | 1.99% |
| `l2_distance_squared` | **1.33%** |

调用图进一步显示查询经 LogicalTableReader 逐行、逐段读取，并包含每查询对象路径检查、文件打开和多路归并。

**结论：当前查询瓶颈主要是物理行解码和多段读取框架，而不是 float 欧氏数学循环。** 只向量化距离计算而不处理读取路径，依据当前 perf 不足以取得大幅端到端提升。

### 写入

cpu_core cycles 自耗时：

| 符号 | 占比 |
|---|---:|
| `std::basic_streambuf::xsgetn` | 18.49% |
| `std::basic_filebuf::xsgetn` | 17.95% |
| `std::istream::sentry` | 13.46% |
| `std::istream::read` | 8.72% |
| **上述 iostream 读取合计** | **58.62%** |
| Python `_PyEval_EvalFrameDefault` 自身 | 5.52% |
| `parse_vector` 自身 | 0.27% |

写入调用图显示 INSERT 为检查已有主键而完整消费 LogicalTableReader；在 cpu_core 区段，insert_rows 下 LogicalTableReader::next 的子树占比较明显。优化优先级应先处理读取与重复键检查，不是优先微调 SQL 文本转 float。

## 6. 下一轮优化假设（尚未实施）

按测量依据排序：

1. **替代逐字节 ifstream 解码：**使用有界缓冲的块读取/pread 或只读 mmap，一次解码 BIGINT、长度和整个向量，减少 sentry/xsgetn 调用。
2. **减少查询段归并成本：**评估在索引准备阶段合并到单一基础段，或为向量 TopK 提供仅读取必要列的逻辑扫描；索引准备不计查询 T，但其 B/M 必须计入并复测。
3. **减少 INSERT 全表重复键扫描：**在内存中维护已提交/事务可见 PK 集合或利用批量键范围，保持崩溃恢复和事务隔离。
4. 每项独立 RED/正确性回归后，用同一三轮工具比较 T/B/M median；不能只展示 perf 占比或单次最快值。

这些是根据热点形成的假设，不是尚未验证的优化收益承诺。本轮没有修改生产实现，没有提交或推送。
