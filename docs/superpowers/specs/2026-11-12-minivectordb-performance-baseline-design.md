# MiniVectorDB 本地性能基线与 perf 采样设计

日期：2026-11-12
目标提交：bbc17f7ac26678f8ce6ea1032b5966ed878c2a3d

## 1. 目标与边界

建立符合赛题 §3.3 默认形状的本地、可复现、端到端性能基线，并通过 Linux perf 找到写入和 exact TopK 的实际热点。基线用于后续比较同一工具生成的相对变化，不冒充平台成绩。

固定负载：10,000 条记录、128 维、100 条/批、1,000 次 TopK、K=10，其中 500 次带普通列等值过滤，查询使用 exact。三轮使用独立容器、cgroup 和数据目录。

不修改生产实现、公开题包、配置、断言或协议。没有官方 T/B/M 基线值，因此不计算或虚构 E、排名和性能分数。当前用户提到的“200分”不作为本地指标公式输入。

## 2. 工具结构

新增非生产工具：

- `tools/benchmark/minivectordb_perf.py`：数据生成、Docker 生命周期、请求计时、cgroup 采样、结果校验、三轮汇总。
- `tools/benchmark/minivectordb_oracle.cpp`：独立暴力欧氏真值程序，不链接或调用 MiniVectorDB 核心。
- `tools/benchmark/README.md`：固定负载、运行方法、输出格式和局限。

原始证据写入 `.superpowers/sdd/performance-baseline/`。数据缓存和工具包不进入 Docker 生产镜像。

## 3. 确定性数据与查询

本地种子固定为 20261112，三轮使用同一输入，避免把数据变化当成性能变化。另可用不同种子做正确性复核，但不混入基线三轮。

表结构：

```sql
CREATE TABLE items (
  id BIGINT PRIMARY KEY,
  grp BIGINT,
  payload VARCHAR(64),
  v VECTOR(128)
);
```

记录：

- id 为 0..9999；
- grp=id%10；
- payload 为 `row-<id>-` 后接 x，并截到64字符；
- 向量每个分量为 `random.Random(20261112).uniform(-1,1)` 经 struct 量化后的 float32；SQL 使用有限十进制表示。

查询使用独立随机流，避免数据生成消耗顺序影响查询。第偶数次查询带 `grp=<query_index%10>`，奇数次不带过滤，形成交替的500/500序列。查询向量同样量化为 float32，K 固定10。

数据、查询及独立真值可缓存；缓存由输入生成器产生，不读取数据库答案。正式隐藏种子和答案不可获得，也不推断。

## 4. 写入项目

每轮使用新容器和空卷，限制为2 CPU、2 GiB、no-swap、128 PID、只读根、禁网。

阶段：

1. 启动并等待 ready，记录 startup；
2. CREATE TABLE，记录 schema；
3. 从空表执行100个多行 INSERT，每批100条，自动提交成功响应即持久化边界；
4. 记录每批及总写入墙钟时间，包含请求、SQL处理、结果输出和响应；
5. 停止写入计时后执行文件同步并采集最终 cgroup；
6. 事后强杀/重启，按主键分10页核对全部10,000个ID、grp、payload和向量的float32位模式，以及重启后持久性；分页避免超过响应大小上限。事后校验不加入性能 T/B/M。

写入 T 是100批之和，不含启动、建表、最终同步和事后校验。B/M 包含启动、建表、装载、同步和写回。

## 5. 查询项目

查询项目使用另一套新容器和空卷，避免写入项目的峰值内存和历史段影响查询基线。

阶段：

1. 启动、建表并按同样的100批装载；
2. CREATE VECTOR INDEX，记录 index_prepare；
3. 固定20次预热（10有过滤、10无过滤），不计入查询 T；
4. 执行1,000次正式 exact 查询，记录每次及总墙钟时间；有过滤/无过滤分别汇总；
5. 停止计时后同步并采集 cgroup；
6. 独立核对每次响应的列、数量、ID唯一性、过滤条件、当前记录距离和 TopK 顺序。

