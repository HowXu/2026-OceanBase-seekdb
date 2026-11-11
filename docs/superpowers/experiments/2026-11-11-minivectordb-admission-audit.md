# MiniVectorDB：准入流程、Dockerfile 与赛题要求核验

> 本文主体是 Docker 安装前、SQL 修复前的审计快照。后续已安装实际 Docker，并修复本文记录的重复投影排序问题；无缓存 Docker 构建、原 `--image` 握手/20 个公开用例和容器强杀恢复已通过。后续实测范围见 [SQL 修复与 Docker 验证记录](2026-11-11-minivectordb-sql-order-docker.md)。下文“缺 docker”和“未修复”均保留为当时的事实，不代表当前状态；正式失败根因、隐藏用例和超内存资源准入仍未确认。

## 核验对象和结论范围

源码提交：`3e6792da48a533ee09160c8f9677ab02bb42ca09`。

本轮先按用户要求撤除未提交的 NULL 扩展，将源码与原有测试恢复到该提交。随后按用户明确授权，将 `kernel/round2` 快进合入 `2026-competition-round2` 并推送 `origin`；没有新造合并提交，没有强推。远端目标分支已确认指向上述 SHA。

没有正式失败报告，因此本记录**不认定正式失败的根因，也不宣称正式准入通过**。这里只区分已经实测的事实、可复现缺陷、未验证风险与交付问题；未执行大规模压力测试或性能排名测量。

## 一、依据与版本

- 网页：`chores/加赛赛题一：完成一个轻量向量数据库.html`，以及同名 `_files` 附件目录。
- 网页正文已完整抽取至 `.superpowers/sdd/admission-audit/requirements.txt`，包括 2.1—2.4、3.1—3.5、4.1—4.4。
- 原保存 HTML 中，4.2.1 的三个代码块只有空的 `ne-card` 容器，未保存示例代码正文。目录内也未找到其 Dockerfile/启动命令原文。因此能核验该节完整文字要求，不能假称逐字比对了缺失的代码块。
- 官方远端 `remote/2026-competition-round2` 经 `git ls-remote` 核对，仍为 `f4d4e26653af823a7601668802f80e374ad6643c`。本地 STDIO_PROTOCOL.md、protocol_check、mysqltest 相对该官方版本无差异。
- 当前公开题包是 `minivectordb-public-v4`：20 个公开样例、95 条 SQL；README 声明正式为 29 个用例，另 9 个具体输入未公开。

## 二、判题流程：逐段检查

### 1. 平台取得分支和锁定提交

核验前：

- `origin` 只有实现分支 `kernel/round2`，没有 `2026-competition-round2`。
- 本地 `2026-competition-round2` 停在官方题包基线；该基线的 Dockerfile 为 0 字节，没有 minivectordb 实现。

这是实际存在的交付缺口。如果此前平台取的是该目标分支/基线，就不能构建当前数据库。但缺少提交页面实际锁定的分支/SHA，不能认定这就是此前失败原因。

用户授权后已修正：`origin/2026-competition-round2` → `3e6792da48a533ee09160c8f9677ab02bb42ca09`。平台若仍使用已锁定的旧提交，不会因推送而自动替换，后续提测必须核对其锁定版本。

### 2. 仅从交付源码构建

使用 `git archive HEAD` 将该提交的 131 个已跟踪文件导出到独立临时目录，目录中没有宿主 `.venv`、build、未跟踪文件或预编译扩展。对该目录实际执行：

```text
podman build --no-cache -t localhost/minivectordb:admission-clean -f <归档目录>/Dockerfile <归档目录>
```

构建成功。实际 C++ 编译及依赖安装都在构建阶段发生，不依赖宿主产物。

镜像：`051fa8a0526323df6a04ba231b2772bc4d2dce76726da34da3fc4eadd4f48328`，inspect 为 `linux/amd64`。

### 3. 默认入口、启动与协议

镜像默认 Entrypoint 为 `/opt/venv/bin/python -m minivectordb`，Cmd 为空，不需要评测方另传私有参数。程序的默认数据目录为 `/data`。

网页 3.1 的“不再采用此前示例中的 JSON 请求／响应封装”与当前题包有冲突；但该段同时明确协议以目标分支题包为准。实际官方材料证据：

- `STDIO_PROTOCOL.md` 明确一行 JSON 请求/响应、主动 `{"type":"ready"}`、execute/shutdown。
- `protocol_check/_transport.py:123-136` 实际发送含 id/op/sql/search_mode 的 JSON 请求并校验 JSON 响应。
- `protocol_check/README.md` 明确不会将纯 SQL/READY 自动转换为 JSON。

因此当前公开协议核验应继续使用 JSONL；没有证据支持改成裸 SQL。若正式平台实际采用另一协议，则属于尚未核实的平台/题包不一致，不能由我们猜测报文格式。

### 4. 标准驱动的容器与生命周期

原 `--image` 分支会启动 Docker 容器，禁网、2 CPU、2 GiB、禁 swap、128 PID、cap-drop、no-new-privileges、只读根，并将独立数据卷挂载到 `/data`；驱动不额外传程序启动参数。重启复用该卷，强杀通过容器运行时向实际容器发送 KILL。

