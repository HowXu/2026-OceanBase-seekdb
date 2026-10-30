# V5 与候选版本配对评测工具设计

## 目标

在不联网、没有官方 SIFT250K/Hidden250K 数据和官方评测程序的前提下，提供可复现的本地评测工具。工具以冻结提交 `634888d9b037153fa143d2c913ba4b25503e1d62` 为基线，比较候选 seekdb 版本的 GPU CAGRA 构建、查询、准确度和资源使用。

结果分为 `OFFICIAL_LIKE_PASS` 与 `LOCAL_STRICT_PASS`。本地通过不代表 Hidden250K 或正式评分必然通过。

## 方案

新增独立的 Python 评测入口，不修改 seekdb 内核，也不改变现有 CPU/GPU benchmark。数据生成、精确 ground truth、数据库生命周期、配对执行、指标计算和报告由工具统一负责。运行依赖远端已有 `/opt/venv` 的 NumPy 和 PyMySQL；缺少依赖时明确失败，不联网安装。

文件：

- `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`：评测入口。
- `docs/gpu-vector-index-hipvs-cuvs/validation/RUN_CONTEST_EVAL.md`：构建、执行和结果解释。
- `unittest/oblib/vector/test_contest_eval.py`：标准库 `unittest` 单元测试。

## 构建和隔离

- V5 与候选版本使用独立源码目录、bridge、observer 和构建目录。
- 两个版本均使用组织方冻结的 `deps/3rd`，编译并发最多 `-j96`，不下载新依赖。
- 本地验证两边均开启 `OB_BUILD_CUVS_TRACE=ON`；正式评测仍使用赛题规定的 `OFF`。
- 每次只运行一个被测服务。服务使用独立端口、数据目录、trace、日志和结果目录。
- 每轮重新启动服务，避免复用索引文件或进程内缓存。数据导入不计时。
- 工具只停止自己启动且命令行匹配的进程，不干扰已有构建或服务。
- 评测前检查 GPU 空闲、端口空闲、binary/bridge 存在、Python 依赖可用。

## 数据集

全部数据按固定 seed 离线生成。V5 与候选版本使用相同的数据、查询和 ground truth；缓存记录 shape、dtype、seed、分布参数及 SHA-256。

完整矩阵：

| 名称 | 行数 | 维度 | 分布 |
| --- | ---: | ---: | --- |
| gaussian250k-d128 | 250000 | 128 | 独立高斯 |
| clustered250k-d128 | 250000 | 128 | 多簇、各向异性 |
| gaussian250k-d256 | 250000 | 256 | 独立高斯 |
| clustered250k-d256 | 250000 | 256 | 多簇、各向异性 |
| gaussian250k-d384 | 250000 | 384 | 独立高斯 |
| gaussian250k-d768 | 250000 | 768 | 独立高斯 |
| clustered1m-d128 | 1000000 | 128 | 多簇、各向异性 |
| clustered1m-d256 | 1000000 | 256 | 多簇、各向异性 |

每个数据集有 4096 条独立查询，包含普通查询、簇边界查询及分布偏移查询。ground truth 使用分块精确 L2 距离计算 Top-100，不使用任何近似索引生成答案。

`quick` 模式使用 10K、128 维数据验证完整链路；`full` 模式执行完整矩阵。数据集可通过命令行筛选，以便重跑失败项。

## 配对与计时

每个数据集运行两轮并交换顺序：

```text
Round 1: V5 -> candidate
Round 2: candidate -> V5
```

使用官方固定索引参数：

```sql
CREATE VECTOR INDEX idx_items ON items(v)
WITH (distance=l2, type=hnsw, lib=cuvs,
      m=16, ef_construction=200, ef_search=64);
```

构建计时包括：

- `create_seconds`：DDL 时间。
- `first_query_seconds`：第一条固定查询时间。
- `ready_seconds = create_seconds + first_query_seconds`。
- 构建加速比：V5 `ready_seconds` / candidate `ready_seconds`。

查询阶段使用 4096 条独立的单行近似 SQL，不使用 `batch_knn` 替代。Top-10 与 Top-100 分开测量 QPS、P50/P95/P99、Recall 和结果完整性。

## 正确性和性能门禁

每个版本分别检查：

- 每条查询恰好返回 K 个互不重复的合法 ID。
- Recall 为每条查询与精确 ground truth 交集比例的平均值。
- 128 维官方-like 数据检查 Recall@10/100 均不低于 0.95。
- 256 维官方-like 数据检查 Recall@10 不低于 0.90、Recall@100 不低于 0.85。
- candidate 相对同轮 V5 的 Recall@10/100 下降均不超过 0.005。
- candidate/V5 查询 QPS 比不低于 0.90；Top-10/100 分别判断并计算几何平均。
- 日志和 trace 必须证明 GPU 快照构建及 `cuvs_serve`，并检查 CPU 回退痕迹。
- 显存峰值不超过同轮 V5 的 1.25 倍且不超过 45 GiB，进程 RSS 不超过 96 GiB。
- 构建和查询期间监测 GPU 利用率、显存和进程资源。

本地严格评测还包括多分布、384/768 维和 1M 压力矩阵。额外数据集同样执行结果完整性、baseline delta、QPS 和资源检查，但不伪装成官方 Hidden250K 结论。

## 功能补充

正式配对前后运行小规模功能检查：

- `lib=vsag` 建表、建索引和查询可用。
- GPU 快照持久化与服务重启后查询正确。
- 删除和插入后查询不含过期 ID，并包含新 ID。
- 非 L2 DDL 拒绝及 GPU 不可用回退沿用已有 GPU smoke，不计入官方-like 性能结果。

Notebook 中的 10K CPU/GPU 与 `batch_knn` 结果仅作背景资料，不作为本工具性能口径。

## 报告和证据

结果写入 `/work/evals/<timestamp>/`，大型数据、日志和结果不提交 Git：

```text
config.json
summary.json
summary.md
raw/
datasets/
```

保存源码 revision、binary/bridge SHA-256、数据集 SHA-256、运行命令、环境信息、每轮原始查询结果、trace、服务日志和资源采样。任何缺失证据、服务异常或结果不完整均使对应门禁失败，不能按加速结果处理。

## 单元测试与执行顺序

先写失败测试，再实现：

1. 精确 ground truth 和 Recall。
2. 重复/缺失/非法结果检查。
3. baseline delta、绝对 Recall 和 QPS 门禁。
4. 构建/查询加速比及延迟百分位。
5. 显存和内存门禁。
6. 数据集矩阵、缓存一致性和报告状态。
7. 服务参数、进程归属及清理边界。

实现后依次运行：单元测试、现有离线 hipVS 测试、远端 `quick`、小规模 V5/candidate 配对、完整矩阵。正在进行的构建未结束前不启动评测。