查询 T 只含1,000次正式请求/响应；装载、建索引、预热、同步和事后真值计算不计入 T。它们中的数据库侧 I/O 和内存仍计入 B/M，事后独立校验不计入。

## 6. 独立真值与 Recall

Oracle 使用独立 C++ 程序读取生成器写出的二进制数据和查询：

- 存储分量按 float32 bits 读取，转 double 计算欧氏距离；
- 先应用普通过滤，再按距离、PK升序排序；
- 每条查询返回精确的10个ID及距离；
- 不链接 minivectordb_core，不复用 exact_search、执行器或索引描述符。

先用小数据集与 Python 独立计算逐值交叉验证 oracle，再生成基准真值。真值只生成一次并供三轮使用。

exact 模式要求返回 ID 序列与真值一致，距离在题包容差内；同时计算 Recall@10。空过滤集合在本负载中不存在，但校验器仍按 min(K,N) 定义分母并排除空集合平均。无过滤和有过滤平均 Recall 均必须至少0.95；本地精确实现预期为1.0。

## 7. T/B/M 与分阶段输出

使用 cgroup v2：

- T：`time.perf_counter_ns()` 的请求墙钟时间；
- B：数据设备 io.stat 的 rbytes+wbytes 增量，保留所有设备原始值，汇总取最大单设备总量以避免 partition/mapper 重复计数；
- M：容器 memory.peak，包含页缓存，不使用进程 RSS 代替；
- 同时保存 memory.events、io.stat、cpu.max、pids 和容器配置证据。

每轮输出：

- startup/schema/load/index/warmup/sync 阶段耗时；
- 写入100批逐批时间；
- 查询1,000次逐次时间及 filtered/unfiltered 标记；
- 各阶段 sum/min/p50/p95/p99/max；
- T/B/M、Recall、正确性状态、镜像ID、种子和工具SHA256；
- 三轮完整值及中位数，不拼接不同轮次的最佳指标。

输出至少包括 `round-<n>-write.json`、`round-<n>-query.json`、`summary.json`、原始请求结果和 stderr。校验失败或预算失败必须保留 FAIL，不因性能结果较快而改写为 PASS。

本地新卷不能等价于平台统一冷缓存；不清理宿主系统缓存，不宣称复现平台硬件或官方基线。

## 8. perf 采样

宿主未安装 perf，sudo 需要密码。允许将 Kali linux-perf 包下载并解压到本地临时工具目录，不安装系统包、不修改 perf_event_paranoid。先检查动态库和 `perf version`，不可用时保留实际错误，不用其他工具冒充。

基线 Docker 运行不附加 perf。采样使用单独 RelWithDebInfo 构建、同一源码和相同负载的宿主进程，避免容器内 root 与宿主普通用户之间的 perf 权限差异：

```text
perf record -F 199 -g --call-graph dwarf -o <file> --
  python -m minivectordb --data-dir <独立目录>
```

分别采集写入和 exact 查询，输出 `perf report --stdio`、热点函数和调用图。采样进程不套用2GiB/2CPU基线数据，也不将其耗时纳入三轮基线；perf 只用于定位热点。

后续优化必须以同一基线复测为依据，不能仅因 perf 占比高就宣称端到端提升。

## 9. 验收

- 生成器和 oracle 的小规模独立交叉检查通过；
- 三轮写入、查询全部完成，100批和1,000次计数准确；
- 所有内容、过滤、距离、顺序、Recall 和持久化检查通过；
- 三轮 T/B/M 与分阶段原始数据齐全，汇总可重算；
- perf 数据和文本报告真实生成，或明确记录无法获得的系统阻塞；
- 无遗留压测容器/卷；
- 不修改生产代码，不提交或推送，直至后续单独授权。
