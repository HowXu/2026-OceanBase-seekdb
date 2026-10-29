# Kali FTS Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 合入指定 Kali 适配提交并完成可验证编译，然后依据赛题计分实现、代码差异和同机测量报告 FTS 初步优化瓶颈。

**Architecture:** 在当前 `2026-competition-fts` 分支只引入一个指定提交，复用现有 `build_release` 构建树。环境适配与性能探索严格分离；性能结论以实际计分代码、调用链和同机 A/B 数据为依据。

**Tech Stack:** Bash、Git、CMake、C++、Python 3、仓库 FTS benchmark、Linux profiler（可用时使用）。

## Global Constraints

- 只 cherry-pick `1032025add14d5acbe836a6c31c77942ba438dac`，不合并整个 `kernel/2026-competition` 分支。
- 不执行 `telemetry_test.py` 或任何外部上报。
- 适配阶段不修改 FTS 性能实现；不合并无关重构。
- 不跨机器或跨构建配置直接比较性能。
- 遇到编译失败先定位根因；任何修复须使用 systematic-debugging 与 test-driven-development，先建立失败复现，再单项修改。
- 所有完成声明须有实际命令输出；不可运行的测试必须说明原因。

---

### Task 1: 合入并静态验证 Kali 适配

**Files:**
- Modify: `.gitignore`
- Modify: `deps/init/dep_create.sh`
- Create: `telemetry_test.py`

**Interfaces:**
- Consumes: 提交 `1032025add14d5acbe836a6c31c77942ba438dac` 与当前 HEAD `f3ddbd9b3`。
- Produces: Kali 使用 CentOS 9 依赖配置的源码树，不运行遥测脚本。

- [ ] **Step 1: 确认合入前工作区与目标提交**

Run:
```bash
git status --porcelain
git show --no-patch --oneline 1032025add14d5acbe836a6c31c77942ba438dac
```
Expected: 第一条命令无输出；第二条显示 `chore: suit for kali linux`。

- [ ] **Step 2: 合入指定提交**

Run:
```bash
git cherry-pick -x 1032025add14d5acbe836a6c31c77942ba438dac
```
Expected: 创建一个 cherry-pick 提交，不冲突。若冲突则停止并报告具体文件，不改写其他提交。

- [ ] **Step 3: 验证补丁范围和脚本语法**

Run:
```bash
git diff HEAD^..HEAD --check
bash -n deps/init/dep_create.sh
python3 -m py_compile telemetry_test.py
git show --stat --oneline HEAD
```
Expected: 前三条命令退出码为 0；统计显示 `.gitignore`、`deps/init/dep_create.sh` 和 `telemetry_test.py` 三个文件。`py_compile` 仅做语法检查，不发起请求。

- [ ] **Step 4: 初始化 Kali 开发依赖**

Run:
```bash
./build.sh release --init
```
Expected: 退出码为 0，Kali 被识别并选择兼容的 CentOS 9 依赖。若失败，记录首个有效错误和 `/etc/os-release`，完成根因调查后才增加修复任务。

### Task 2: 完成 Release 编译与适配回归

**Files:**
- Modify: 仅限 Task 1 或实际编译错误证明需要修改的现有构建文件。
- Test: `build_release/src/observer/seekdb`；针对性测试目标由实际改动确定。

**Interfaces:**
- Consumes: Task 1 初始化后的 `deps/3rd` 和已有 `build_release/CMakeCache.txt`。
- Produces: 当前源码对应的 `build_release/src/observer/seekdb`，以及可复现的构建验证结果。

- [ ] **Step 1: 复现当前源码的 Release 构建**

Run:
```bash
./build.sh release --make -j8
```
Expected: 退出码为 0。若失败，保留完整错误输出，从首个编译或链接错误追踪到输入源文件及配置；不得同时尝试多个修补。

- [ ] **Step 2: 如有错误，为单项根因补充计划和回归测试**

Run: 使用能稳定触发该错误的最小构建目标或脚本命令作为失败测试。
Expected: 修复前失败，最小修复后同一命令通过。每个修复单独验证和提交；没有实际错误时不创建测试文件或兼容性补丁。

- [ ] **Step 3: 验证二进制及源码树**

Run:
```bash
test -x build_release/src/observer/seekdb
build_release/src/observer/seekdb --version
git diff --check
git status --short
```
Expected: 二进制可执行并打印版本；无空白错误；状态只包含已报告的适配或计划文件改动。

### Task 3: 理解赛题、计分与初步优化路径

