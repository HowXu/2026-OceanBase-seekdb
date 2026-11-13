# MiniVectorDB 首轮性能优化设计

日期：2026-11-12
基线：bbc17f7ac26678f8ce6ea1032b5966ed878c2a3d

## 1. 目标与证据边界

针对本地 perf 已确认的三个热点进行独立优化：逐字节 ifstream 解码、每次查询重新打开/检查多段文件、INSERT 为重复键检查扫描完整逻辑表。

基线工具与原始结果保持不变：

- `tools/benchmark/`
- `.superpowers/sdd/performance-baseline/rounds/`
- `.superpowers/sdd/performance-baseline/profile-write2/`
- `.superpowers/sdd/performance-baseline/profile-query/`

每个优化先写 RED，再运行原正确性测试和同一单轮负载；全部完成后做三轮对比。T/B/M 同时观察，不能只优化 T 而隐藏 B/M 退化。不虚构平台 E 或分数。

## 2. 有界缓冲和向量块解码

`storage::Input` 保持现有构造、remaining、magic、end 接口，新增64 KiB内部缓冲区。逻辑 `remaining()` 始终表示调用方尚未消费的字节，包括缓冲区内字节。

`bytes(char*,n)`：

1. 先检查 n 不超过逻辑剩余；
2. 从已有缓冲复制；
3. 缓冲为空时最多预读64 KiB；
4. 对大型目标且缓冲为空时直接读入目标，避免额外复制；
5. 短读、截断和尾随字节的现有错误不变。

新增 `floats(float* destination,uint64_t count)`，检查乘法溢出和字节边界后整块读取。TableFileReader 和 DeltaFileReader 的 VECTOR 列都改用该方法。宿主仍为固定的 Linux x86_64 little-endian IEEE float32，文件字节序不变。

Output、Catalog magic、段 magic、CURRENT 发布顺序均不变。缓冲区大小乘以持有段数构成有界内存开销，在三轮 M 中观察。

## 3. 持有并重置逻辑表 reader

TableEntry 增加惰性、可共享的 `shared_ptr<LogicalTableReader>`。由于当前严格单进程、单客户端，同一可见段序列可复用 reader；Catalog 根复制时共享未修改表的 reader。

新增：

- `Input::reset()`：清除流状态并回到文件开头，清空缓冲，不重新打开路径；
- TableFileReader/DeltaFileReader `reset()`：重置 ended、last key 并重新验证已有头部；
- LogicalTableReader `reset()`：重置所有段游标和最小堆，准备下一次全表扫描。

Executor 的 `table_reader(entry,store)` 在对象不存在时创建，否则复用；每次扫描前显式 reset。SELECT、INSERT、UPDATE/DELETE 和合并路径都使用同一可见 reader。

Catalog.replace_objects 在表段变化时清空该候选表的 reader；未修改表及旧根继续持有旧 reader。事务回滚只需丢弃候选根，下一次使用已提交 reader 前仍 reset。GC 的存活段规则不变，旧根/事务根持有的 reader 只能引用受保护段。

RootStore 启动验证可继续使用一次性 reader；运行期 SELECT/DML 使用持有 reader。该优化不改变段数或归并语义。

## 4. INSERT 的 COW 主键集合

TableEntry 增加惰性 `shared_ptr<unordered_set<int64_t>> primary_keys`。

第一次需要时通过当前逻辑 reader 建立 PK 集合。INSERT 仍先绑定全部行并检查批内重复，再用集合检查已存在键；成功后创建旧集合的副本并加入本批键。这样即使集合由旧根共享，也不会原地污染旧状态。

append_delta_or_compact 成功后才把新集合放入候选 TableEntry。若段写入、发布或后续语句失败，旧根集合保持不变。显式事务中的后续 INSERT 看到事务候选集合；ROLLBACK 丢弃它；恢复后集合惰性重建。

UPDATE/DELETE 不允许主键更新，因此保留原集合。新建空表从空集合开始。该集合只保存主键，不全量保存 VARCHAR/VECTOR 行；超内存正确性仍需复测。

## 5. 测试与测量

### 解码 RED/GREEN

- 跨64 KiB边界的行、VARCHAR、向量和多个段；
- 截断、错误长度、footer、reset后重复读取；
- float bits、负零、最小/最大有限值；
- 全部现有 CTest/pytest。

### 持有 reader RED/GREEN

- 同一表连续 SELECT 的段文件打开次数不增加；使用测试构建计数，不给生产协议增加调试输出；
- 修改后看到新段，回滚后看到旧段，restart后重建；
- 多表、事务、合并和故障注入测试保持通过。

### PK 集合 RED/GREEN

- INSERT 重复检查不扫描向量行；测试构建计数验证；
- 批内重复、已提交重复、事务内重复、回滚后重插入；
- 失败语句和失败发布不污染旧集合；
- 重启、删除后重插入和跨合并一致。

### 性能验收

每一步使用相同种子和镜像构建方法做一轮比较；最终完成三轮。报告原始 T/B/M、median 和相对基线变化，同时保留无收益/退化结果。最后再跑一次 perf，判断热点是否实际移动。

本轮完成后不自行 commit/push。
