# MiniVectorDB 性能基线与 perf 采样实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立符合赛题默认形状的本地三轮 T/B/M 基线、逐阶段耗时和独立正确性校验，并生成真实 perf 热点报告。

**Architecture:** Python 驱动官方 JSONL Session、确定性生成数据并采集 cgroup；独立 C++ oracle 暴力计算真值。基线使用 Release Docker，perf 使用同源码 RelWithDebInfo 宿主进程，二者严格分开。

**Tech Stack:** Python 3.12 stdlib、C++20、Docker/cgroup v2、pytest、Linux perf。

## Global Constraints

- 设计：`docs/superpowers/specs/2026-11-12-minivectordb-performance-baseline-design.md`。
- 默认10,000行、128维、100条/批、1,000次K=10 exact查询，500次等值过滤。
- 不修改 `minivectordb` 生产代码、公开题包、协议、配置或断言。
- 不计算无官方基线的 E/排名/赛事分数，不把本地结果称为正式成绩。
- 新增工具先写独立测试并观察 RED；不复制数据库输出作为真值。
- 不安装系统包、不修改内核权限；不提交、不推送，除非后续明确授权。

---

### Task 1：确定性生成器、独立 oracle 与统计函数

**Files:**
- Create: `tools/benchmark/minivectordb_perf.py`
- Create: `tools/benchmark/minivectordb_oracle.cpp`
- Create: `tools/benchmark/test_minivectordb_perf.py`
- Create: `tools/benchmark/README.md`

**Interfaces:**
- `build_dataset(seed=20261112) -> (records, queries)`；record 为 `(id,grp,payload,vector)`，query 为 `(query_id,optional_grp,vector)`。
- `write_oracle_input(path,records,queries)` 写独立二进制输入。
- `run_oracle(executable,input_path) -> list[[id,distance],...]`。
- `distribution(values) -> {count,sum,min,p50,p95,p99,max}`。

- [ ] **Step 1：写生成器、统计和 oracle RED 测试**

```python
def test_default_shape_and_determinism():
    records, queries = build_dataset()
    assert len(records) == 10_000 and len(queries) == 1_000
    assert len(records[0][3]) == 128
    assert sum(q[1] is not None for q in queries) == 500
    assert all(q[1] == i % 10 for i, q in enumerate(queries) if i % 2 == 0)
    assert records[0] == build_dataset()[0][0]

def test_distribution_is_recomputable():
    assert distribution([1,2,3,4]) == {
        'count':4,'sum':10.0,'min':1.0,'p50':2.0,'p95':4.0,'p99':4.0,'max':4.0}
```

Oracle 小型输入包含不同距离、同距离 PK tie 和过滤，测试要求先过滤再 TopK；用 Python `math.fsum((a-b)**2...)` 独立计算并逐值比较。测试进程先编译 oracle，再执行；不得 import minivectordb_core。

- [ ] **Step 2：运行 RED**

```bash
.venv/bin/python -m pytest tools/benchmark/test_minivectordb_perf.py -q | tee .superpowers/sdd/performance-baseline/task1-red.log
```

预期：模块/接口不存在，测试失败而非依赖环境错误。

- [ ] **Step 3：实现生成器和统计函数**

使用 `random.Random` 的独立数据/查询种子；每个分量经：

```python
def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]
```

`distribution` 先排序；p50 使用 nearest-rank `ceil(.50*n)-1`，p95/p99 同理；空输入返回 count=0，其余为 null。所有时间对外以秒、内部以纳秒整数保存。

- [ ] **Step 4：实现独立暴力 oracle**

二进制格式使用固定 little-endian header：magic `MVDBPERF1`、record/query数、维度、K；随后每行 id、grp、payload长度/内容、128个float bits，每条查询 id、grp（-1表示无过滤）、128个float bits。

Oracle 逐查询扫描全部记录，过滤后计算：

```cpp
double sum = 0.0;
for (std::size_t j = 0; j < dim; ++j) {
  double difference = double(record[j]) - double(query[j]);
  sum += difference * difference;
}
double distance = std::sqrt(sum);
```

以 `(distance,id)` 排序并输出前K JSON，`std::setprecision(17)` 保留 double。检查完整输入、有限值、空过滤集合和K边界。

- [ ] **Step 5：GREEN**

