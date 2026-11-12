# MiniVectorDB 判题流程逐条合规重审

## 0. 审计对象、结论与证据范围

本轮任务是核对流程，不是再猜一个 SQL 功能修补。**没有修改生产实现、Dockerfile、协议、自测驱动、公开 SQL、断言或配置；没有提交或推送。**

核验的交付三元组：

```text
HTTPS URL: https://github.com/HowXu/2026-OceanBase-seekdb.git
分支:      2026-competition-round2
commit:    c4a54ebef06bf7ab5686b2c4866b53c658782b62
```

实际从该公开 HTTPS URL 无交互凭据克隆，确认 HEAD 与上述完整 SHA 一致，再直接以干净克隆为构建上下文执行 Docker 无缓存构建；没有使用宿主 .venv、build、未提交补丁或预编译扩展。

```text
Docker client/server: 28.5.2+dfsg4
平台: linux/amd64；cgroup v2
镜像标签: minivectordb:flow-reaudit-c4a54ebef
本地镜像 ID: sha256:325ba39630db368a05046eb9ae2fecafb6c9a6f76834dac00765b7746db44998
```

镜像 ID 是本次本地构建的内容标识，**不是平台回执中的锁定镜像摘要**。当前 origin 分支与本地 SHA 一致，官方分支仍为 `f4d4e26653af823a7601668802f80e374ad6643c`；本提交的 STDIO_PROTOCOL.md、protocol_check、mysqltest 与该官方版本无差异。

### 总结

1. **按当前官方题包的 JSONL 管道流程，本机没有复现流程失败。** 主动 ready、逐请求 flush、stdout/stderr 分离、退出、重启、原 `--image` 入口均有本轮实测证据，而不是用原生 Engine 单元测试替代。
2. **协议核验以指定分支公开题包和实际驱动为准：**它们明确要求 JSONL。网页“此前示例”的指代不明确，不能仅凭这句话认定当前 round2 公开协议冲突、要求裸 SQL，或认定现实现的 JSONL 不合规。此前将它直接列为协议冲突的判断过强，现撤回该判定；保留原文仅作来源说明。
3. **交付材料并不齐全：**`agent/conversation.json` 缺失；根 conversation.json 为 0 字节，不是合法 JSON。按评分说明这影响 Agent 材料；是否在平台 SQL 执行前被完整性预检拦截，尚无平台证据，不能认定为此次 MySQL 未通过的原因。
4. **版本锁定不完整：**Python、SQLGlot、pybind11 固定了版本，但基础镜像未锁 digest，apt 构建工具未精确锁版本。构建成功不能抹去这项可复现性交付缺口；也没有证据证明它造成当前 0 分。
5. **本轮补上实际超内存和恢复中再强杀的证据：**2 GiB 容器内通过 stdin 写入 2,112 MiB 有效负载，查询和两次故障恢复均通过，包括在恢复读表、尚未 ready 时再次 KILL。仍不是全部正式隐藏数据或所有资源组合的认证。
6. 平台实际锁定的 SHA、镜像和协议版本尚未取得，**正式 0 分的根因仍未定位**。本报告不能得出“流程一定没问题”或“实现必然满足所有正式输入”。

### 按公开题包再次复核

依据用户明确的核验口径，重新对照官方分支 `f4d4e26653af823a7601668802f80e374ad6643c` 的公开题包与检查驱动，再从干净克隆运行原始三步：握手 PASS、单独 sql_crud PASS、全部公开 20/20 cases / 95 SQL PASS；所有公开用例 stderr 为 0 字节，未出现 TIMEOUT/SKIPPED。复用了此前由同一干净提交构建的不可变实际 Docker 镜像，没有声称重新构建，也没有 --command、--config 或私有启动参数。

最新原始记录：`.superpowers/sdd/public-package-recheck/run-eKPRnTY8/`，汇总 `summary.json`。按这个明确可执行的公开标准，未发现当前实现的 JSONL、主动 ready、id/ok、结果比较、重启或退出对接错误；无需改为裸 SQL。

### 本轮赛题正文来源

除重新阅读保存的 HTML 外，还通过公开正文接口取得当前完整 lake 文档：`https://oceanbase.yuque.com/api/docs/291289948?book_id=81051108`，元数据 content_updated_at 为 `2026-11-11T10:25:23.000Z`。解析其中 card 的 URL 编码 JSON，取回 SQL、Dockerfile、docker build、docker run 四个原始代码块。因此旧审计中“§4.2.1 代码块缺失、无法逐字核验”的限制在本轮已经解除。当前正文仍有“此前示例”的措辞，但指代不明确；本报告按其指向的当前分支题包和实际驱动核验，不再据此推断另一套裸 SQL 契约。

### 状态与证据标识

- **通过**：本轮有对应源码核对及/或实际 Docker 管道实测，仅限所写范围。
- **部分/未验证**：要求涉及尚未公开/取得的信息，或本轮没有完整覆盖，不能当作通过。
- **不满足**：存在明确可见缺口；不自动等同于正式失败根因。

原始证据均在 `.superpowers/sdd/flow-reaudit/`（本地诊断目录，未作为提交内容）：

