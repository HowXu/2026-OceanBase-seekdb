# AI 函数增强：阶段进度与续做记录

更新时间：2026-10-27 08:29（本机时间）

代码基线：`33aab9454`
状态：**六个子题均已有实现改动，六类本地端到端主流程通过；尚未完成最终验收，不代表组委会测试通过或获得 60 分。**

## 1. 当前结论

- 已在 seekdb SQL 表达式和原有 Provider 路径上实现第一轮增强，没有在应用层替代 SQL 功能。
- 当前源码已成功构建为 seekdb 二进制。
- 新增的 Python 回归套件通过真实 SQL 连接测试实例，并使用本地 HTTP Mock；最新一次运行结果为 **6 tests / OK**。
- 仍需补齐 C++ 单元测试、比赛指定的增强 mysqltest、异常响应与网络失败覆盖，并处理旧测试中的过时预期。
- 本报告与代码作为 `wip(ai): implement function enhancements and document handoff` 阶段提交交接；尚未最终验收。用户原有未跟踪目录 `unittest/scripts/` 未修改。

## 2. 六项功能状态

| 子题 | 当前实现 | 已验证内容 | 仍需补齐或复核 |
|---|---|---|---|
| AI_PROMPT 类型与模板 | 接受 JSON Array/Object；保留结构；读取 TEXT/LONGTEXT 的真实内容；递归展开嵌套 Prompt；重复占位符使用可增长缓冲；对数字下标溢出、越界报错 | JSON 参数；重复 10 次占位符；中文嵌套 Prompt；极长非法下标；70 KB LONGTEXT 参数展开为 140 KB 文本 | Unicode/转义和非法占位符完整边界；两种文本类型在模板与参数位置的组合；更新旧测试 |
| AI_EMBED 单行 | NULL 内容传播；显式维度参数检查；返回数量、数组类型、非空维度、一致维度和数值有限性检查；读取文本 LOB | 默认维度、显式 5 维、NULL、非法维度、NULL 元素、维度不匹配 | 更完整的错误响应形状、超大值、空结果和维度边界单测 |
| AI_COMPLETE | 正确处理 NULL/JSON config；支持新 Prompt 渲染；OpenAI/Ollama completion 响应要求非空字符串 | 普通 Prompt、NULL config、JSON config、max_tokens 请求传递、空/NULL 响应拒绝 | temperature 精确断言、长文本直接输入、HTTP 错误、非对象 JSON 根节点、断连等 |
| Ollama Provider | Endpoint 白名单加入 Ollama；免 access key；Provider 大小写不敏感路由；原生 generate/embed；JSON header；stream=false；temperature/max_tokens 转入原生 options | 混合大小写 Provider 修改、清空 key、原生生成和 Embedding | 创建时省略 key 的完整 CRUD 回归；异常响应；config 与原生 options 的兼容边界 |
| AI_RERANK | 字符串和对象输入统一走模型层；按 20 条分批；校验结果数量、index 唯一性/范围、分数合法性；全局 index 还原；按分数降序、原始 index 升序排序 | 47 条跨三批字符串/对象输入；整数分数与并列分数；原始 ID 映射；非法 index 拒绝 | 重复 index、缺失条目、错误字段类型、非有限分数、对象文本字段异常；旧辅助方法路径复核 |
| AI_EMBED 跨行批量 | 新增表达式批量求值；同模型/维度输入每请求最多 64 条；过滤 NULL/skipped 行；保留原始行映射；按响应 index 恢复顺序；结果校验后写回 | 150 行表扫描，含 NULL、WHERE、ORDER BY、LIMIT；显式 4 维；Mock 逆序返回；断言确有多输入请求 | 跨批次失败、重复/缺失响应 index、全 NULL、变化模型/维度、多个执行计划和批量大小 |

## 3. 验证证据

### 构建

```bash
python3 build.py build //:seekdb -j3
```

最近一次构建成功。输出二进制：

```text
build/bin/src/observer/seekdb
```

直接调用 Bazel 曾因缺少 workspace status 导致版本生成失败；应使用仓库的 `build.py`，无需修改构建系统。

### 新增端到端回归

文件：

- `unittest/sql/engine/expr/test_ai_function_enhancement.py`
- `unittest/sql/engine/expr/mock_ai_service.py`

最近一次执行：

```bash
python3 unittest/sql/engine/expr/test_ai_function_enhancement.py
```

结果：

```text
test_complete ... ok
test_embed ... ok
test_embed_batch ... ok
test_ollama ... ok
test_prompt ... ok
test_rerank ... ok
Ran 6 tests in 0.974s
OK
```

这些是本地编写的回归测试，不是组委会原始测试。实现前曾运行并复现六类失败；其中后续修正了测试自身对 completion 字符串输出及 TEXT 最大长度的错误假设。

### Mysqltest