```bash
.venv/bin/python -m pytest tools/benchmark/test_minivectordb_perf.py -q | tee .superpowers/sdd/performance-baseline/task1-green.log
```

预期：生成器、统计、小 oracle 与 Python 真值全部通过。

---

### Task 2：Docker 分阶段计时、cgroup 与正确性

**Files:**
- Modify: `tools/benchmark/minivectordb_perf.py`
- Modify: `tools/benchmark/test_minivectordb_perf.py`
- Modify: `tools/benchmark/README.md`

**Interfaces:**
- `run_write(round_index,image,records,out) -> dict`。
- `run_query(round_index,image,records,queries,truth,out) -> dict`。
- `cgroup_metrics(session) -> dict`。
- CLI：`--output <新目录> --rounds 3 --image minivectordb:five-fixes`。

- [ ] **Step 1：写指标和响应校验 RED 测试**

用纯 Python 小样本测试：

```python
def test_recall_and_exact_ids():
    actual = [[2, 0.2], [1, 0.2]]
    expected = [[1, 0.2], [2, 0.2]]
    assert recall_at_k([x[0] for x in actual], [x[0] for x in expected], 10) == 1.0
    assert exact_topk_matches(expected, expected)  # 顺序及距离均检查
    assert not exact_topk_matches(actual, expected)

def test_metric_delta_uses_device_total_without_double_counting():
    before = {'259:0': {'rbytes':10,'wbytes':20}}
    after = {'259:0': {'rbytes':30,'wbytes':50}}
    assert io_bytes(before, after) == {'259:0': {'rbytes':20,'wbytes':30,'total':50}}
```

再测试错误过滤ID、重复ID、非有限距离、错误数量和错误顺序均不能通过。

- [ ] **Step 2：RED**

```bash
.venv/bin/python -m pytest tools/benchmark/test_minivectordb_perf.py -q | tee .superpowers/sdd/performance-baseline/task2-red.log
```

预期：校验/指标函数尚不存在。

- [ ] **Step 3：实现容器运行和指标采集**

通过 `docker inspect` 的 State.Pid 和 `/proc/<pid>/cgroup` 定位 `/sys/fs/cgroup`。记录原始 io.stat、memory.max/swap.max/peak/events、cpu.max、pids.max；IO 以设备分别计算 rbytes/wbytes/total，summary 使用最大设备 total。

Session 使用2 CPU、2GiB/no-swap/128PID、read-only、network=none。正式请求计时之外采样和同步。每批/每查询保存 `elapsed_ns`，输出 distribution。

写入按100条组成一个 INSERT；每批调用一次 request 并保留 affected_rows=100。查询严格按交替过滤序列执行 exact，保存完整响应。执行 `os.sync` 后再取最终 B/M。

- [ ] **Step 4：实现事后完整正确性**

写项目在最终性能采样后 crash/restart，按1,000行一页读取全部10页，比较每行 id/grp/payload 和每个 float bits；分页响应不超过协议上限。

查询项目对1,000条保存的响应检查：

- columns 恰好为 id,distance；
- K或过滤集合不足K时的正确数量；
- ID无重复、满足过滤、距离有限；
- exact ID顺序与 oracle 一致、距离符合1e-5容差；
- filtered/unfiltered 分别计算 Recall@10，均至少0.95。

所有失败写进结果并将状态置 FAIL；性能采样已完成也不能掩盖校验失败。

- [ ] **Step 5：单轮端到端 GREEN**

```bash
c++ -std=c++20 -O2 tools/benchmark/minivectordb_oracle.cpp -o /tmp/minivectordb-oracle
.venv/bin/python -m pytest tools/benchmark/test_minivectordb_perf.py -q
.venv/bin/python tools/benchmark/minivectordb_perf.py \
  --image minivectordb:five-fixes --rounds 1 \
  --output .superpowers/sdd/performance-baseline/smoke 2>&1 | \
  tee .superpowers/sdd/performance-baseline/task2-green.log
```

预期：一轮写入/查询、完整校验和 Recall 通过，输出可重算。`--rounds 1` 只用于工具联调，不作为三轮基线。

---

### Task 3：执行三轮正式形状的本地基线

**Files:**
- Modify: `tools/benchmark/README.md`
- Create: `docs/superpowers/experiments/2026-11-12-minivectordb-performance-baseline.md`

- [ ] **Step 1：确认固定输入和干净运行条件**