**Files:**
- Read: `tools/benchmark/fts_large_bench.sh`
- Read: `tools/benchmark/fts_large_bench_score.py`
- Read: `tools/benchmark/fts_large_bench_baseline.json`
- Read: `tools/benchmark/fts_large_bench_setup.sql`
- Read: Fqin 提交涉及的 `src/storage/fts/`、`src/storage/retrieval/` 和回归 SQL。
- Reference: 用户提供的语雀赛题页面；若正文需要登录，明确标记未获得的约束，不猜测。

**Interfaces:**
- Consumes: 当前 HEAD 相对 `5bce9e06d` 的 FTS 差异及用户提供的 JSON 指标。
- Produces: 指标定义、基准可比性说明、按代码路径分组的瓶颈假设及证据缺口。

- [ ] **Step 1: 核对得分公式和基准**

Run:
```bash
python3 tools/benchmark/fts_large_bench_score.py --help
git diff 5bce9e06d..HEAD -- tools/benchmark/fts_large_bench_baseline.json
```
Expected: 明确命令参数；从脚本计算每个 improvement 的符号、均值方式和零分阈值，并确认基准是否来自同一环境。

- [ ] **Step 2: 复算用户提供的结果**

Create `run_temp/initial-report.txt`，逐行写入用户提供的 `metrics` 键值，格式为 `fts_mean_improvement: -0.0557631546667764`，其余所有指标同样保留原始数值。

Run:
```bash
python3 tools/benchmark/fts_large_bench_score.py run_temp/initial-report.txt
```
Expected: 输出各计分类别及其中每个指标的 improvement；不把 `fts_mean_improvement` 直接解释为端到端耗时变化，区分测量退化与不可比基准。

- [ ] **Step 3: 检查优化提交及正确性覆盖**

Run:
```bash
git log --reverse --format='%h %s' 5bce9e06d..HEAD -- src/storage/fts src/storage/retrieval unittest/storage/fts_unicode_regression.sql
git diff --check 5bce9e06d..HEAD -- src/storage/fts src/storage/retrieval
```
Expected: 逐项说明 UTF-8/IK 字符分类、arbitrator/processor、文档 ID 比较和小 posting-list 合并改变了什么；列出已有 SQL 覆盖与未覆盖风险。

### Task 4: 同机验证热点并输出优化优先级

**Files:**
- Read: `tools/benchmark/fts_large_bench.sh` 的部署、数据生成和运行参数。
- Create: 仅在已有测试套件无法覆盖新优化时添加对应回归测试；不新增 `tools/` 下的一次性测试文件。
- Report: 会话最终报告及必要的原始结果文件，放在被 `.gitignore` 排除的 `run_temp/` 下。

**Interfaces:**
- Consumes: Task 2 的可执行文件、Task 3 的计分解释和候选代码路径。
- Produces: 可复现基线、热点证据、优化项排序；本计划不直接实施性能优化。

- [ ] **Step 1: 建立当前分支同机基线**

Run: 先执行 `bash tools/benchmark/fts_large_bench.sh --help`，再按脚本实际支持的参数启动隔离实例并运行当前源码；保留完整 JSON，记录构建 commit 和运行条件。
Expected: benchmark 正常退出且正确性指标无异常；若环境阻塞，报告阻塞，不伪造绝对性能结论。

- [ ] **Step 2: 对初步优化做可复现 A/B**

Run: 工作树切换前执行 `git status --porcelain` 并确认无输出，再在同一构建配置下比较 `5bce9e06d` 与当前 HEAD；两次均使用 Step 1 的 benchmark 命令及相同样本数。
Expected: 每个关键指标有中位数和波动；不能复现的差异标为噪声或环境差异，不声称优化有效或无效。

- [ ] **Step 3: 采集热点并排序优化点**

Run: 先执行 `cat /proc/sys/kernel/perf_event_paranoid`；若权限允许，对建索引、tokenize 和查询分别执行 `perf record -g -- <benchmark-command>` 并用 `perf report --stdio` 读取热点。若被拒绝，使用 benchmark 分阶段计时和代码路径计数替代，并说明证据限制。
Expected: 热点与实际函数对应；候选优化按预期收益、正确性风险和验证成本排序，每项指定同机 A/B 与正确性验证方式。

- [ ] **Step 4: 最终一致性检查**

Run:
```bash
git diff --check
git status --short
```
Expected: 没有意外性能改动或生成文件；最终报告分别给出编译结果、0 分原因、初步优化瓶颈和下一轮建议，明确区分实测事实与推断。