- 新增 `ai_prompt_enhancement.test` 及对应 `r/mysql/ai_prompt_enhancement.result`；此前 JSON 参数回归已通过，后续改动后仍应重跑。
- 既有 `ai_prompt.test` 在只实现 JSON 参数时曾通过。
- 加入 TEXT 支持后，该旧测试最新一次运行在 **第 65 行**失败：测试仍要求 `select ai_prompt(a) from t1` 对 tinyblob 报错，但当前字符串类型判断已允许它。后面 TEXT/LONGTEXT 的旧拒绝预期也需检查。
- **目前不能宣称既有 mysqltest 全部通过。**

### 静态检查

```bash
git diff --check
```

最新检查通过。

## 4. 已修改的生产代码

```text
src/share/ai_service/ob_ai_service_struct.cpp
src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp
src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h
src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp
src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp
src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.h
src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp
src/sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.cpp
```

已有测试修改：

```text
tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test
tools/deploy/mysql_test/test_suite/ai_function/r/mysql/ai_prompt.result
```

新增测试：

```text
tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt_enhancement.test
tools/deploy/mysql_test/test_suite/ai_function/r/mysql/ai_prompt_enhancement.result
unittest/sql/engine/expr/mock_ai_service.py
unittest/sql/engine/expr/test_ai_function_enhancement.py
```

以下题目列出的文件**尚未创建**：

```text
unittest/sql/engine/expr/ob_expr_ai_prompt_test.cpp
unittest/sql/engine/expr/test_ob_openai_utils.cpp
tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test
```

## 5. 已知风险与下一步顺序

1. **先补异常响应回归，再修 HTTP JSON 根节点检查。** 检查发现 `ob_ai_func_client.cpp` 中单请求响应解析后直接将 JSON 根节点转为 `ObJsonObject *`，尚未验证实际类型。非对象响应存在不安全类型转换风险；该文件目前未改，必须补测试并处理。
2. **补 C++ 单元测试并接入 Bazel。** 测试目录原来不存在；可以参考 `unittest/storage/memtable/BUILD.bazel`。SQL 内部头文件目前有模块可见性限制，需要明确测试依赖边界，不要为方便测试扩大所有内部目标的可见性。
3. **完善 Provider 解析边界。** 特别检查 Embedding 的数组/对象类型、缺失/重复 index、Ollama 错误响应；保留其他现有调用方需要的 base64 embedding 能力，避免误伤。
4. **处理旧 AI_PROMPT 类型断言并生成对应结果。** TEXT/LONGTEXT 必须支持；BLOB 的允许范围应明确，避免无意扩展超出题目要求。随后重跑完整 AI 函数 mysqltest。
5. **补跨行批量失败回归。** 验证一个批次失败后不返回错位或残缺结果；覆盖全 NULL、变化模型/维度、过滤、LIMIT 和多批次。
6. **补比赛指定增强 mysqltest与运行说明。** 应使用通用 Mock 协议，不对生产代码硬编码模型、输入或响应。
7. **最后做完整 diff review 与回归验收。** 检查新增路径的资源生命周期、响应类型转换、错误码传播和普通 SQL 回归。不得仅凭当前六个测试认定满足全部评分点。

## 6. 运行环境与恢复命令

本次使用独立测试实例，未使用 `run_temp/store` 数据：

- 测试目录：`/tmp/seekdb-ai-regression`
- SQL 端口：`18881`
- Mock 端口：`8000`
- 数据目录已初始化，可以复用。
- 为暂停工作释放资源，**测试 seekdb 实例已停止**。报告生成时两个端口均无监听。
- Python 套件自行启动、停止 Mock，并创建/清理测试模型、Endpoint 和表；不依赖外部 Python 包。

恢复测试：

```bash
python3 build.py build //:seekdb -j3
build/bin/src/observer/seekdb \
  --base-dir /tmp/seekdb-ai-regression \
  --port 18881 \
  --parameter memory_limit=4G \
  --log-level WARN
# 等待数据库就绪后：
python3 unittest/sql/engine/expr/test_ai_function_enhancement.py
```

客户端：

```text
deps/3rd/u01/obclient/bin/obclient
deps/3rd/u01/obclient/bin/mysqltest
```

Mysqltest 示例：

```bash
deps/3rd/u01/obclient/bin/mysqltest \
  --host=127.0.0.1 --port=18881 --user=root --database=oceanbase \
  --test-file=tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test \
  --result-file=tools/deploy/mysql_test/test_suite/ai_function/r/mysql/ai_prompt.result
```

## 7. 交接摘要

下次可以直接从“**补 HTTP 非对象响应回归 + C++ 单元测试**”继续。无需重新确认六项任务范围，也不要重复从头探索。当前是能构建、主流程可运行的第一轮实现，还不是可提交验收的最终版本。
