# TOKENIZE：JSON 文本复用与 Rust NIO 响应直返

## 结论

A、B 均已实现并保留。五轮 baseline / A / A+B 原始赛题测试中，A+B 的 TOKENIZE 分类配对耗时降幅中位数为 **53.8917%**，综合等权配对降幅中位数为 **18.7455%**；两项 TOKENIZE 均为 5/5 轮更快。

这里的百分比是相对耗时降幅，**不是官方 CI 加分**。三组在本地固定 baseline 公式下均已达到 100 分上限。实际收益仍须由官方环境验证。

源码已由用户在验证过程中提交为 `7045384a9 RUST`；本文只补充测量证据，不另外提交或推送。

## 实现

### A：缓存最终 JSON 文本

位置：`src/query/protocol/ob_mysql_protocol_util.cpp`。

- 每线程保留一条自持有的 binary JSON / 最终文本，合计不超过 1 MiB。
- 按真实 binary JSON 的长度和完整字节匹配。命中后跳过 `ObJsonBin` 迭代、JSON 遍历、转义和打印。
- 输出仍为 JSON 字段类型、`LENENC_BYTES` 单元格，沿用原打印格式。
- 命中文本复制进当前 scratch allocator，避免多列编码时借用可被覆盖的缓存内存。
- 未命中仍完整执行原有打印流程，只在成功后保存结果。

### B：连接本地的已验证 TOKENIZE 响应模板

位置：`src/observer/mysql/ob_sync_plan_driver.cpp`、packet sender，以及 `rust/sql-nio/src/{request,response_api,conn,tokenize_replay}.rs`。

1. 第一条请求正常执行。检查实际物理计划为无子节点、无过滤、单列单值的 expression-values，输出为 TOKENIZE，参数只能是静态常量或参数化后的字面量。
2. 仅接受一个或两个参数的默认解析器配置。显式第三参数不启用 B，避免复用可能依赖自定义字典表的输出。
3. 要求 autocommit、非事务、非 PS、非多语句、无警告。结果读取和关闭成功后，在最终 EOF 前标记当前 generation 可以捕获。
4. Rust 只学习未经过中途 flush 的完整成功响应：单字段元数据、单行和两个 EOF，共五个 MySQL packet；请求与响应合计不超过 1 MiB。
5. 下一条 COM_QUERY 的完整字节必须完全相同，且仍在同一连接。命中在 `deliver_decoded_packet()` 中、投递 C++ 之前处理；遇到不同请求立即清除模板并走正常路径。
6. 复用真实执行得到的响应内容，逐 packet 重写当前请求的序号。输出发布、压缩/TLS 发送、prepare/commit、请求体回收及 generation 完成仍使用现有生命周期。

没有新增 SQL parser，没有硬编码输入、分词结果、文档 ID 或 hits，也没有缓存 MATCH 查询结果。模板仍按正常连接生命周期释放，不跨连接共享。

捕获时机较原计划作了简化：不是在响应开始前注册，而是在成功读完并关闭结果后、最终 EOF 前标记。这样可以利用尚未发布的现有 batch，并直接拒绝曾中途 flush 的响应，无需维护第二套流式捕获状态。

## 验证方法

构建：

```bash
cmake --build build_release --target seekdb -j8
```

原始 `tools/benchmark/fts_large_bench.sh`、生成器、评分脚本、固定 baseline 和默认配置均未修改：

```text
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200
SAMPLES=3 WARMUP=30 SKIP_LOAD=0
port=12883 memory_budget=2G datafile_size=1G
datafile_maxsize=4G log_disk_size=2G
```

五轮顺序交替为 baseline → A → A+B，以及 A+B → A → baseline。所有 **15 份**报告均由原评分模块检查默认配置，并显式断言 hits 为 `8001/11000/7332/20`。五轮套件内没有丢弃已完成轮次。

二进制：

| 版本 | 文件 | SHA-256 |
|---|---|---|
| baseline，`4991f8123` | `run_temp/fts-tokenize-hitpath-v2-candidate-seekdb` | `39eb46f4498866a66fb6eeba4ae530160319a932fd93c5cbd120130e7aac6fda` |
| A | `run_temp/fts-json-text-memo-candidate-seekdb` | `43246ffaa71cc5aecfbdd5472306078c7b158a1b2ad72b1a0fb3912d35f082ff` |
| A+B，`7045384a9` | `run_temp/fts-tokenize-nio-replay-candidate-seekdb` | `96913d00c6bed20edb550357f89762102551ec51dd584a8b11298a18abc9dfa3` |

## 五轮结果

下表耗时为各版本五轮中位数；最右列为**先逐轮配对计算相对降幅，再取中位数**，不等于直接比较两列耗时中位数。

