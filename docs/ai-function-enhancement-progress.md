# AI 函数增强：实现与验证记录

## 基线与范围

本题以 `upstream/2026-competition` 的 `5bce9e06d1281347ad61344e4b01fab9158c58a2` 为代码基线，以[赛题一：AI函数增强](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/psu9fks8dexldoum)（2026-10-26 更新）为需求依据，不以最新 master 或旧自写测试替代竞赛要求。

只处理六个 AI 子题及其相关回归。`max_concurrency` 不在本题功能及评分表内；不因其他 DDL 测试失败而扩展本题实现，也不修改无关构建适配或其他赛题。

**以下是本地验证结果，不是组委会验收结论。** 本次没有取得组委会 Mock，使用确定性的本地协议 Mock，未调用真实模型。生产代码不包含针对模型名、输入或响应的硬编码。

## 实现摘要

| 子题 | 实现与检查范围 |
|---|---|
| AI_PROMPT | TEXT/LONGTEXT 的真实文本读取；JSON Array/Object 参数；嵌套 Prompt 渲染；重复占位符；数字下标越界及溢出检查；Unicode、转义和内嵌 NUL |
| AI_EMBED 单行 | NULL 内容传播；正整数 dimension；默认维度从响应校验；返回数量、向量类型、维度一致性和有限数值检查；文本 LOB 读取 |
| AI_COMPLETE | 普通 Prompt 与 AI_PROMPT；NULL/JSON config；temperature、max_tokens 传递；长文本；非空字符串响应检查 |
| Ollama | Endpoint 增删改查及 Provider 大小写兼容；省略/清空 access key；原生 generate/embed；JSON header；非流式生成；options 映射 |
| AI_RERANK | 字符串/对象数组；省略或 SQL NULL 文本键使用字符串数组模式；非 NULL 文本键使用对象数组模式；每批 20 条；返回数量、index、分数校验；全局 index 映射；分数降序及同分原始 index 升序；保留对象原始 ID |
| AI_EMBED 跨行 | 表达式批量求值；同模型/维度每请求最多 64 条；NULL/skipped 行过滤及原始行映射；OpenAI 乱序响应恢复；过滤、LIMIT、多批次和失败处理 |

## 本次修正

竞赛原始 `ai_function_enhancement.test` 使用四参 `AI_RERANK(model, query, documents, NULL)` 处理字符串数组。旧实现按第四参数指针是否存在选择模式，误把显式 SQL NULL 当成对象文本键，返回错误 1210：`document_array element is not object`。

此前归档回归只测试三参形式，未发现这一差异。本次使数组校验与实际执行共用“第四参数存在且非 NULL”的条件，没有改动排序、分批或模型协议。

新增回归位于：

```text
unittest/sql/engine/expr/mysqltest/ai_rerank_enhancement.test
unittest/sql/engine/expr/mysqltest/ai_rerank_enhancement.result
```

覆盖三参/显式 NULL 结果等价、运行时 nullable 文本键逐行切换模式，以及对象数组缺少文本键、字符串数组指定文本键、空文本键等非法输入。修改前已复现同一错误，修复后通过。新增测试保留在 `unittest/`，未改动竞赛原始主用例及预期结果。

## 最近验证（2026-10-28）

### CMake 增量构建

复用用户已构建的 `build_release`（`RelWithDebInfo`）：

```bash
cmake --build build_release --target seekdb -j3
```

退出码为 0；测试使用更新后的 `build_release/src/observer/seekdb`，不是旧分支的二进制。

### 竞赛 Mysqltest

使用仓库 `.github/script/seekdb/mysqltest_for_seekdb.py` 的 `run_case` 和环境设置，运行以下用例：

| 用例 | 结果 |
|---|---|
| `tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test` | 通过，原始 test/result 与竞赛基线一致 |
| `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test` | 通过，保留竞赛预期结果 |
| `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt_enhancement.test` | 通过，JSON Array/Object 补充回归 |
| `unittest/sql/engine/expr/mysqltest/ai_rerank_enhancement.test` | 通过，NULL 文本键补充回归 |

主用例依赖 `0.0.0.0:8000` 的 HTTP 服务。本地 Mock 为 OpenAI completion/embedding、SiliconFlow rerank、Ollama generate/embed 提供通用协议响应；Embedding 默认 768 维。没有按输入特判结果，也没有降低竞赛 test/result 的预期。该服务不是组委会官方 Mock。

### C++ 单元测试

题目指定的两个测试源码已接入现有 Bazel 目标 `//unittest/sql/engine/expr:test_ai_function_utils`：

```text
unittest/sql/engine/expr/ob_expr_ai_prompt_test.cpp
unittest/sql/engine/expr/test_ob_openai_utils.cpp
```

本机未使用 Bazel 执行该目标。验证程序使用现有 CMake 编译参数生成的测试对象，重新链接本次更新后的生产静态库，移除生产 main、加入 libgtest。运行结果：**14 tests，0 failures**。这不表示 CMake 已自动集成该测试目标。

### 补充 SQL 边界回归

从 `b474c1d91` 恢复归档 Mock/Python 测试到临时目录，仅调整仓库路径，未恢复到提交内容中。使用当前二进制运行：**10 tests，全部通过**。

覆盖六个子题的长文本、嵌套 Prompt、显式维度、跨行请求合并、NULL、过滤、LIMIT、乱序响应、150 行多批次、47 文档跨批次排序及 ID 映射、Ollama 无 key，以及非法 JSON、HTTP 错误、断连、重复/缺失 index、非法分数和失败后继续执行普通 SQL。

这些自写回归仅用于补充边界证据，不替代竞赛原始用例。

## 环境与验证边界

- 本次日志、链接命令及本地运行辅助脚本保存在 `/var/tmp/seekdb-task1-fix.cKBS8F/`。
- 数据库使用独立测试实例 `/var/tmp/seekdb-comlp-tests.Cj3PO2/sql-instance`，SQL 端口 `18881`，不使用已有业务数据。
- Mock 使用端口 `8000`；验证结束后关闭本次数据库和 Mock。
- 原有未跟踪目录 `unittest/scripts/` 及既有 `build.sh` 修复保留，不并入本次 AI 修正。
- 未运行全仓库测试、其他赛题、Sanitizer 或资源泄漏专项测试；不承诺比赛得分。