补充了与此对应的 **Podman** 实测，未伪装为 Docker：

- 使用上述纯源码镜像的默认入口，无 entrypoint 覆盖、无额外程序参数。
- 相同资源与安全限制、只读根、`/data` 独立 named volume。
- Podman 额外使用 `--read-only-tmpfs=false`，避免 Podman 自动创建 Docker 命令没有声明的可写 tmpfs。
- 连续 SQL、事务与精确/ann 查询正常；NULL 明确返回 unsupported_sql 后仍可继续执行。
- 实际 `podman kill --signal=KILL` 容器，而非只杀 CLI。
- 未提交 UPDATE 强杀后回滚；已收到成功的 COMMIT 强杀后保留。
- 同卷重启后标量和向量查询一致；shutdown 时回滚未提交修改，退出码为 0，之后没有多余 stdout。
- 所有新增容器和数据卷均清理完成。

### 5. 实际 Docker 的明确限制

本机没有 `docker` 可执行文件。实际运行原命令 `protocol_check/run.py --image ... --handshake-only`，在创建 Docker 数据卷前即失败，报告为 `[Errno 2] No such file or directory: 'docker'`。

这属于本机验证条件缺失，不是已复现数据库启动失败；同样不能把 Podman 成功换名为 Docker 成功。实际 Docker `--image` 全流程仍未验证。

## 三、4.2.1 Dockerfile 要求对照

| 要求 | 当前状态与证据 |
|---|---|
| 从提交源码构建，不依赖本地预编译二进制 | 满足：多阶段构建，COPY C++ 源码后 CMake 编译；纯 git 归档、无缓存构建通过 |
| Linux x86_64 | 实测镜像 linux/amd64 |
| 容器直接前台启动程序 | exec-form ENTRYPOINT，无 shell 后台运行、无 TTY、无需启动命令参数 |
| stdin/stdout 接入，不需网络服务 | 原驱动握手和连续请求通过；Dockerfile 无 EXPOSE/端口依赖 |
| 运行阶段禁止联网、依赖预装 | 禁网运行通过；Python 包与 native 扩展/运行库在镜像内 |
| 所需动态运行库齐全 | 对最终镜像扩展执行 ldd，无 not found；sqlglot 和 native 模块导入成功 |
| 持久化文件写 /data | 默认路径 /data；只读根、named volume、正常重启和强杀恢复通过 |
| 基础镜像及依赖按题包版本要求锁定 | Python 镜像标签为 3.12.10，sqlglot 30.31.0、pybind11 3.1.0 精确锁定；OS 层 apt 包没有精确版本、基础镜像未用摘要固定，构建可复现性仍可加强。当前题包未见强制 Ubuntu 24.04 条款 |

“下面以 Ubuntu 24.04 为基础镜像”是该节示例文字，不能单凭此句推导所有参赛程序必须用 Ubuntu。当前 python:3.12.10-slim 不是已证明的违规或失败原因。

仓库没有 `.dockerignore`。这会让本地构建上下文包含不必要文件；不是已经证明的数据库功能缺陷。本次纯源码归档构建排除了宿主文件依赖，但没有替平台确认其 clone/context 大小与构建时限。

## 四、赛题要求的实现与缺口

### 有实现和小型验证证据的部分

- 标量 CRUD、五种比较、AND、普通排序/LIMIT、BIGINT 精度、主键重复和不可更新。
- Unicode 码点计长/排序；不进行语言排序或规范化。
- 固定维度有限 float32；NaN、Infinity、维度不符及转换溢出拒绝；距离使用转换后向量。
- 过滤前置 TopK、欧氏距离、距离/普通排序并列时 PK 升序。
- 自动提交、显式事务 read-own-write、失败语句原子性、此前成功语句可继续提交/回滚。
- 正常重启、已确认提交强杀保留、未提交回滚和恢复中再次中断的既有独立测试。
- 不支持语法明确拒绝。本轮 NULL、JOIN、聚合、子查询、ALTER、外键等小型探测均被拒绝，没有降级成别的含义。

### 已复现的 SQL 缺陷：重复投影误报排序歧义