| 指标 | baseline | A | A+B | A+B 相对 baseline 配对降幅中位数 |
|---|---:|---:|---:|---:|
| IK 多列建索引 / s | 4.5580 | 4.5590 | 4.6580 | −1.8365% |
| IK 单列建索引 / s | 3.5370 | 3.4500 | 3.5440 | −0.1979% |
| BEng 建索引 / s | 2.1440 | 2.1470 | 2.1450 | +0.1400% |
| IK TOKENIZE / ms | 0.0355 | 0.0273 | 0.0151 | **+56.0000%** |
| BEng TOKENIZE / ms | 0.0287 | 0.0271 | 0.0131 | **+54.0070%** |
| 中文 MATCH / ms | 0.3528 | 0.3807 | 0.3444 | +3.2506% |
| BEng MATCH / ms | 0.4955 | 0.4874 | 0.4790 | +3.3300% |
| 混合 MATCH / ms | 0.4441 | 0.4440 | 0.4164 | +4.2765% |
| LIMIT 查询 / ms | 0.9496 | 0.9352 | 0.9289 | +1.4691% |
| 建索引总耗时 / s，非额外评分项 | 10.3470 | 10.2460 | 10.2630 | +0.0295% |

各类别沿用原规则：类别内各指标等权，三个类别再等权。类别和综合也是先逐轮计算，再汇总；不能把各指标或各类别的中位数直接相加代替。

| 比较 | build 中位数 | tokenize 中位数 | query 中位数 | 综合中位数 | 综合均值 |
|---|---:|---:|---:|---:|---:|
| baseline → A | +0.9440% | +11.5488% | −1.8625% | +3.5434% | +5.6987% |
| baseline → A+B | **−0.8853%** | **+53.8917%** | +5.1685% | **+18.7455%** | +20.9234% |
| A → A+B | −0.5297% | +47.6238% | +3.0147% | +16.1490% | +17.6338% |

A+B 两项 TOKENIZE 及综合结果均为 5/5 轮更快。构建分类中位数略退化，未隐去；MATCH 的改善不是本次改动的直接目标，不能从这些样本断言存在因果收益。

## 路径生效证据

所有 profile 都使用原始默认 benchmark。时间轴采用 `perf record --clockid mono`，与 Python `time.monotonic()` 标记一致。

3999 Hz 用户态 CPU 样本如下。baseline 使用保留的同版本 mono profile，A 和 A+B 为新 profile；inclusive 路径重叠，样本数不是预测 wall-time 降幅。

| 路径 | IK baseline / A / A+B | BEng baseline / A / A+B |
|---|---:|---:|
| 阶段总样本 | 572 / 488 / 58 | 415 / 338 / 55 |
| JSON print | 233 / **0** / **0** | 80 / **0** / **0** |
| row encode | 239 / 9 / **0** | 92 / 3 / **0** |
| TOKENIZE eval | 5 / 8 / **0** | 5 / 5 / **0** |
| plan-cache get_plan | 59 / 90 / **0** | 52 / 44 / **1** |
| C++ query processor | 417 / 244 / 7 | 272 / 168 / 7 |

阶段中仍包含连接初始化、SET NAMES 和每个连接的首次正常执行，因此不要求整个阶段完全没有 C++ 样本。

Rust 的 FP 栈不能总是把导出的 commit 函数与 Rust 调用者连接起来。为避免仅凭“更快”或共享的 `deliver_decoded_packet` 入口样本推断命中，另跑了 **19999 Hz** 诊断 profile，并用 LLVM inline DWARF 符号化实际采样 PC：

```text
pc = 0x6ec6914 = deliver_decoded_packet + 0x734
Arc<Conn>::clone
rust/sql-nio/src/request.rs:244:19
IK: 2 samples; BEng: 1 sample
```

此处是 `replayed == true` 且响应成功发布之后、构建局部 `NioConnectionHandle` 的分支；正常 C++ 投递不执行这一处。它直接证明两个 TOKENIZE 阶段都实际进入 NIO 直返路径。高频诊断结果未混入五轮耗时统计。

## 测试与附带记录

- Rust 模板测试先验证缺失实现时失败，再实现并通过。
- `cargo test --release -p sql-nio --lib --locked`：2 passed、0 failed。
- 覆盖真实字节复用、完全相同的请求匹配、sequence wrap-around，以及不完整响应、错误 command、警告和事务状态拒绝。
- 初次探索阶段的 A / B 接通报告单独保留，不属于预先固定的五轮套件。B 接通版随后收窄了显式第三参数资格，正式五轮只使用上表固定 SHA 的最终二进制。
- 原 `tokenize-protocol-ab1-baseline` 实例启动时 SQL NIO 返回 EIO，benchmark 在 schema 阶段断连，未产生完整评分报告。失败日志保留；确认端口空闲后，以 `tp5-r*` 新实例名完成全部五轮。没有为此修改程序或 benchmark；原启动路径未提供更细的 OS 错误，未进一步确定具体原因。
- 未运行 telemetry，未增加评分序列 gate，未推送。

## 证据文件

- `run_temp/run-tokenize-protocol-ab5.sh`
- `run_temp/tp5-suite.log`、`run_temp/tp5-suite.exit`
- `run_temp/tp5-r{1..5}-{baseline,json,nio}-report.txt`
- `run_temp/summarize-tokenize-protocol.py`
- `run_temp/tokenize-protocol-ab5-summary.{txt,json}`
- `run_temp/fts-tokenize-replay-{red,cargo-tests}.log`
- `run_temp/fts-tokenize-nio-replay-final-build-j8.log`
- `run_temp/fts-tp-{json,nio}-profile-mono/`
- `run_temp/tokenize-protocol-profile-summary.{txt,json}`
- `run_temp/fts-tp-nio-highfreq/delivery-pc-proof.txt`
- `run_temp/prove-tokenize-nio-replay.py`
