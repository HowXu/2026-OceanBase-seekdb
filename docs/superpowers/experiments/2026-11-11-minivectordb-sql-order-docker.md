# MiniVectorDB 重复投影排序修复与实际 Docker 验证

## 版本与改动范围

基线提交：`3e6792da48a533ee09160c8f9677ab02bb42ca09`，当前分支 `2026-competition-round2`。本记录对应基线加当前未提交改动，不冒称全部内容已存在于该 SHA。

本轮仅修复普通 SQL 的重复投影排序误报歧义，不增加 NULL 或其他题目外功能，不做资源/性能重构，不自动暂存、提交、推送或提测。

生产改动位于 `minivectordb/cpp/executor.cpp`：

1. 为 `BoundExpression` 增加默认值相等比较，覆盖源列、字面量、声明类型和距离查询向量。
2. 解析 ORDER BY 的同名投影时，仅在两个绑定表达式不同时报 `ambiguous_column`。

不能只取第一个匹配项，也不能比较当前行的计算结果来决定歧义：不同列当前值相同仍然是不同表达式；空表和 LIMIT 0 同样必须进行绑定检查。

## RED → GREEN

`minivectordb/tests/test_sql_semantics.py` 新增 45 个参数化小用例：

- 36 个成功组合：重复 id、`*,id`、重复字符串列、同源重复别名、重复距离表达式、重复常量；分别覆盖 ASC/DESC、无 LIMIT、LIMIT 2、LIMIT 0。
- 9 个拒绝组合：不同列、不同常量、不同距离查询共享同名别名；覆盖空表、有行、LIMIT 0。其中不同源列当前值特意设为相同，以防错误地按数据判等。

修改生产代码前：36 个正例因 ambiguous_column 失败，9 个真正歧义用例通过。修改后：整个标量语义模块 49/49 通过。

日志：`.superpowers/sdd/sql-order-fix/{red.log,green.log}`。

## 实际 Docker 环境

不是 Podman 替代测试。实际 Docker 客户端/守护进程均为 `28.5.2+dfsg4`，Linux x86_64，cgroup v2。

使用用户 `howxu`（UID 1000），没有切换 root。当前 Pi 进程尚未继承用户新加入的 docker 组；通过 `sg docker -c ...` 刷新子进程组身份后连接 `/var/run/docker.sock`。没有修改 socket 权限、用户组配置或守护进程设置。

## 构建来源与 Dockerfile 验证

将 `git archive HEAD` 导出到新的临时目录，仅覆盖本轮修改的 C++ 源码和回归测试文件，得到基线加本轮改动的源码上下文；没有复制宿主 `.venv`、build 或预编译 native 扩展。

实际命令：

```bash
sg docker -c "docker build --no-cache --progress=plain \
  -t minivectordb:round2-sql-order \
  -f '<纯源码目录>/Dockerfile' '<纯源码目录>'"
```

构建通过。镜像 ID：

```text
sha256:9c0a009fd554d212e8e082e786976aed47233736bcda49dbdcbafd0f0cac258d
```

沿用多阶段源码构建、默认 exec-form ENTRYPOINT、默认 `/data` 和关闭生产故障注入的 Dockerfile，没有为测试追加私有启动参数。源码补丁与指纹记录于 `.superpowers/sdd/sql-order-fix/{source.patch,source.sha256}`，上下文路径记录于 `docker-context-path.txt`。

## 原始 --image 判题路径

未修改 `protocol_check`、公开样例、断言或资源配置；不是 `--command` 包装器，也没有 Docker→Podman 别名。

```bash
sg docker -c '.venv/bin/python protocol_check/run.py \
  --image minivectordb:round2-sql-order --handshake-only \
  --output .superpowers/sdd/sql-order-fix/docker-handshake'
sg docker -c '.venv/bin/python protocol_check/run.py \
  --image minivectordb:round2-sql-order \
  --output .superpowers/sdd/sql-order-fix/docker-public'
.venv/bin/python minivectordb/tools/check_delivery.py \
  --report .superpowers/sdd/sql-order-fix/docker-public/result.json \
  --manifest mysqltest/manifest.json
```

