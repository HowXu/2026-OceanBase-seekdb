# Route A：私有词频聚合实验结果（2026-10-30）

## 决定

保留本轮 A 补丁，作为下一实验的直接基线。复测类别等权综合改善 **3.3662%**，超过计划中的 2% 工程门槛；目标分词类别改善 9.2725%。这只是一对本机完整 A/B，不代表稳定统计收益或官方成绩。

查询类别本次为 -2.2252%，其中 mixed 查询 -8.4036%，存在回退风险，不宣称 A 改善全部工作负载。查询样本有波动，不据此证明具体代码因果；后续 C 以保留 A 的版本为直接基线。首轮异常分词慢本次未复现，原因尚未确认，不将其删除或称为已修复。

## 版本与复现条件

- 当前分支：`2026-competition-fts-clean-base`。
- 起点：`3ced67e17e01d9c86ad9474f9028af23451a12bf`；只 cherry-pick `bc6dfc916440a274a2364b56b6f7ade7a99064c1`，新提交 `1bf3c8435460d67d736bb5d218f6c5e453c49721`。
- 两提交 tree 均为 `57c4df55d00fada77381f3ba9cb6b2e9f8890df9`。bc6 的直接父提交本来就是 3ced，因此分支切换未改变源码内容。
- 基线二进制 SHA256：`e390bb3367f6936a4c076b3b221f9e4910cd6346dc8b1c05762e80721e724bb0`。
- 候选二进制 SHA256：`33aabda59350a8e6001cb6c44f683f8280fdda5663d8059008e7c3f3c4281b76`；与当前已构建文件一致，未重复构建。
- 两报告的 git_head 都为新分支初始 HEAD，git_dirty=1；报告记录的是驱动脚本所处源码状态，不是二进制内嵌版本。基线不含 A，候选含 A，由以上独立二进制身份区分。
- ROWS=20000，BATCH=500，ROUNDS=3000，QUERY_ROUNDS=200，SAMPLES=3，WARMUP=30，SKIP_LOAD=0。
- 端口 12883；memory_budget=2G，datafile_size=1G，datafile_maxsize=4G，log_disk_size=2G；无 profiler/临时热路径插桩。
- 用户打断的最早复测未完成，无有效成绩，日志保留为 `run_temp/route-a-recheck-interrupted-*`。

```bash
bash run_temp/run-fts-variant.sh route-a-recheck-baseline run_temp/route-a-baseline-seekdb
bash run_temp/run-fts-variant.sh route-a-recheck-candidate run_temp/route-a-candidate-seekdb
```

临时助手通过直接启动 seekdb、等待 SELECT 1、运行 BEng/Unicode/IK-chain/word-map 回归，再以以上固定配置调用未修改的 `tools/benchmark/fts_large_bench.sh`；退出时停止测试实例。长期复现需要先保存对应版本的两个二进制，不能用当前候选替代基线。

## 本次复测（2026-10-30 08:01）

| 指标 | 基线 | 候选 | 改善率 |
|---|---:|---:|---:|
| build_ik_all_sec | 6.247 | 6.045 | 3.2336% |
| build_ik_content_sec | 4.849 | 4.742 | 2.2066% |
| build_beng_en_sec | 2.747 | 2.645 | 3.7131% |
| tokenize_ik_avg_ms | 0.0697 | 0.0671 | 3.7303% |
| tokenize_beng_avg_ms | 0.0378 | 0.0322 | 14.8148% |
| query_cn_avg_ms | 1.3684 | 1.3506 | 1.3008% |
| query_beng_avg_ms | 2.0084 | 2.0251 | -0.8315% |
| query_mixed_avg_ms | 1.4577 | 1.5802 | -8.4036% |
| query_limit_avg_ms | 1.8213 | 1.8389 | -0.9663% |

构建类别：3.0511%；分词类别：9.2725%；查询类别：-2.2252%。类别内部平均相应指标改善率，再对三个类别等权平均。build_total 不作为第四个构建评分项。

- build_total：13.856 → 13.445 秒。
- SELECT 1 均值：0.0219 → 0.0213 ms；基线三个样本 0.0188 / 0.0244 / 0.0226，候选 0.0208 / 0.0198 / 0.0234。
- IK 三样本：基线 0.0700 / 0.0689 / 0.0703，候选 0.0647 / 0.0702 / 0.0665 ms。
- BEng 三样本：基线 0.0398 / 0.0383 / 0.0354，候选 0.0302 / 0.0341 / 0.0322 ms。
- mixed 查询三样本：基线 1.5250 / 1.5055 / 1.3427，候选 1.5824 / 1.6503 / 1.5079 ms。
- 四组回归输出逐字节一致（八个 diff 均为空）；查询 hits 两边均为 8001 / 11000 / 7332 / 20。

## 首次测量（保留不利证据）

| 指标 | 基线 | 候选 | 改善率 |
|---|---:|---:|---:|
| build_ik_all_sec | 6.453 | 6.142 | 4.8195% |
| build_ik_content_sec | 4.947 | 4.951 | -0.0809% |
| build_beng_en_sec | 2.836 | 2.745 | 3.2087% |
| tokenize_ik_avg_ms | 0.0683 | 0.2392 | -250.2196% |
| tokenize_beng_avg_ms | 0.0367 | 0.0951 | -159.1281% |
| query_cn_avg_ms | 1.4257 | 1.4053 | 1.4309% |
| query_beng_avg_ms | 1.9285 | 2.029 | -5.2113% |
| query_mixed_avg_ms | 1.604 | 1.477 | 7.9177% |
| query_limit_avg_ms | 1.7892 | 1.8997 | -6.1759% |

首轮类别综合改善：-67.5115%。候选 IK 0.2332 / 0.2369 / 0.2474 ms；BEng 0.1667 / 0.0777 / 0.0408 ms，后者在同次运行中明显回落。两轮不能合并后挑有利样本；首轮与复测差异意味着因果仍有限制。

## 补丁契约与检查

- 五个函数内词频 map 同步使用，未跨线程逃逸；仅改文档私有 map 防护模式。
- 单次 set_or_update 保留首次计数 1、重复词累加、general_ci hash/equality，以及原 overwrite 的最后拼写语义。
- 只统一三个手写 map/iterator 类型；不改词典 loader、全局 stopword 锁、分词算法、BM25、benchmark 或评分输入。
- 新 word-map SQL/result 在原基线和候选均通过；证据和测试独立于 A 生产提交保存，以便未来定向 revert。
