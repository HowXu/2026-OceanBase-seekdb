# Route B：FTS 构建复用编码排序键（2026-10-30）

## 决定：保留窄范围补丁

本机一对完整 A/B 的类别等权综合改善为 **2.9998%**，超过计划的 2% 工程门槛。构建类别 3.5844%，但并非每项变快：IK 多字段构建 -6.7527%，须保留这项不利结果。构建总时间 13.554 → 13.353 秒（不是额外一个评分项）。不承诺稳定收益或官方得分，不将未改动分词/查询路径的波动归因于排序代码。

## 原因定位

先做一次定向构建诊断，不重新采集 perf。数据来自固定 20000 行原始语料的三个 FTS DDL，不是缩小输入：

- 全局 `_enable_newsort` 为 true，但生成的在线 DDL SQL 显式写入 `opt_param('enable_newsort', 'false')`。
- 所有观测到的在线 DDL 策略记录均为 has_hint=true、can_sort_opt=false；FTS 两键分别观察到 VARCHAR 的 general_ci（45）及 binary（63）。
- 运行时出现约 7万–10万行的 chunk sort，全部 enable_encode_sortkey=false。不是 AQS 在运行中回退。
- 通用自动编码白名单不含 VARCHAR，不能全局放开；既有编码器本体包含 VARCHAR 分支及 general_ci/bin/binary 编码。
- 定向正确性测试先对比强制原排序/已有编码排序，验证 NULL、大小写/重音、尾部空格、NUL、四字节字符、长字符串、复合数字键及降序，结果一致。

## 最小补丁契约

只修改 `ObDDLTaskUtil::generate_build_replica_sql`：

1. 仅目标表为 `is_fts_index_aux()` 且原全局开关已开启时，考虑既有 newsort。
2. 逐一检查目标 rowkey：必须都是 VARCHAR，且 collation 是 UTF8MB4_GENERAL_CI / UTF8MB4_BIN / BINARY 之一。
3. 全部符合时，将该内部 DDL hint 写成 true，复用现有表达式编码与 AQS；否则保持 false。
4. 其他 DDL、其他键类型、其他 collation、全局关闭、doc-word 等非目标辅助表均保留旧路径。不改比较函数，不用 raw memcmp 代替 general_ci。

候选诊断已确认 general_ci 和 utf8mb4_bin 的在线 FTS 两键策略为 true，实际 FTS 构建的 runtime sort 也进入编码路径；小测试分别观察到 40 / 7 行。临时计数/日志在正式候选中已移除，排序器和通用优化器源码无残留改动。

## 固定条件与复现

- 直接基线：`e1eda5c6b`（生产代码等价于保留 A 的 `a6b94fb24`；A.5 已 revert）。
- 基线二进制 SHA256：`33aabda59350a8e6001cb6c44f683f8280fdda5663d8059008e7c3f3c4281b76`。
- 候选二进制 SHA256：`6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c`。
- 编译最多 -j11。两次均无 profiler/临时日志，benchmark 与评分脚本未改。
- ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0；端口 12883；memory_budget=2G、datafile_size=1G、datafile_maxsize=4G、log_disk_size=2G。
- 为避免反复复用同一个测试数据目录的历史状态影响，B 的两次启动分别使用新的 `run_temp/route-b-baseline-instance` / `run_temp/route-b-candidate-instance`，配置与执行序列相同。此举不证明前次 A.5 异常根因，未借机重跑 A/A.5。

```bash
bash run_temp/run-fts-variant.sh route-b-baseline run_temp/route-b-baseline-seekdb
bash run_temp/run-fts-variant.sh route-b-candidate run_temp/route-b-candidate-seekdb
```

## 一对完整 A/B

| 指标 | 基线 | 候选 | 改善率 |
|---|---:|---:|---:|
| build_ik_all_sec | 6.042 | 6.45 | -6.7527% |
| build_ik_content_sec | 4.749 | 4.448 | 6.3382% |
| build_beng_en_sec | 2.749 | 2.442 | 11.1677% |
| tokenize_ik_avg_ms | 0.0642 | 0.0605 | 5.7632% |
| tokenize_beng_avg_ms | 0.033 | 0.0328 | 0.6061% |
| query_cn_avg_ms | 1.3198 | 1.3343 | -1.0987% |
| query_beng_avg_ms | 1.9743 | 1.9719 | 0.1216% |
| query_mixed_avg_ms | 1.5712 | 1.4988 | 4.6079% |
| query_limit_avg_ms | 1.8995 | 1.799 | 5.2909% |

类别构建 3.5844%、分词 3.1847%、查询 2.2304%；按每类指标平均后再三类等权平均。五组回归的十个 diff 均为空，hits 两边都是 8001 / 11000 / 7332 / 20。

## 测试

新增 `unittest/storage/fts_build_sort_regression.sql/.result` 并接入现有 runner。原基线先捕获，候选逐字节对比；覆盖已有编码排序与通用排序的精确顺序、FTS CI/二进制、多字段、相关度、更新/删除/插入，以及 Unicode_CI 非白名单回退。非白名单用例在完整 A/B 后追加并分别在同一基线/候选验证，不重跑性能。

原始诊断和报告位于 `run_temp/route-b-*`；本记录包含可长期保留的核心数据。