对独立小表：

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY,s VARCHAR(2),v VECTOR(2));
INSERT INTO t VALUES (2,'中😀','[3,4]'),(1,'é','[0,0]');
SELECT id,id FROM t ORDER BY id;
SELECT *,id FROM t ORDER BY id;
SELECT s,s FROM t ORDER BY s;
```

三条查询均返回 `ambiguous_column`，而重复的相同源列并不构成实际排序歧义；没有 ORDER BY 时重复投影正常。定位：`minivectordb/cpp/executor.cpp:283-291`，第二个同名投影即被当作歧义，未判断其是否为同一个表达式。

这是普通投影和 ORDER BY 组合中的具体缺陷，不能只因公开样例未覆盖而忽略。但仍没有证据说明正式失败就是它。本轮只记录，未顺手修改生产代码。

### 高优先级未验证：超过内存预算的数据及受限时间

网页 2.3 和 3.2 明确将其列为功能准入，不是只影响性能分。

当前表文件扫描和无界排序有流式/外排路径，但仍存在：

- 每次 INSERT/UPDATE/DELETE 重写整个受影响表，见 executor.cpp 的 insert_rows/rewrite_rows。
- 自动提交时还会流式验证所有候选根表并同步文件，见 root_store.cpp:213-248；修改小表也可能扫描其他大表。
- 重复小批次插入累计 I/O 可接近二次增长；事务中连续修改也可能反复重写。
- INSERT 批次在 Python AST/IR/native additions 中物化；最终查询结果和 JSON 响应也物化。
- 正式 CPU/内存下的页缓存、磁盘容量、恢复启动时间与持续写入时间尚未测定。

不能把“表文件逐行读取”当作“已通过超内存资源准入”，也不能未经测量宣称一定超限。需要下一阶段专门设计有明确预算、超时和清理条件的资源验证；本轮没有运行大规模压力测试。

### ANN：不要误判要求

当前 CREATE VECTOR INDEX 保存 FlatScan 描述符，exact/ann 都扫描当前可见根。它不是近似加速结构，但现有扫描可满足有效 ID、过滤、无重复、当前可见记录和精确召回。

网页将近似索引细节交给统一题包，benchmark 也允许固定 exact 或 ann，且参考基线是精确扫描。因此“没有 HNSW/近似算法”本身不能被认定为此次功能失败；实际正式规模下的时间/资源限制仍可能使 exact fallback 不合格。

### float32 下溢：待澄清，不擅自认定缺陷

最小正非正规 float32、最大有限 float32 和负零的小型写入探测通过。`[1e-46,0]`、`[1e-50,0]` 被当前 from_chars 以 type_error 拒绝；通用 float32 转换可将其舍入为 0。题包未明确下溢舍入策略，因此先保留事实，不把 NaN/溢出拒绝测试通过扩大为所有舍入边界通过，也不凭空改成另一种策略。

### 交付/评分材料，不是 SQL 故障

- 分支交付问题已按用户授权修正。
- `agent/conversation.json` 不存在；根目录 conversation.json 为 0 字节，不是合法 JSON。网页 4.4 写明默认入口是 agent/conversation.json、顶层对象、UTF-8、不超过 128 KiB。Agent 材料尚未完成，不能靠源码或日志路径代替，更不能编造。这会影响 Agent 评价，不应混同为已查明的 MySQL case 根因。
- 性能 benchmark 的三轮 T/B/M、cgroup v2 整容器与页缓存、I/O 写回窗口均未实施。其基准成绩尚不存在，不能从小测试时间推导得分。
- 4.2 的从空目录演示写入、查询、向量、提交、回滚、重启的运行说明仍可补齐；本轮未顺带改动历史容器说明。

## 五、本轮实际验证结果

| 检查 | 实测结果 |
|---|---|
| 撤除前运行原有 NULL 拒绝断言 | 预期失败（旧断言发现 NULL 被接受） |
| 撤除后原前端测试 | 57/57 通过 |
| 撤除后生产/故障构建 | 成功 |
| CTest | 10/10 通过 |
| pytest，不含容器 | 122/122 通过 |
| pytest，含 Podman 容器 | 124/124 通过；推送前在纯源码镜像上再次通过 |
| 独立边界探测 | 28 项：23 项符合预期、3 项重复投影缺陷、2 项下溢策略待定 |
| 纯 git 归档、无缓存构建 | Podman 成功 |
| 纯源码镜像的原驱动检查 | Podman --command：握手通过、公开 20/20 通过，报告校验接受 |
| named volume + 受限容器 + 实际容器 SIGKILL | Podman 通过，4 次同卷启动，容器/卷清理完成 |
| 实际 Docker --image 握手 | 本机缺 docker，失败在引擎调用前；不能算数据库验收成功 |
| 官方协议/公开题包差异 | 无修改 |

证据位于 `.superpowers/sdd/admission-audit/`：`requirements.txt`、`docker-section-source.txt`、`boundary-results.json`、`boundary.log`、`clean-build.log`、`runtime-dependencies.log`、`named-volume-lifecycle.json`、`clean-podman-{handshake,public}/result.json`、`actual-docker-handshake/result.json`、`pre-push-{ctest,pytest}.log` 等。

撤除的 NULL 扩展证据仅在 `.superpowers/sdd/null/withdrawn/` 保存；该目录原有 NULL 报告不是当前版本验收证据。

## 六、建议排查顺序

1. 提测页面核对 HTTPS 仓库、目标分支和完整 SHA，避免继续评测已锁定的旧基线。
2. 有实际 Docker 环境时，先用同一纯源码提交构建，再跑原 --image 握手和完整公开用例；不要切换成裸 SQL 碰运气。
3. 修复并回归已复现的重复投影排序问题。
4. 为超过内存的数据、持续批次写入、跨表提交及恢复设定明确资源预算，定位是否撞上正式功能时限/磁盘/内存，而不是直接开始广泛优化。
5. 正式功能稳定后，单独完成性能测量和真实 Agent 材料。

本记录只新增审计文档，没有修复上述 SQL 缺陷，没有自动提交该文档或再次推送。