结果：握手通过、公开 20/20 cases（95 SQL）通过；报告校验接受 20 个唯一 PASS ID。

驱动实际创建 Docker named volume 并以默认入口启动受限容器：2 CPU、2 GiB、禁 swap、128 PID、禁网、只读根、cap-drop ALL、no-new-privileges、`/data` 可写。不是复用宿主开发数据。

## 原始 Docker 传输的强杀/恢复小型检查

附加脚本 `.superpowers/sdd/sql-order-fix/check_docker_lifecycle.py` 直接使用原 `Session(..., image=...)`、`Session.crash()`、shutdown/start。检查：

- 实际容器 inspect 的内存、swap、CPU、PID、只读根、禁网及 named volume 配置。
- 容器中重复投影、SELECT * 加重复列排序成功；不同源同名别名继续报错。
- 实际 Docker KILL 数据库容器后，同卷重启丢弃未提交修改。
- 收到 COMMIT 成功再 KILL，重启后修改保留，标量/向量结果一致。
- 正常 shutdown 回滚未提交修改，随后同卷启动状态正确。
- NULL 明确返回 unsupported_sql。
- 最后原 Session 清理容器和数据卷，无本次残留。

这些是自写、确定性、小数据的故障检查，不是声称运行了未公开的官方故障用例。

## 回归结果

| 检查 | 结果 |
|---|---|
| 生产与故障注入构建 | 通过 |
| 标量语义测试 | 49/49 通过 |
| CTest | 10/10 通过 |
| 全量 pytest，显式选择实际 Docker | 169/169 通过，无 skip/xfail |
| Docker 无缓存纯源码构建 | 通过 |
| 原 --image 握手 | 通过 |
| 原 --image 公开用例 | 20/20 通过 |
| 原公开报告校验 | 接受 20 个唯一 PASS ID |
| Docker 原 Session 强杀/恢复 | 通过 |

全量回归命令（在具有 docker 组身份的子进程中）：

```bash
export PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python"
export MINIVECTORDB_FAULT_BUILD="$PWD/build/task11-fault/python"
export VECTORDB_CONTAINER_ENGINE=docker
export VECTORDB_IMAGE=minivectordb:round2-sql-order
.venv/bin/python -m pytest minivectordb/tests -q
```

原始证据位于 `.superpowers/sdd/sql-order-fix/`，包括 docker-version.log、docker-build.log、docker-handshake/result.json、docker-public/result.json、docker-lifecycle.json、ctest.log、full-pytest.log。

一次复测在 pytest 清理旧默认临时目录时出现 9 个清理警告：此前 Docker 在临时 bind mount 中创建的子文件归 root 所有。该次仍为 169 通过，原日志保留于 full-pytest-before-isolated-temp.log。最终使用全新的独立 `--basetemp` 目录复测，169 通过且无警告，未修改或过滤测试断言；并仅对本次生成的测试目录用容器恢复为 UID/GID 1000 所有，便于后续清理。没有改变 Docker socket 权限或生产程序权限。

## 独立只读审查

审查覆盖 executor 的绑定表达式相等性、同名排序项解析、两条排序路径和新增回归测试，未发现需要处理的 Major/Blocker。审查者读取了源码与原始测试日志，但没有独立运行测试或构建；不将其结果扩大为全量认证。原始审查报告保存在 `.superpowers/sdd/sql-order-fix/review-initial.md`。

## 结论边界

现在有实际 Docker 默认入口、正式公开传输路径和受限容器的证据，不再沿用“本机无 Docker”的旧结论。但公开用例和小型故障检查仍不能证明正式 29 个用例全部通过。

此前正式失败的实际锁定版本和失败阶段仍未知；超内存数据、持续批量写入/跨表提交的时间与资源约束仍未验证。没有运行性能 benchmark，没有真实 ANN 加速，没有生成 Agent 材料或成绩。
