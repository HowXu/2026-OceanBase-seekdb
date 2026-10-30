# Route A.5：IK 字符编码复用（2026-10-30）

## 决定：收益未证明，候选提交后 revert

只完成一对完整 A/B，不追加多轮跑分。虽然按报告机械计算综合改善为 24.4380%，但**不能把这个数当成优化收益**：未改动 DAT 路径的 BEng 也由 0.1050 降到 0.0327 ms，基线分词阶段明显异常。基线 IK 0.2078 / 0.2105 / 0.2280 ms；BEng 0.1615 / 0.1120 / 后续恢复的第三样本。此前同一基线二进制在 A 复测中 IK 为 0.0671、BEng 为 0.0322 ms；这些跨轮数据只能提示异常，不拿来拼出有利对照。

正确性已验证，优化价值尚未可靠测出。依照计划的“未证明收益则撤回”规则，保留扩展测试和本记录，单独提交生产候选后定向 revert，继续 B/C。

## 基线、补丁和所有权

- 直接基线为保留 A 的 `a6b94fb24`。
- 基线二进制 SHA256：`33aabda59350a8e6001cb6c44f683f8280fdda5663d8059008e7c3f3c4281b76`。
- 候选二进制 SHA256：`11ced32f3a7003a72374542b0b3d2d504c64e7edb5fc839f755bd4ce95493b08`。
- `ObIKFTParser::build_dict_from_cache` 为每个 parser 创建 range/cache 字典包装；reader 属于包装；parser 的 KVCache handles 在解析期间 pin 住不可变 DAT。
- 候选仅在 `ObFTDATReader` 内缓存上一个字符的值、副本 code 和成功/未找到状态；每个 reader 只对应一个稳定 DAT，因此不跨字典共享编码、不持有新的裸指针、不改变 DAT 转移或仲裁。
- 固定配置与 A 相同：ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0；2G memory_budget；端口 12883。
- 编译命令：`cmake --build build_release --target seekdb -j11`；临时计数已从正式候选中移除。

## 重复查询证据

在扩展的 IK SQL 上，仅对每个 reader 的第 32 次查询输出一次局部快照，未逐词日志，未挂 profiler。22 个快照总计 704 次查询、152 次连续相同字符查询（21.5909%）；重复计数分布为 0×8、8×6、16×2、17×2、13×2、6×2。

这些是有选择的前缀快照，包含不同字典 reader，证明存在可复用工作，不代表完整 workload 的命中率或 CPU 占比。原始数据：`run_temp/route-a5-lookup-snapshots.txt`。

## 完整 A/B 原始结果（不作为可信收益）

| 指标 | 基线 | 候选 | 表面改善率 |
|---|---:|---:|---:|
| build_ik_all_sec | 6.647 | 6.246 | 6.0328% |
| build_ik_content_sec | 4.953 | 4.961 | -0.1615% |
| build_beng_en_sec | 2.745 | 2.745 | 0.0000% |
| tokenize_ik_avg_ms | 0.2154 | 0.0649 | 69.8700% |
| tokenize_beng_avg_ms | 0.105 | 0.0327 | 68.8571% |
| query_cn_avg_ms | 1.4409 | 1.3118 | 8.9597% |
| query_beng_avg_ms | 2.0123 | 1.968 | 2.2015% |
| query_mixed_avg_ms | 1.578 | 1.5913 | -0.8428% |
| query_limit_avg_ms | 1.8724 | 1.9163 | -2.3446% |

构建类别 1.9571%，分词类别 69.3636%，查询类别 1.9934%。分词控制项异常使综合值失去直接归因依据。四组回归均逐字节一致；hits 都为 8001 / 11000 / 7332 / 20。

```bash
bash run_temp/run-fts-variant.sh route-a5-baseline run_temp/route-a5-baseline-seekdb
bash run_temp/run-fts-variant.sh route-a5-candidate run_temp/route-a5-candidate-seekdb
```

扩展的 `fts_ik_chain_regression.sql/.result` 在改代码前用基线捕获，再在基线/候选上对比。增加连续歧义词、跨首字范围、重复字符、四字节字符、emoji、NUL 和 smart/max_word 详细词频，原有八条输出保留。