| 编号 | 证据 |
|---|---|
| E1 | `https-clone.log`、`source.sha256`、`docker-version.log`、`docker-info.json` |
| E2 | `docker-build.log`、`image-inspect.json`、`runtime-dependencies.json` |
| E3 | `readme-step1-handshake/result.json`、`readme-step2-crud/result.json`、`readme-step3-public/result.json` |
| E4 | `doc7-verbatim.sh`、`doc7-result.json`、`doc7-requests.jsonl`、`doc7-stdout.jsonl`、`doc7-stderr.log` |
| E5 | `interactive-protocol.trace.jsonl`、同名 stderr.log、`pipe-audit.json` |
| E6 | `requirements-sql.trace.jsonl`、同名 stderr.log、`pipe-audit.json` |
| E7 | `cross-table-lifecycle.trace.jsonl`、同名 stderr.log、`pipe-audit.json` |
| E8 | `startup-negative.json`、`startup-negative.stdout`、`startup-negative.stderr` |
| E9 | `resource-probe.json`、`bounded-resource.trace.jsonl`：256 MiB/320 MiB 诊断 |
| E10 | `resource-probe-2g.json`、`bounded-resource-2g.trace.jsonl`、`resource-summary.json`：2 GiB/2,112 MiB 复测 |
| E11 | `delivery-materials.json` |
| E12 | `web421-result.json`、`web421-stdout.jsonl`、`web421-stderr.log`：网页启动示例，仅换唯一卷名/镜像标识并增清理用 name |
| E13 | `web22-result.json`、`web22-literal-example.trace.jsonl`：网页原始七条多行 SQL 经 JSONL stdin 执行 |
| E14 | `yuque-doc-api.json`、`api-content.txt`、`requirements-current.txt`、`requirements-current-metadata.json`、`requirements-codeblocks.json`：当前完整赛题与原代码块 |

所有用于语义验收的 SQL 都经 `docker run -i` 的 stdin 写入，响应只从该进程 stdout 读取，stderr 独立保存；**没有通过 native Engine API、HTTP、TCP、MySQL 网络端口或 docker exec 来执行验收 SQL、填充数据或读取查询答案**。`docker inspect`、ldd/包版本检查、只读文件大小/cgroup 检查及恢复 FD 观察仅用于检查运行环境，不是数据库接口。恢复观察没有修改数据文件或开启生产故障注入。

## 1. 赛题 §2.1：实现范围

| 条目 | 要求 | 核验依据与结果 | 状态 |
|---|---|---|---|
| 2.1-01 | 单机、单进程、单客户端；至多一个显式事务 | Python 前台进程以内嵌 pybind11 调用自写 C++，没有子数据库服务；第二次 BEGIN 报 transaction_active，原事务仍能继续。E5/E6 | 通过 |
| 2.1-02 | CREATE TABLE，BIGINT/VARCHAR(n)/VECTOR(n) | E6 创建含三种类型的表并经重启复验 | 通过 |
| 2.1-03 | 恰好一个 BIGINT 主键、至多一个向量列 | `types.cpp::validate_schema`；E6 拒绝无主键、双主键、字符串主键、双向量列 | 通过 |
| 2.1-04 | INSERT/SELECT/UPDATE/DELETE，多行 INSERT | E3 原公开 CRUD 与 E6 独立多行写入、更新、删除及 affected_rows | 通过 |
| 2.1-05 | 按主键或条件更新、删除 | E3/E6：id 条件及普通列条件；无匹配与匹配但值未变时影响行数亦核验 | 通过 |
| 2.1-06 | 普通投影、SELECT * | E3/E5/E6：显式列、星号、重复投影和列顺序；空集仍有列名 | 通过 |
| 2.1-07 | =、<、<=、>、>=、AND | E3 sql_comparisons；E6 组合上下界与 AND | 通过 |
| 2.1-08 | ORDER BY、LIMIT | E3/E6：升降序、限制数量、并列主键顺序；大数据 LIMIT 路径见 E10 | 通过 |
| 2.1-09 | L2_DISTANCE、精确 TopK、带过滤 TopK | E6 的独立真值与 E10；过滤集合先于排序截断 | 通过 |
| 2.1-10 | 表定义和数据持久化 | E3 多表/重启；E7 跨表故障状态；E10 超内存数据恢复 | 通过，限已测数据 |
| 2.1-11 | BEGIN/COMMIT/ROLLBACK，自读修改，普通/向量视图一致 | E6/E7 更新向量、删除和插入后在事务内查询；回滚/提交状态分别核验 | 通过 |
| 2.1-12 | 已确认提交不丢，未提交不部分生效 | E7 实际容器 KILL，不是仅终止 Docker CLI；E10 重复恢复 | 通过，限已测故障时序 |
| 2.1-13 | 不要求的语法不能被静默改义执行 | E6 明确拒绝 NULL/JOIN/聚合/子查询/ALTER/FK/嵌套事务/PK 更新，之后可继续执行 | 通过 |
| 2.1-14 | 主键唯一 | E3/E6：已有键和多行插入冲突；失败前缀不可见 | 通过 |
| 2.1-15 | VARCHAR 长度按 Unicode 码点，按码点比较，不做本地化排序 | 中+emoji 两字符、e+组合附加符两字符可写；三字符超长拒绝；é、z、é、中顺序核验。E6 | 通过 |
| 2.1-16 | 固定维度 float32；拒绝 NaN/Inf、维度错、溢出 | E3 float32；E6 拒绝非法值并接受最小正 subnormal、最大有限 float32 | 通过，极小值下溢舍入策略未由题包冻结 |
| 2.1-17 | 不得借用现成数据库/向量引擎执行核心 | CMake 核心由 engine/executor/exact_search/types/table_file/catalog/root_store 编译；Python 依赖仅 SQLGlot/pybind11，运行链接无外部数据库/向量库。E2 | 源码/依赖核对通过 |

范围说明：本轮没有把完整 MySQL 语法作为目标。例如表级 PRIMARY KEY、更多 SQL 表达式的完整语法是否属于正式冻结子集，当前公开材料不足以逐个确认；不能因为常见 MySQL 支持就擅自认定题目要求。本轮也没有恢复已撤回的 NULL 扩展。

