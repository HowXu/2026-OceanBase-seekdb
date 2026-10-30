# T0-internal/B0-reuse：原始 IK token 序列缓存

## 决定

**按用户“有改善就留”的口径暂保留。** 编译及唯一一对完整原始 benchmark 均成功，两端 hits 均为 `8001 / 11000 / 7332 / 20`。完整报告构建类别 +0.1319%、分词 +6.6883%、查询 +5.2513%、综合 +4.0239%。

**预期的第二次 content 构建收益未出现**：首次 IK 构建 −1.6877%、第二次 IK content −2.2022%。不能把未启用缓存的 BEng 构建改善或其他波动解释为跨 DDL 缓存收益，也不能声称已经证明 content 构建加速。不重跑、不挑样本；这些是单对本地结果，官方 CI 待验证。

未新增或运行额外功能回归、SQL/golden/单元测试、EXPLAIN 或 profiler。

## 版本与实现

- 起始生产 HEAD：`f65055696`，A+B+T0-SQL+B0-NONE。
- 只修改 `src/data_plane/api/data_plane/fts/ob_fts_parser_helper.h` 和 `src/storage/fts/ob_fts_parser_helper.cpp`，不改变公开 `ObFTParseHelper::segment()` 签名。
- 仅 IK 启用进程内共享缓存：16 个独立锁分片，每片 8 MiB/4096 条，总缓存计费上限 128 MiB、条目上限 65,536。计费包括 key、token 字节、字符串/向量 capacity、条目结构及额外元数据余量，不只统计条目数；达到分片上限时清空该片重建。
- key 深拷贝全文原始字节/长度、含版本 parser 名、完整 properties，并比较 charset、collation；hash 相等仍比较完整 key。当前 SeekDB 是进程级单库 runtime，不存在 `MTL_ID` 租户切换接口，未引入多租户路由或伪造租户 key。
- `should_read_newest_table()` 当前恒 false，沿用现有固定词典缓存；没有词典重载功能变更或裸 dictionary 指针缓存。
- miss 在原 token 迭代中记录成功 `process_word()` 的原始 `(bytes, char_cnt, word_freq)`，只有完整到达 `OB_ITER_END` 才发布；过滤成功的原始 token 也保留，以便原样重放处理过程。
- hit 按原始顺序调用现有 `ObAddWord::process_word()`，由调用方生成自己的 map 和 doc_len，不复制缓存 map、不改变 flags/频次/累计顺序。
- 条目以 `shared_ptr<const ...>` 发布；查找后释放锁再重放。被淘汰但仍有读取者的条目由共享所有权维持至读取完成，构造中及在途 pin 属于临时工作内存，不计为仍驻留缓存。
- 重放 token 字节先深拷贝到调用方 arena，避免 word map 的浅引用在本次缓存 handle 释放或条目淘汰后失效。发布/记录的可选缓存分配失败不替换原解析成功结果。

| 二进制 | SHA256 |
|---|---|
| `run_temp/score-b0-seekdb` | `7ba0eb2792195831a682b8250c5f6c9466c02d8d562fb3fbb032448d0274fcfc` |
| `run_temp/score-reuse-seekdb` | `e45cf6d49e92efae40469909dde4086c0b814b0200c5b03b164e6c71948f31bd` |

成功构建日志：`run_temp/score-reuse-build-final.log`，最多 `-j11`。首次编译暴露 const serializer 调用和不存在的旧租户 API 两处错误，已在 A/B 前修正：对局部 parser 副本序列化，并沿用实际单库模型。

## 唯一完整 A/B

```bash
bash run_temp/run-fts-score-route.sh score-reuse-baseline run_temp/score-b0-seekdb
bash run_temp/run-fts-score-route.sh score-reuse-candidate run_temp/score-reuse-seekdb
```

两端新实例，port=12883，memory_budget=2G，datafile_size=1G，datafile_maxsize=4G，log_disk_size=2G。原始脚本未修改，固定 `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。没有与编译或 profiler 并行。

| 指标 | 直接基线 | 候选 | 改善 |
|---|---:|---:|---:|
| build_ik_all_sec | 5.866 | 5.965 | **−1.6877%** |
| build_ik_content_sec | 4.450 | 4.548 | **−2.2022%** |
| build_beng_en_sec | 2.450 | 2.345 | +4.2857% |
| tokenize_ik_avg_ms | 0.0381 | 0.0365 | +4.1995% |
| tokenize_beng_avg_ms | 0.0316 | 0.0287 | +9.1772% |
| query_cn_avg_ms | 1.4482 | 1.3576 | +6.2560% |
| query_beng_avg_ms | 1.9849 | 1.9397 | +2.2772% |
| query_mixed_avg_ms | 1.6209 | 1.4828 | +8.5200% |
| query_limit_avg_ms | 1.8497 | 1.7766 | +3.9520% |

类别采用类内平均，再三类等权；上述综合结果不能与前两阶段百分比相加。原始评分脚本相对仓库自带 CI baseline 的两端本地 score 均为 100.0（饱和），不是官方 CI 成绩。

## 证据

`run_temp/score-reuse-binaries.sha256`、`score-reuse-{baseline,candidate}-report.txt`、对应 `-benchmark.log`、`-score.json`、`score-reuse-comparison.json`。服务退出，实例 store 删除，完整日志/配置保留。最后按计划重建实际最终生产版本并运行一次原始 benchmark，不追加功能测试。
