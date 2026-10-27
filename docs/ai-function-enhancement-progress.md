# AI 函数增强：实现与验证记录

## 当前状态

六个子题的实现已接入 seekdb SQL 引擎及原有 Provider 路径。使用重新编译的 seekdb 二进制完成本地回归；没有调用真实模型，也没有在生产代码中硬编码测试模型名、输入或响应。

**本地验证通过不等于组委会验收通过。** 组委会 Mock 服务和缺失的原始测试没有提供，无法承诺比赛得分。

## 实现摘要

| 子题 | 已实现 |
|---|---|
| AI_PROMPT | TEXT/LONGTEXT 的真实文本读取；JSON Array/Object 参数；嵌套 Prompt 渲染；重复占位符展开；数字下标越界及溢出检查；Unicode、转义和内嵌 NUL 处理 |
| AI_EMBED 单行 | NULL 内容传播；正整数 dimension 检查；默认维度从响应校验；返回数量、向量类型、维度一致性和有限数值检查；文本 LOB 读取 |
| AI_COMPLETE | 普通 Prompt 与 AI_PROMPT；NULL/JSON config；temperature、max_tokens 请求传递；长文本；非空字符串响应检查 |
| Ollama | Endpoint Provider 校验与大小写兼容；省略/清空 access key；元数据读取兼容空 key；原生 generate/embed；JSON header；非流式生成；原生 options 映射 |
| AI_RERANK | 字符串/对象数组及文本字段选择；每批 20 条；返回数量、index、分数校验；批次 index 转全局 index；按分数降序、原始 index 升序排序；对象原始 ID 保留 |
| AI_EMBED 跨行 | 表达式批量求值；同模型/维度每请求最多 64 条；NULL/skipped 行过滤及原始行映射；OpenAI 乱序响应恢复；过滤、LIMIT、多批次和失败处理 |

异常响应还补充了 HTTP JSON 根节点类型检查，以及错误诊断函数的空指针检查。

## 最后一次验证

### 构建

由用户完成当前源码构建：

```bash
python3 build.py build //:seekdb -j3 --action_env=LDFLAGS=-fuse-ld=lld
```

本机 Kali 的旧 linker 不支持系统 libc 的 `.relr.dyn`，使用 lld 避开该构建环境问题，没有为此修改生产构建规则。

### SQL 端到端回归

测试使用真实 SQL 客户端连接新二进制，通过本地 HTTP Mock 返回确定性结果。

```text
test_complete ... ok
test_embed ... ok
test_embed_batch ... ok
test_embed_batch_boundaries_and_failures ... ok
test_http_failures ... ok
test_ollama ... ok
test_ollama_endpoint_crud_without_key ... ok
test_prompt ... ok
test_rerank ... ok
test_rerank_invalid_responses ... ok
Ran 10 tests in 3.420s
OK
```

覆盖了省略 Ollama key、非对象/无效 JSON、HTTP 错误、断连、全 NULL、逐行变化维度、重复/缺失 index、跨批次失败，以及失败后继续执行普通 SQL。

自写 Mock/Python 测试已归档于 `ecac503b1`，并从后续提测版本删除。需要时可从该提交恢复：

```text
unittest/sql/engine/expr/mock_ai_service.py
unittest/sql/engine/expr/test_ai_function_enhancement.py
```

### C++ 单元测试

题目指定路径的两个单测文件已补充并接入 Bazel：

```text
unittest/sql/engine/expr/ob_expr_ai_prompt_test.cpp
unittest/sql/engine/expr/test_ob_openai_utils.cpp
```

最后一次执行现有单测可执行文件：**14 tests / PASSED**。

构建并运行：

```bash
python3 build.py test //unittest/sql/engine/expr:test_ai_function_utils \
  -j3 --action_env=LDFLAGS=-fuse-ld=lld --test_output=errors
```

### Mysqltest 与静态检查

- `ai_prompt.test`：通过；已更新旧的 TEXT/BLOB 拒绝预期。
- `ai_prompt_enhancement.test`：通过；覆盖 JSON Array/Object 参数。
- `git diff --check`：通过。
- `ai_model_ddl.test`：直接单独运行时停在测试框架的 `connect` 命令，缺少 host 参数；不计为通过，没有为绕过它修改代码。
- 未运行完整普通 SQL 回归、Sanitizer 或资源泄漏专项测试。

## 工作区和运行环境

- 原有未跟踪目录 `unittest/scripts/` 未修改、未提交。
- 测试实例使用 `/var/tmp/seekdb-ai-regression`，SQL 端口 `18881`；不用已有业务数据。
- 验证结束后停止本次启动的测试实例。
- 本地 Mock 使用端口 `8000`，由测试启动并关闭。

题目指定路径的 C++ 单测和既有 mysqltest 修改保留；本次清理只移除上述两个自写 Mock/Python 文件。它们不是生产实现依赖。