## 2. 赛题 §2.2：示例与向量规则

| 条目 | 要求 | 核验依据与结果 | 状态 |
|---|---|---|---|
| 2.2-01 | 给出的 docs 建表与三行 INSERT 示例可执行 | E6 独立语义检查；E13 再将网站原始七条多行 SQL 原样封装为 JSON，经 stdin 执行 | 通过 |
| 2.2-02 | category 过滤、distance 别名排序、LIMIT 2 | E6 返回 id=1,2 与对应 content；独立按 float32 查询向量计算距离作校验 | 通过 |
| 2.2-03 | 示例 BEGIN/更新 embedding/删除/ROLLBACK | E6 事务内已删记录不可见，回滚后原向量查询完整恢复 | 通过 |
| 2.2-04 | 返回欧氏距离而非平方距离 | [3,4] 到 [0,0] 返回 5；E3/E6/E10 | 通过 |
| 2.2-05 | 先 WHERE 再 TopK；不足 K 全返回；空集为空 | E6 全表最近 id=9 被过滤，返回 id=2,4；另测不足 K 和空集合 | 通过 |
| 2.2-06 | 距离/普通排序并列按 PK 升序；无 ORDER 按集合 | E6 距离并列 2,4，普通 n DESC 并列亦按 PK；E10 全部 ID 按多重集合核对；原驱动无序比较保留重复行 | 通过 |
| 2.2-07 | 默认 exact，ann 显式模式；建索引不改变 exact | E6 建索引后分别发送 ann、exact 和省略 search_mode 的请求，均核对独立真值 | 通过 |
| 2.2-08 | CREATE VECTOR INDEX；建索引后新增/更新/删除可见 | E3 vector_index_maintenance；E6 事务中交错变更并在 rollback/restart 后核验 | 通过，采用 FlatScan exact fallback |

ANN 说明：现有实现 ann 走精确扫描，不是近似加速。所测小集合返回 ID、过滤、无重复、当前事务可见性和真值均一致，样本 Recall 为 1；没有据此声称正式随机 ANN 全部达标或性能达标。网页不强制 HNSW，公开题包也未要求必须近似算法；未知的正式冻结要求仍不可代猜。

## 3. 赛题 §2.3：存储与事务语义

| 条目 | 要求 | 核验依据与结果 | 状态 |
|---|---|---|---|
| 2.3-01 | 数据/恢复文件统一 /data | Docker 只读根+唯一可写 /data 下完成 E3–E10；TMPDIR=/data，存储对象/catalog/CURRENT 均在该目录 | 通过，限已执行路径 |
| 2.3-02 | 每例独立目录；本例重启复用 | 原驱动每例新卷，restart 复用；独立检查亦每组新卷、多次同卷启动 | 通过 |
| 2.3-03 | 数据超过内存不能整体常驻 | E10 的有效载荷 2,214,592,512 字节，大于 2,147,483,648 字节内存上限；真实文件 2,214,679,401 字节，查询/恢复通过 | 通过该实际配置；不覆盖所有正式规模 |
| 2.3-04 | 未 BEGIN 时自动提交 | E3/E7 自动提交响应后强杀，重启保留；E10 每批写入后确认 | 通过 |
| 2.3-05 | 事务内普通查询/向量读到自己的成功修改 | E6 插入、更新向量、删除后的标量与 ann 结果一致 | 通过 |
| 2.3-06 | 失败语句原子；不抹掉事务之前的成功修改 | E6 前一成功 INSERT 后多行冲突，失败前缀不出现，原成功记录仍可 COMMIT | 通过 |
| 2.3-07 | ROLLBACK 恢复记录内容、集合及向量视图 | E6/E7，含索引存在时的回滚 | 通过 |
| 2.3-08 | 发成功响应前完成持久化；已确认修改重启不丢 | root_store::publish 对对象、目录、catalog、CURRENT 执行 fsync/原子替换后返回；E7/E10 以实际 KILL 检查 | 源码与已测故障验证通过 |
| 2.3-09 | 索引可重建，但恢复完成前不得 ready，成本受限 | Engine 构造先 recover 再 emit ready；E10 在恢复读表期间观察到未 ready，KILL 后再恢复。持久化 FlatScan 描述，不存在后台未就绪索引 | 通过本轮数据；未知正式恢复时限/更大规模未验证 |

## 4. 赛题 §3.1：统一运行环境与通信流程

