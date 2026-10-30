# TOKENIZE 默认参数命中路径：十组 A/B

## 保留结论与改动范围

以 `715e660a9` 为基线，保留 `src/sql/engine/expr/ob_expr_tokenize.cpp` 的窄范围优化：

- 没有第三个 properties 参数时，先以完整文本、trim 后的原始 parser 名称、隐式默认属性标记、collation、charset、output mode 和 LOB header 标志匹配现有 thread-local TOKENIZE 结果 memo。
- 命中直接复制结果到当前表达式分配器，不再重复分配和序列化 parser 名称、重建默认 properties、查找 IK dictionary loader。
- 未命中仍执行原有 parser 名称验证、版本序列化、属性重建、词典初始化、分词及 JSON 生成；全部成功后才发布缓存。
- 显式传入第三个参数的调用保持原有验证和初始化顺序。新增 `implicit_defaults_` 区分两类 key，防止原始名称 key 与规范化名称 key 混用。
- 当前 `ObFTParser::init()` 只接受内置 parser，版本为 `BUILTIN_VERSION`。不引入 parser 插件版本缓存或 MATCH 结果缓存。
- memo 仍深拷贝 key 和结果，命中结果仍写入当前 allocator，没有新增跨生命周期 datum 指针。

benchmark、数据生成器、固定 baseline、评分脚本和默认参数均未修改。本地复现不代表官方 CI 成绩。

## 候选与构建

构建命令：

```sh
cmake --build build_release --target seekdb -j8
```

基线：`run_temp/fts-dense-topk-v3-candidate-seekdb`

```text
9c8bf4d3fe58fdc232ffcc3bf9c5842e5b5a38dcd5259d757ac850f52ab58ce8
```

保留候选：`run_temp/fts-tokenize-hitpath-v2-candidate-seekdb`

```text
39eb46f4498866a66fb6eeba4ae530160319a932fd93c5cbd120130e7aac6fda
```

第一版只推迟 properties 重建和词典检查，仍在 memo 命中前构造 parser 名称。其五组 TOKENIZE 类改善中位数为 -3.8790%，综合中位数 +0.0013%、均值 -0.5742%，没有稳定收益，未保留该中间版本。

## 十组完整原始 benchmark

每组都用全新实例；奇数组基线先跑，偶数组候选先跑。每次使用原始默认值：

```text
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200
SAMPLES=3 WARMUP=30 SKIP_LOAD=0
```

实例设置：端口 12883、memory_budget=2G、datafile_size=1G、datafile_maxsize=4G、log_disk_size=2G。

20 份报告的默认参数均匹配，hits 全部为中文 8001、英文 11000、混合 7332、LIMIT 20。所有轮次都纳入统计；profiling 运行不纳入性能验收统计。

正值表示耗时下降。配对降幅按每组 `(baseline - candidate) / baseline` 计算，类别内与类别间使用原评分脚本相同的等权方式；配对降幅中位数不等于两列耗时中位数的直接比值。

| 指标 | 基线中位数 | 候选中位数 | 配对降幅中位数 | 配对降幅均值 | 获胜组数 |
|---|---:|---:|---:|---:|---:|
| IK 多列构建 / s | 4.6575 | 4.5580 | +0.0538% | +0.8866% | 6/10 |
| IK 单列构建 / s | 3.5460 | 3.5420 | +0.0282% | +0.5179% | 5/10 |
| BEng 构建 / s | 2.1420 | 2.1395 | +0.0699% | +0.1492% | 8/10 |
| 构建总耗时 / s | 10.3095 | 10.2465 | +0.4976% | +0.6749% | 6/10 |
| IK TOKENIZE / ms | 0.0382 | 0.0340 | **+12.7795%** | +11.0445% | 7/10 |
| BEng TOKENIZE / ms | 0.0294 | 0.0283 | **+6.2275%** | +3.7020% | 7/10 |
| 中文查询 / ms | 0.3730 | 0.3757 | -0.9690% | -0.4784% | 3/10 |
| 英文查询 / ms | 0.4927 | 0.4942 | -0.4059% | -0.1925% | 5/10 |
| 混合查询 / ms | 0.4249 | 0.4305 | -1.8884% | -1.5654% | 5/10 |
| LIMIT 查询 / ms | 0.8955 | 0.9304 | -2.0862% | -4.5674% | 3/10 |

| 类别 | 配对改善中位数 | 配对改善均值 |
|---|---:|---:|
| build | +0.5018% | +0.5179% |
| tokenize | **+7.8555%** | **+7.3733%** |
| query | -1.2297% | -1.7009% |
| 三类等权综合 | **+2.5676%** | **+2.0634%** |

查询回退计入综合结果；不能将构建变化归因于本次 TOKENIZE 专用代码。两版本地固定 baseline 公式的分数均已达到 100/100，因此本轮证明的是本地耗时改善，不是新增官方分数。

## Perf 证据

基线和候选各用原始 benchmark 采样，频率 3999 Hz。最终采用 `perf record --clockid mono`，与 Python `time.monotonic()` 的阶段标记统一时钟；早先未显式设置 clockid 的采样出现阶段错位，不用于以下结论。

| 阶段 / 路径（inclusive 样本数） | 基线 | 候选 |
|---|---:|---:|
| IK 阶段总样本 | 739 | 572 |
| IK properties 重建 | 35 | 0 |
| IK dictionary 初始化/检查 | 121 | 1 |
| IK parser 名称序列化 | 5 | 0 |
| BEng 阶段总样本 | 473 | 415 |
| BEng properties 重建 | 33 | 0 |
| BEng parser 名称序列化 | 2 | 0 |

计数支持目标初始化工作基本从重复命中路径消失；采样不是调用次数，inclusive 路径有重叠，也不能将样本数直接当作耗时降幅。候选仍会在首次 miss 执行完整初始化。

## 原始证据路径

- 报告：`run_temp/tokenize-hitpath-v2-ab{1..10}-{baseline,candidate}-report.txt`
- 汇总：`run_temp/tokenize-hitpath-v2-ab10-summary.{txt,json}`
- 汇总程序：`run_temp/summarize-tokenize-hitpath.py`，读取未修改的评分脚本进行配置校验与类别划分。
- 构建日志：`run_temp/fts-tokenize-hitpath-v2-build-j8.log`
- 最终 perf：`run_temp/fts-tokenize-hitpath-v2-{baseline,candidate}-profiling-mono/`
- 被替代的 v1 patch：`run_temp/fts-tokenize-hitpath-v1.patch`

未运行 telemetry 测试；没有修改 benchmark 或推送远端。
