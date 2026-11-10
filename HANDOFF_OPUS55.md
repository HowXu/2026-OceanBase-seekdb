# MiniVectorDB Opus 5.5 交接文档

更新时间：2026-11-10，Asia/Shanghai。

本文件交给新的 Opus 5.5 窗口使用。先阅读本文，再检查实际 Git 状态和文件，不要依据历史摘要猜测当前仓库。

## 1. 任务目标

这是 OceanBase 加赛题一 MiniVectorDB。优先级是：

1. 通过正式 MySQL case 功能准入。
2. 在同一提交中提供源码、Dockerfile 和 `agent/conversation.json`。
3. 功能准入通过后，才有性能和 Agent prompt 排名分。

用户已决定：Astra 和 Opus 历史对话只读，后续在新的 Opus 5.5 窗口处理；学习证据必须真实、可定位、互不重复。

## 2. 仓库和 Git 状态

完整源码仓库：`/Users/timmy-south/Documents/Codex/2026-11-09/qin/outputs/seekdb`。

本地分支：`minivectordb-dev`。正式提测分支：`2026-competition-round2`。

远端：`private=https://github.com/Dline666/seekdb-2026.git`，`origin=https://github.com/oceanbase/seekdb.git`。

本地 HEAD：`9345d9fc6c4783ea46591214c149deb405346b85`，提交信息为 `Restructure evidence into deduplicated scoring units`。

远端最后确认：`3f8aa6095d3c7fb26467bec475da933116450f8a`，提交信息为 `Fix filtered vector TopK admission`。

本地新增但尚未确认推送的提交：`a66320e`、`4874ae6`、`9345d9f`。

新窗口首先执行：`git status --short --branch`、`git log --oneline --decorate -8`。

GitHub 最近多次返回 `curl 52 Empty reply from server`。之前成功推送使用 HTTP/1.1 和较大的 postBuffer：`GIT_TERMINAL_PROMPT=0 git -c http.version=HTTP/1.1 -c http.postBuffer=524288000 push private HEAD:refs/heads/2026-competition-round2`。推送失败时不要强推、reset 或 rebase 覆盖本地提交；保留 SHA，稍后重试或切 SSH。

## 3. 当前代码和最近修复

源码位于 `minivectordb/cmd/minivectordb/main.go`、`minivectordb/internal/engine/`、`minivectordb/internal/proto/`、`minivectordb/internal/sqlparse/`、`minivectordb/internal/storage/` 和 `minivectordb/internal/types/`。

根目录 `Dockerfile` 使用 Ubuntu 24.04、多阶段构建和 Ubuntu Go 1.22 包。

`3f8aa60` 修复了 filtered TopK：旧实现先按全表距离进入 heap，再执行 `WHERE`，不满足条件的近邻会占据 TopK 名额；现在先过滤再进入 heap，并新增 `TestFilteredTopKFiltersBeforeAdmission`。

本地已验证：`go test ./...` PASS，`go vet ./...` PASS，公开 cases 为 20 passed/0 failed，kill-9 crash test 40 rounds PASS，filtered TopK 回归测试 PASS。

公开样例只有 20 个，题包说明正式用例共 29 个，另有 9 个保留用例。公开 20/20 不能证明正式准入通过。

## 4. 正式提测状态

提测页面是 `https://open.oceanbase.com/train/TestRecord?questionId=600081`。用户已经打开页面，但尚未确认点击“立即提测”。

页面截图显示上一版本 MySQL 正式用例未通过，因此性能和 Agent prompt 不计分。`3f8aa60` 尚未取得新的正式平台反馈。

下一次提测前必须确认远端 SHA 与本地 SHA 一致；最终“立即提测”按钮需要用户确认。

## 5. 题目硬约束和代码风险

- Linux x86_64 容器，前台运行；stdin 接收 SQL，stdout 返回结果，stderr 只放日志。
- 数据和恢复文件写入 `/data`；单机、单进程、单客户端，最多一个显式事务。
- 支持 CREATE TABLE、INSERT、SELECT、UPDATE、DELETE、BEGIN、COMMIT、ROLLBACK 和自动提交。
- 过滤先于 TopK，距离并列按主键升序；exact 必须精确。
- 拒绝 NaN、Infinity、维度错误和 float32 溢出；语句失败不能留下部分修改。
- 已确认提交强杀后必须保留；恢复中再次被杀必须可重入。
- 正式数据可能超过内存限额，不能只依赖全量内存加载。
- 索引重建完成前不能输出 ready。
- 正式结果格式、错误码、ready、退出和 exact/ann 切换以题包为准，不能把本地 JSON runner 当官方协议。