| 条目 | 要求 | 核验依据与结果 | 状态 |
|---|---|---|---|
| 3.1-01 | Linux x86_64 容器；平台从源码构建镜像 | 干净 HTTPS 克隆→Docker build --no-cache；镜像 inspect linux/amd64；本地构建不等同平台已构建 | 本地等价构建通过；平台锁定待确认 |
| 3.1-02 | 运行禁止联网，依赖构建阶段完成 | 所有验收容器 --network=none；运行时无 apt/pip/下载动作，native/SQLGlot 可导入 | 通过 |
| 3.1-03 | 依赖版本锁定 | Python 3.12.10、SQLGlot 30.31.0、pybind11 3.1.0 已固定；基础镜像 digest/apt 版本未精确固定 | 部分，不是完全版本锁定 |
| 3.1-04 | 前台常驻、stdin 接收、stdout 返回，同进程连续语句/事务 | 默认 ENTRYPOINT，不带私有参数；E5/E6 连续交互，stdin 保持打开；不使用 -d/-t | 通过 |
| 3.1-05 | 不启动网络监听服务 | 主体只读 stdin/写 stdout，native 在同进程；无 EXPOSE/端口映射，禁网下验收完成 | 源码/启动配置核对通过 |
| 3.1-06 | 语句边界/格式/错误/ready/模式/退出遵循目标分支驱动 | E3–E7 与 §7 逐条检查，原驱动明确把 SQL 包在 JSON 请求内 | 按当前题包通过 |
| 3.1-07 | 网页“不再采用此前示例中的 JSON 封装” | “此前示例”指代不明确；网页同时指定遵循目标分支题包。该分支 STDIO_PROTOCOL.md、README、Session.request/read 明确为 JSONL，本实现按它们对接 | 按指定分支通过；不据此另行要求裸 SQL |
| 3.1-08 | 日志 stderr，不混 stdout；及时 flush | E5 实时读取，不预发请求；E8 强制启动失败仅 stderr；全部协议报文均换行、严格 JSON，无附加输出 | 通过 |
| 3.1-09 | 正常回滚与必要落盘；异常复用 /data | E7 normal shutdown rollback、KILL 后提交保留；E10 重复恢复 | 通过已测序列 |
| 3.1-10 | 冻结资源/规模/超时；预检正式同协议 | 已发布 config 原样用于 E3：120s/60s/600s、2 CPU/2 GiB/128 PID、1e-5 容差、16 MiB 响应上限；磁盘容量/I/O 带宽、全部正式规模及平台预检/正式一致性无法由公开包确认 | 公开配置通过，其余未验证 |

“SQL 经 stdin/stdout”与“SQL 被 JSON 封装”并不互斥。当前实际链路是：

```text
.test 中的 SQL
  → 原驱动包装 {id,op:"execute",sql,search_mode}
  → docker run -i 的 stdin（无 TTY）
  → PID 1 Python 解 JSON、SQLGlot 降 IR、自写 C++ 执行
  → 一行 JSON 写 stdout 并 flush
  → 原驱动解析、校验 id/ok、比较 columns/rows/affected_rows
stderr → 独立日志文件，不参与结果解析
```

不能因网页出现“SQL”就推断必须裸 SQL；也不能因公开包是 JSON 就推断正式平台绝无版本不一致。

## 5. 赛题 §3.2：功能准入测试