```bash
git rev-parse HEAD
docker image inspect minivectordb:five-fixes --format '{{.Id}}'
git diff --exit-code HEAD -- minivectordb STDIO_PROTOCOL.md protocol_check mysqltest
```

要求当前提交为 bbc17f7ac26678f8ce6ea1032b5966ed878c2a3d，镜像为58cd76b9…aed0，生产源码相对提交无变化。

- [ ] **Step 2：运行三轮**

```bash
.venv/bin/python tools/benchmark/minivectordb_perf.py \
  --image minivectordb:five-fixes --rounds 3 \
  --output .superpowers/sdd/performance-baseline/rounds 2>&1 | \
  tee .superpowers/sdd/performance-baseline/rounds.log
```

每轮独立 write/query 容器与卷；不得复用 Task2 smoke 输出。保留每批和每次查询原始时间、所有响应校验和三轮完整 T/B/M。

- [ ] **Step 3：独立重算汇总**

```python
from statistics import median
for project in ('write','query'):
    for metric in ('T_seconds','B_bytes','M_bytes'):
        values = [rounds[r][project][metric] for r in range(3)]
        assert summary[project][metric]['median'] == median(values)
```

核对三轮总批数各100、查询各1,000、过滤各500、两组 Recall 均达标、所有状态 PASS。写入与查询分项目报告 median，不生成 E。

- [ ] **Step 4：写基线报告**

列出镜像/种子/工具哈希、三轮 T/B/M、median、阶段耗时与查询分位点、完整校验和局限。明确本地缓存与平台冷缓存不同，无官方基线时无法计算排名分数。

---

### Task 4：本地 perf 工具与独立热点采样

**Files:**
- Modify: `tools/benchmark/minivectordb_perf.py`
- Modify: `tools/benchmark/README.md`
- Modify: `docs/superpowers/experiments/2026-11-12-minivectordb-performance-baseline.md`

- [ ] **Step 1：本地解压并验证 perf**

```bash
mkdir -p /tmp/mvdb-perf-tool
cd /tmp/mvdb-perf-tool
apt download linux-perf
dpkg-deb -x linux-perf_*.deb root
find root -type f -name 'perf*' -print
```

选择实际二进制，检查 `ldd` 和 `version`。缺失库时只下载/解压对应运行库到该目录并使用 LD_LIBRARY_PATH，不 sudo 安装。记录包SHA256、路径和版本。

- [ ] **Step 2：构建独立 RelWithDebInfo 采样版本**

```bash
cmake -S minivectordb -B build/perf-release \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
  -DMINIVECTORDB_ENABLE_FAULT_INJECTION=OFF \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
  -Dpybind11_DIR="$(.venv/bin/python -m pybind11 --cmakedir)"
cmake --build build/perf-release -j2
```

该构建只用于 perf，不替换基线镜像。

- [ ] **Step 3：分别采集写入与查询**

benchmark 增加 `--profile <write|query> --perf <path>`，用官方 Session 的 command 模式启动：

```python
command = [perf, 'record', '-F', '199', '-g', '--call-graph', 'dwarf',
           '-o', str(output/'perf.data'), '--', sys.executable,
           '-m', 'minivectordb', '--data-dir', '{data_dir}']
```

设置 PYTHONPATH 为 build/perf-release/python 和 minivectordb/python。执行同一负载及校验，但不采集为三轮 T/B/M。若内核权限拒绝，保存完整错误并停止，不把 perf 未运行写成成功。

- [ ] **Step 4：生成报告并解释热点**

```bash
perf report --stdio --no-children --percent-limit=0.1 -i perf-write.data
perf report --stdio --no-children --percent-limit=0.1 -i perf-query.data
```

保存文本、调用图和采样运行校验结果。报告区分 SQLGlot/Python、JSON、C++扫描/距离、存储写入和 fsync 热点；只根据实际占比提出下一轮优化假设，不在本任务直接修改生产实现。

- [ ] **Step 5：收尾检查**

```bash
git diff --check
git diff --exit-code HEAD -- minivectordb STDIO_PROTOCOL.md protocol_check mysqltest
docker ps -a --filter name=vectordb-judge- --format '{{.Names}}'
docker volume ls --filter name=vectordb-judge- --format '{{.Name}}'
git status --short --branch
```

不得遗留容器/卷，不 stage、不 commit、不 push。