优先审查：README 承认 JSON 响应、`READY` 和部分协议仍是假设；正式错误码和 ready 尚未被平台确认；超内存路径、Linux cgroup `memory.peak`、`io.stat` 和 Docker build 尚未在本轮完整验证；历史 C++ 线曾发现 L2_DISTANCE 平方值与题面欧氏距离的冲突；ANN 小数据回退 exact 是否符合正式驱动也要以题包为准。

## 6. Astra / Opus 历史材料

不要继续向两个线程发消息，只读使用。

Astra 主要材料：正确性优先、可审计 kill/recovery、故障注入、fsync marker 假阴性、batch frame、原始 SHA、非空闲机器、flat view 来源审计，以及协议/SQL/事务/超内存缺口审计。

Opus 主要材料：批次 frame 和 trailer、no_fsync 语义、独立参考实现、K 边界浮点反例、严格浮点取舍、batch=100 写入统计、L2_DISTANCE 输出规范风险，以及“先补准入再做 ANN/flat/SIMD”的优先级。

历史结果来自不同 commit、worktree 和 macOS arm64，不能拼成当前版本正式 benchmark；JSON 已标记历史或 `reference-only`。

## 7. 当前 conversation.json

文件是 `agent/conversation.json`。当前约 70 KB，包含 31 条真实对话、27 条结果记录、24 个 evidence unit，并内嵌完整 `FINAL_AGENT_PROMPT.md`。

证据规则：`counted_in_score=true` 才进入 `rubric_coverage`；重复内容用 `merged_into` 或 supporting evidence 保存但不重复计分；高于 0.5 分的主 unit 必须有独立 `result_evidence`；每个引用必须是 JSON 内 RFC6901 pointer 指向的字符串且不超过 240 字符；不能把 Agent 工作冒充学生理解。

当前最强主 unit 是 `U18`：旧 commit/旧 frame 导致的 137 行预测修正，再迁移到第 37 行中断场景，并明确小维度、进程强杀和 macOS 局限。它可作为接近 2 分的候选。多数其他 unit 仍是 1 或 1.5 分，不应强行升档。

## 8. JSON 校验

执行 `jq empty agent/conversation.json`，并执行 `cmp -s <(jq -j '.prompt_material.content' agent/conversation.json) FINAL_AGENT_PROMPT.md` 和 `git diff --check`。

还要检查：所有 pointer 可定位；excerpt 是 pointer 指向字符串的精确子串；student/result excerpt 不超过 240 字符；高于 0.5 分的主 unit 有结果证据；主 unit 的结果 pointer 不重复；`rubric_coverage` 只含主 unit；没有重复 JSON key；文件小于 128 KiB。

## 9. 新窗口执行顺序

第一步，恢复状态并确认本地 HEAD、远端最后确认 SHA 和工作区。

第二步，修正当前 JSON 的重复 key、primary unit 标记、pointer 数组索引和 coverage，然后重新通过全套校验。

第三步，使用 HTTP/1.1 命令推送本地最新材料 commit；成功后用 `git ls-remote` 核对远端 SHA。

第四步，用远端已确认的 commit 做一次正式提测；提交前核对仓库、分支和完整 commit，最终按钮等用户确认。

第五步，拿到正式失败类别后再改代码，不要盲目继续 ANN 或 flat storage。

第六步，新增实例时要求用户先写预测、解释、修正、新场景和局限；Agent 只负责实验、原始输出、SHA、commit 和 JSON 结构。

## 10. 禁止事项

- 不要继续向 Astra 或 Opus 线程发送消息。
- 不要把历史 C++ 代码合并进当前 Go 源码。
- 不要拼接不同 commit 的最佳性能结果。
- 不要把 macOS 结果写成 Linux 正式排名。
- 不要把公开 20/20 写成正式 29/29。
- 不要把 Agent 工作冒充学生学习证据。
- 不要强推覆盖远端分支。
- 不要未经用户确认点击正式提测。

## 11. 交接结论

最有价值的顺序是：先校验和推送材料，再用远端已确认 commit 正式提测；拿到失败类别后再改代码。当前不能宣称功能准入、性能排名或 Agent prompt 正式分数已经通过；所有本地分数都是估计或证据上限，不是平台成绩。