| 条目 | 要求 | 本轮检查 | 状态 |
|---|---|---|---|
| 3.2-01 | mysqltest 风格脚本+断言，不是 TCP 连接 MySQL | 从干净克隆运行原 run.py --image，未连接端口、未用 mysql 客户端 | 通过 |
| 3.2-02 | 读取实际发布用例，不替换同名测试 | E3 原 manifest/test/result 全部未修改；20 个用例、95 SQL 全部通过 | 通过公开包 |
| 3.2-03 | 全部正式 29 例，每例全部断言通过，否则 0 分 | 未持有另外 9 例输入及平台报告；公开通过不能替代 | 未验证，不能宣布准入通过 |
| 3.2-04 | 确定性 SQL/存储结果、错误、数量与重启状态 | E3/E6/E7；独立期望值由检查脚本定义，只在宿主测试端比较，镜像不读取答案 | 通过已测序列 |
| 3.2-05 | 过滤集合精确真值、距离、TopK/主键/顺序 | E6 独立计算距离，故意放置过滤外最近记录及并列值；E10 小结果 TopK | 通过样本 |
| 3.2-06 | ANN ID 有效、无重复、过滤、当前可见、Recall | E3/E6/E10 样本精确匹配真值；含事务与重启，空集合为空 | 通过样本；未跑正式随机集 |
| 3.2-07 | 交错建索引、插入、向量更新、删除 | E3/E6，包含 rollback/restart 后状态 | 通过 |
| 3.2-08 | 提交/回滚/语句失败参考状态，普通与向量一致 | E6/E7 跨表、重复键失败、后续 commit/rollback | 通过已测序列 |
| 3.2-09 | 强杀后复用 /data，恢复中再强杀，重复恢复 | E10 观察 /proc/1/fd 已打开 /data/objects/*.tbl，尚未输出 ready；实际 docker kill KILL，再次 ready 后核对全部 ID、内容样本、向量 TopK | 通过该实际故障序列 |
| 3.2-10 | CPU/内存/磁盘/时间与大于内存数据 | E10 实际 cgroup 2 CPU/2 GiB/无 swap/128 PID、2112 MiB 有效负载，102.89s 完成，无 OOM；磁盘/带宽正式硬限额未发布于 config | CPU/内存/自定数据已测；正式磁盘/规模组合未验证 |
| 3.2-11 | 已 COMMIT 成功保留，未发 COMMIT 回滚 | E7 两张表已确认提交一致保留；未发提交时强杀，二者均回到旧状态 | 通过 |
| 3.2-12 | 发 COMMIT 未收到成功时全生效或全不生效，不能部分 | E7 发送 COMMIT 后不读取响应就 KILL，恢复二表均为 50、向量距离 5；检查允许完整旧/新状态 | 通过此次客户端未确认序列；未穷举内部发布切点 |
| 3.2-13 | 自动提交适用同样确认规则 | E7 未读取 UPDATE 响应便 KILL，标量/向量均完整生效；已读取自动提交响应再 KILL 则必须保留 | 通过此次序列 |
| 3.2-14 | Recall 分母 min(K,N)，空集不参与均值，距离 ties 按 PK | E6 不足 K 全返/空集空返/并列 PK；本轮没有计算正式整组 Recall 平均值 | 样本规则通过；正式聚合未验证 |

本轮只模拟**公开可得的**测试机流程：真实 Docker、原驱动、原配置、原输入输出通道。不能模拟未知隐藏输入、平台锁定步骤、平台未公开资源参数或正式回执，不能生成正式通过结果或分数。

## 6. 赛题 §4.2.1：Dockerfile 与提交

| 条目 | 要求 | 核验依据与结果 | 状态 |
|---|---|---|---|
| 4.2.1-01 | Dockerfile 由提交源码构建 | E1/E2，从匿名 HTTPS 干净克隆构建；C++ 编译在 builder 阶段 | 通过 |
| 4.2.1-02 | 默认入口启动 stdin/stdout 前台模式 | exec ENTRYPOINT 为 /opt/venv/bin/python -m minivectordb，Cmd 为空；原 --image 不补参 | 通过 |
| 4.2.1-03 | 构建镜像后可直接 run | E2/E3，无缓存 build 后原握手启动；不依赖宿主环境 | 通过 |
| 4.2.1-04 | 保持 stdin，用 -i，不用 -d/-t | E3 原驱动 argv 与 E5 inspect 的 OpenStdin=true/Tty=false | 通过 |
| 4.2.1-05 | 管道接 stdin/stdout；stderr 单收 | E3 原驱动；E4 原样文档命令；E5 独立实时管道 | 通过 |
| 4.2.1-06 | 不需要 EXPOSE、端口映射、host/port 参数 | Dockerfile 无 EXPOSE；inspect 无端口配置；默认入口无 host/port，禁网通过 | 通过 |
| 4.2.1-07 | 数据在 /data，限额以题包为准 | 原 --image 自动挂卷、只读根；E10 实际 cgroup 值与 config 对应 | 通过公开配置 |
| 4.2.1-08 | 不依赖未交付的本地二进制，可多阶段构建 | Dockerfile 先 COPY C++ 源码，CMake 编译，再 COPY native 至 runtime；没有 COPY host build | 通过 |
| 4.2.1-09 | Linux x86_64，所需运行库在镜像内 | image linux/amd64；native/SQLGlot import 成功，ldd 无 not found | 通过 |
| 4.2.1-10 | 基础镜像及依赖版本按要求锁定 | Python tag 与两个 Python 依赖版本固定；缺 image digest 和精确 apt 版本 | 部分 |
| 4.2.1-11 | Ubuntu 24.04 示例 | 正文说“下面以…为基础镜像”，是例子，并无明确强制 Ubuntu 的条款；当前采用 Python slim，不能仅因非 Ubuntu 判不合规 | 未发现强制 Ubuntu 要求 |
| 4.2.1-12 | 逐字核对示例 Dockerfile/build/run 代码块 | E14 已从公开正文 API 解码三个原块；E12 复跑原前台启动方式并检查五行交互 | 通过本轮逐项比对与等价启动 |

### 原示例每条指令与当前文件的对应关系

| 示例指令 | 当前提交 | 判断 |
|---|---|---|
| FROM ubuntu:24.04 | 两阶段均为 python:3.12.10-slim | 正文说“下面以…为基础镜像”，没有写只允许 Ubuntu；实际 linux/amd64 及运行库检查通过 |
| WORKDIR /app | 两阶段均 WORKDIR /app | 对应 |
| COPY --chmod=755 minivectordb /app/minivectordb | builder COPY C++ 源码并编译，runtime COPY native 扩展、Python 程序与 venv | 按正文要求补上源码多阶段构建，不依赖本地未交付的可执行文件；不是照抄仅封装二进制的示例 |
| RUN mkdir -p /data | runtime RUN mkdir /data | 目录在构建阶段存在，实际挂卷可用 |
| VOLUME ["/data"] | 未声明 VOLUME；原驱动及示例运行时显式 mount/-v /data | VOLUME 是示例的镜像声明，不是挂载的唯一方式。E3/E12 均已实际提供可写卷；不能因此宣称不挂卷的 readonly 启动也可用，E8 正好证明后者会失败 |
| ENTRYPOINT ["/app/minivectordb"] | exec-form /opt/venv/bin/python -m minivectordb | 程序名允许按实现调整；实际为默认前台数据库入口 |
| CMD ["--data-dir", "/data"] | 无 CMD；程序 --data-dir 默认即 /data | 不需判题器补私参，默认数据路径相同；原 --image 成功 |
| docker build -t minivectordb:latest . | 干净克隆目录构建，使用唯一 tag 并加 --no-cache | 构建入口相同，避免覆盖用户既有 tag |
| docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest | E12 同组运行标志，仅换唯一卷名/镜像并增 --name 便于清理 | 实际五行 ready/create/insert/select/shutdown 均正确，exit 0、stderr 为空；不使用 TTY/后台/端口/私有程序参数 |

### 必须分开的交付检查

| 项目 | 结果 |
|---|---|
| 规定分支和 HTTPS 仓库是否可获取 | 通过，重新克隆并确认完整 SHA |
| 根 Dockerfile 和完整实现是否在同一提交 | 通过 |
| 同一提交的 Agent 材料 | **不满足**：agent/conversation.json 不存在，根文件 0 字节非法 JSON |
| 本地源码是否等于此次构建来源 | 干净远端克隆，无本地补丁参与 |
| 平台本次是否真的锁定 c4a54eb… | **未验证**，没有平台锁定记录；推送本身不会更新已锁定任务 |
| 平台构建镜像是否等于本地镜像 ID | **未验证**；本地构建成功不是平台构建回执 |
| Agent 缺失是否导致 MySQL 尚未执行 | **未知**，不能用评分说明代替平台入口行为，也不能据此认定因果 |

## 7. STDIO_PROTOCOL.md 逐条检查

### §1：基本通信规则（七条逐条列出）

| 条目 | 原要求 | 实测/源码证据 | 状态 |
|---|---|---|---|
| P1-01 | UTF-8；每报文完整 JSON 对象，实际换行结尾 | E5 严格 UTF-8 解码/对象解析；E4 最后一条也以 LF 结束 | 通过 |
| P1-02 | 一个物理行；字符串换行转义 | E5 字符串含实际换行、分号、中/emoji/双引号，经请求/响应正确恢复；raw stdout 每条单行 | 通过 |
| P1-03 | 每次立即 flush，不等下一条或退出 | E5 保持 stdin 打开，先等 ready，再逐次写入/等待响应；__main__.py::emit 显式 flush | 通过 |
| P1-04 | stdout 只协议，日志/堆栈 stderr | 正向三组 stderr 均为 0 字节；E8 存储启动错误仅 stderr，stdout 0 字节且不 ready | 通过 |
| P1-05 | 每请求一个响应，回传 id 及 JSON 类型 | E5 数字、大整数、负数、字符串、bool、float、null/数组/对象 ID 均保真；静默窗口无重复响应。ID null 不是 SQL NULL | 通过 |
| P1-06 | 无空行、提示符、围栏、颜色、额外状态 | E4 精确 5 行；E5 每行非空严格 JSON、无 ANSI，无额外推送 | 通过 |
| P1-07 | 无 NaN/Infinity、无重复键 | emit allow_nan=False；E5 解码器逐条拒绝重复键/非有限输出，所收均合法 | 通过 |
| P1-08 | 一发一等，不推心跳/进度；-i 无 -t、不用网络协议 | E3/E5；ready 等待和逐请求静默窗口；无网络 SQL 接口参与 | 通过 |

### §2：主动启动握手

| 条目 | 原要求 | 实测/源码证据 | 状态 |
|---|---|---|---|
| P2-01 | 打开 /data、完成恢复之后再 ready | Engine 构造先 recover；E10 恢复读表期间无 ready，结束后才产生 ready | 通过 |
| P2-02 | 等待第一条请求前主动输出并 flush | E5 的 first stdout 时间早于 first stdin，未预写任何 SQL；E3 handshake 工具亦只先等 ready | 通过 |
| P2-03 | 准确对象 {"type":"ready"}，不能加字段或输出 READY 等替代 | E3/E4/E5 每次精确对象比较 | 通过 |
| P2-04 | 每次重启/恢复均输出一次 | E5 两次、E6 两次、E7 七次正常就绪；E10 被恢复中 KILL 的那次不 ready，其余各一次 | 通过 |

### §3：execute 与结果

| 条目 | 原要求 | 实测/源码证据 | 状态 |
|---|---|---|---|
| P3-01 | 先解整行 JSON，再执行 sql，不按分号切原始输入 | E5 含分号/换行字符串正确；半行输入未响应，补 LF 后才执行 | 通过 |
| P3-02 | op=execute，sql 为字符串；exact/ann 参数 | __main__::execute_request 校验；E6 显式与缺省模式 | 通过 |
| P3-03 | 成功查询含 id、布尔 ok、对应 columns/rows | E3–E6；严格类型与列/行对应比较 | 通过 |
| P3-04 | 空集仍返回列名与 [] | E5 SELECT s WHERE id=9，columns=[s], rows=[] | 通过 |
| P3-05 | INSERT/UPDATE/DELETE 返回整数 affected_rows | E3/E5/E6 多行、无匹配、值不变更新、删除 | 通过 |
| P3-06 | DDL/事务 {} 断言仍须 id、ok:true | E4 建表响应与 E6 事务响应均含 id、ok | 通过 |
| P3-07 | 失败 ok:false、非空字符串 error.code；message 不要求固定 | E3/E5/E6；原 config error_codes={}，未冒称完整隐藏映射固定 | 通过当前约定 |
| P3-08 | 预期 SQL 错误后进程继续，事务不错误清空 | E5 duplicate 后正常查询；E6 事务失败后保留前成功记录并 COMMIT | 通过 |
| P3-09 | BIGINT JSON 整数，不能转字符串/经浮点丢精度 | E6 -2^63、2^63-1、9007199254740993 精确类型和值比较 | 通过 |
| P3-10 | 行为二维数组；向量数组、有限浮点、列顺序正确 | E3 原向量投影与 float32；E5/E6 严格 rows/columns；输出非有限检测 | 通过 |
| P3-11 | SQL NULL 表示 JSON null | 题目明确不要求 SQL NULL，当前 SQL NULL 返回 unsupported_sql，不伪造 null 结果；JSON null ID 单独保真 | SQL NULL 功能不适用，并非擅自新增 |

### §4–§6：关闭、完整示例、题包驱动指令

| 条目 | 原要求 | 实测/源码证据 | 状态 |
|---|---|---|---|
| P4-01 | shutdown 回滚未提交、必要清理落盘后响应 | E7 正常关闭后标量/向量回到已提交状态；engine.close 在 emit 之前 | 通过 |
| P4-02 | 响应精确 id+ok:true，flush 后退出 0 | E3/E4/E5 exact 比较并 wait exit=0 | 通过 |
| P4-03 | 关闭响应后不再输出，包括空行/Bye | E3 原工具检查剩余 buffer/stdout；E5 EOF 后剩余字节为 0 | 通过 |
| P4-04 | 同目录重启不清空；已确认提交不能依赖 shutdown | E7/E10 不发 shutdown 而真实 KILL，恢复保留 | 通过 |
| P5-01 | 完整例子：ready→建表→插入你好→查询→shutdown，共五行 | E4 原样执行 §7 Bash 中的对应例子，五行解析和实际值均通过 | 通过；主动时序另由 E5 验证 |
| P6-01 | .test/.result 不直接作为通信报文 | 原 run.py 包装 SQL 并用 .result 断言字段；E3 实际运行 | 通过 |
| P6-02 | --restart 是驱动关闭+同目录启动+等 ready，不是 SQL | 原 check_case 分支处理；E3 重启相关样例通过 | 通过 |
| P6-03 | --mode ann 改后续模式，本身不占 SQL 响应 | 原 check_case 仅改 mode；E3 index_maintenance SQL/断言计数一致 | 通过 |
| P6-04 | 每例空目录，本例重启同目录 | 原 Session 每例新卷、同 Session 重启不换卷；E3/E7 | 通过 |

### §7–§9：文档自测、故障定位与交互工具

| 条目 | 原要求 | 本轮动作 | 状态 |
|---|---|---|---|
| P7-01 | Dockerfile 默认 stdin/stdout 入口，不靠私有参数 | E2/E3 默认入口直接运行 | 通过 |
| P7-02 | §7 Bash 的限额、bind /data、stdout/stderr 分离 | 从干净克隆抽取该代码块原样执行，仅设置镜像/TMPDIR 环境；E4 | 通过 |
| P7-03 | 最后 LF、5 行、精确 ready、typed id、affected_rows、你好查询 | 原文内 Python 断言全部通过；原始 stdout 保留 | 通过 |
| P7-04 | 批量预输入不能证明 ready 主动或逐请求 flush | 本报告没有用 E4 证明时序；E3/E5 另行按一发一等验证 | 已遵守限制 |
| P7-05 | 180s 是示例总超时，不冒称正式全局时限 | 原样 timeout 180；正式公开驱动仍用原 config 120/60/600 | 已遵守 |
| P8-01 | 排查 invalid JSON、expected ready、timeout、提前退出 | E3/E5 未出现；E8 人为缺可写 /data 时确认只写 stderr、不假 ready | 相关检测通过，不伪造正式失败日志 |
| P8-02 | 排查 id、bool ok、重复响应、shutdown 尾部输出 | E3/E5 逐请求检测而非只看总 exit；类型、静默和尾部字节均核验 | 通过 |
| P8-03 | 不能只改 READY 而继续读裸 SQL | 本实现完整解 execute/shutdown；E5 故意送裸 SQL 得 invalid_request，再发 JSON 仍可继续 | 按当前包通过；裸 SQL 不是该驱动的合法输入 |
| P9-01 | 新交互工具先等 ready，覆盖 flush/超时/重启/模式 | 从干净克隆运行原工具，不用替代驱动 | 通过 |
| P9-02 | 握手→sql_crud→完整公开包 | E3 严格按此顺序分别运行，新输出目录，exit 0、passed=true | 通过 |
| P9-03 | 不含隐藏测试、不计分、不发提测 | 本轮没有调用平台回执/计分 API，没有生成分数 | 已遵守 |
| P9-04 | example_stdio.py 是骨架，不能代数据库提交 | Dockerfile 实际入口为 minivectordb，自写 native Engine；不是 example_stdio.py | 通过 |

## 8. protocol_check/README.md 逐项检查

| 条目 | 文档要求/说明 | 核验结果 | 状态 |
|---|---|---|---|
| R-01 | Python 3.9+、驱动无需 pip 依赖、Docker 权限 | 用系统 python3 运行克隆内原驱动；当前用户 howxu 可直接调用 Docker | 通过 |
| R-02 | 第一步先等主动 ready，再 shutdown，不先发 SQL 诱导 | E3 handshake passed；E5 first output < first input | 通过 |
| R-03 | 解析错误显示前 240 字节、stderr 单收 | `_transport.read` 保留错误行片段，Popen stderr=self.log；未改原工具 | 源码核对通过，此次无解析错误 |
| R-04 | 第二步运行 sql_crud | E3 单独结果 1/1 PASS | 通过 |
| R-05 | 第三步读当前 manifest/test/result 跑全部 | E3 20/20 PASS、95 SQL，非内部同名替代 | 通过公开范围 |
| R-06 | 每例独立卷，restart 复用，mode ann 改后续模式 | 原 Session/check_case 源码与重启/索引样例 | 通过 |
| R-07 | 首次 timeout 停后续并标 SKIPPED，清容器、留报告 | 原 run.py timeout 分支设置 aborted，后续 SKIPPED；finally close 清卷并写报告。本轮未人为等待 120/600s 触发此负向路径 | 工具源码核对；非数据库额外义务 |
| R-08 | 输出目录须新建，passed=true 与 exit0 才通过 | E3 三个独立新目录，各 exit0 与 passed=true；无 TIMEOUT/SKIPPED | 通过 |
| R-09 | 原 config 120/60/600，2 CPU/2 GiB/128 PID，浮点 1e-5 | E3 未使用 --config；E10 实测 cgroup 上限与公开值一致 | 通过 |
| R-10 | 放宽本地配置不能替代正式要求 | 未放宽原工具参数；256 MiB 另列为诊断，E10 使用公开内存限额 | 已遵守 |
| R-11 | --command 仅可信本地开发，必须再测实际 Docker | 本轮全部验收使用 --image 或同等 docker run -i 原始管道；无 --command | 通过 |
| R-12 | 默认启动参数、依赖、只读根实际检查 | E2/E3/E5/E8；无入口覆写参与 SQL 验收 | 通过 |
| R-13 | {} 成功；error 失败且非空 code | E3 原断言；E4 建表输出非 {}；E6 错误后继续 | 通过 |
| R-14 | BIGINT 精度、列顺序、行数/重复行、float32 | E3/E6；原断言无序按多重集合，不对生产输出去重 | 通过已测数据 |
| R-15 | 不自动把 READY/裸 SQL 转 JSON | 原工具无协议转换；本实现主动发正确 JSONL | 通过当前约定 |
| R-16 | 握手/公开通过不等于正式29例、性能或 Agent | 本报告不宣称正式通过或获得分数 | 已遵守 |

## 9. 超内存与重复恢复的本轮实测明细

### 9.1 正式公开资源标志、独立自造数据

```text
--network=none --cpus=2 --memory=2g --memory-swap=2g --pids-limit=128
--read-only --cap-drop=ALL --security-opt=no-new-privileges
--mount type=volume,...,dst=/data
```

实际读 cgroup v2：cpu.max=`200000 100000`、memory.max=`2147483648`、memory.swap.max=`0`、pids.max=`128`。

- 33 批，每批 64 行；每行 1 MiB VARCHAR，并带 BIGINT/二维向量。共 2,112 行，有效载荷 2,112 MiB，实际文件 2,214,679,401 字节。
- 所有数据通过执行 SQL 写入并确认，不向 /data 注入预制文件。
- 核对全部主键多重集合、内容样本、带过滤的向量 TopK、PK 并列规则及有限结果。
- 确认写入后 KILL，第一次重启 ready 为 1,369ms；再次启动过程中观察到表文件已打开、stdout 尚未 ready，KILL；第三次成功恢复 ready 为 1,253ms，结果再核对。
- 全过程 102.89s；内存峰值达到 2 GiB 上限，memory.events 的 oom/oom_kill 均为 0。这里并非“内存远低于限制”。
- 各次启动采样的单设备累计 wbytes 为 37,661,130,752（约 35.08 GiB），在诊断设定的 48 GiB 预算内。不是正式性能 B：没有统一冷缓存、专用设备和正式采集窗口；临时只读检查进程的资源也包含在容器计数中。
- 容器和卷已清理。脚本 `resource_probe.py --published-memory`、计数和 trace 均保留。

### 9.2 不能隐去的诊断修正与实现风险

初轮“2 GiB + 2,112 MiB”曾用 `SELECT id ... ORDER BY id` 无 LIMIT 来核对全部主键。现实现的外排会携带整行的大 VARCHAR，即使只投影 id，也会产生较高的临时写入。

原诊断脚本只在 INSERT 批次后检查写预算，遗漏之后查询的外排写入；复核发现首次强杀前单设备 wbytes 已到 66,137,538,560，不能记为“48 GiB 预算内通过”。该轮保留在 `resource-initial-with-unbounded-order/`，没有删除或伪装。

随后仅调整**宿主验收脚本**：全部主键用无 ORDER 的多重集合校验，避免校验自身引入不必要的大字段外排；增加查询后和跨启动的累计预算检查。数据库源码/镜像保持不变，重新写入和恢复测试，得到上面的 102.89s 结果。原轮无界排序功能结果正确，但显示出真实 I/O 代价；这不是可以忽略的资源风险，也未证明其触发了平台此次 0 分。

因此：本轮已正面证明一份超过公开内存限额的数据可以通过标准输入输出写入、查询和恢复；**不能据此宣称所有大字段排序、小批持续写入、正式磁盘/带宽上限与所有隐藏资源场景都通过**。

## 10. 对“依旧 0 分”的流程判断与下一步定位

### 已实际排查、未复现失败的流程点

- 远端规定分支缺源码/Dockerfile、提交无法获取、本地文件未交付。
- Docker 不能从源码构建、缺 native 运行库、默认入口不是数据库。
- 误用网络服务、后台/TTY 模式、依赖私有启动参数。
- 等收到 SQL 才 ready、ready 字段不对、ready 无 LF/未 flush。
- 按裸 SQL 读取 JSON、按分号切通信输入、stdout 混日志。
- 数字 id 转字符串、ok 非 bool、错误后退出、多发响应。
- shutdown 无响应/退出非零/多余 stdout、重启清空 /data。
- 仅本地 native 测试通过而未走实际 Docker/原 --image 路径。

### 仍未被排除的流程问题

1. **平台锁定了旧 SHA 或别的提交。**本地远端 c4a54eb…存在，不证明失败任务采用它。
2. **平台评测版本尚未取得。**公开分支的 JSONL 契约已明确且本机复核通过；仅凭“此前示例”不足以支持平台发送裸 SQL 的假设。这里只保留一般性的平台版本核对，不将其列为已发现的协议不合规项。
3. **平台实际启动挂载、用户权限、限额或构建依赖环境不同。**本报告模拟的是已发布命令与配置，不能代替平台实际 argv/构建日志。
4. **完整性预检先检查 Agent 文件而拦截。**文件缺口确实存在；该入口是否如此实现未知，不能把猜测当根因。
5. **正式隐藏输入或资源约束失败。**本轮超内存/重复恢复通过也不能排除其他合法输入、外排 I/O 或持续小批写入代价。

### 应核对的最小平台信息，不索要隐藏 SQL/答案

```text
任务实际锁定的仓库 URL、分支、完整 SHA、构建镜像摘要；
失败阶段：获取源码 / 构建 / 容器启动 / 等 ready / 第一条请求 / 某功能用例 / 资源；
实际容器启动 argv 和 /data 挂载方式；
实际使用的协议/驱动版本，是否与公开 f4d4e266… 版本一致；
若在通信阶段失败：首个 stdout 原始行片段及 stderr 错误；
若已进入正式用例：失败类别即可，不需要提供隐藏输入或参考答案。
```

这不是将本轮检查推给用户：上述公开可模拟的构建、启动、通道、握手、公开驱动、事务故障及超内存流程已执行完成。剩下的是公开材料无法观察的平台边界；在取得边界证据前，再改 READY/JSON/SQL 语法或更换基础镜像，只是在替换未经证实的假设。
