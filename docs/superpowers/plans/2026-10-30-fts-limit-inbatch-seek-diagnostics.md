# LIMIT 批内定位：诊断与单候选选择 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline，逐项执行并在诊断结论处停下检查。用户要求当前会话单线执行，不调用子代理、不创建 worktree。Steps use checkbox (`- [ ]`) syntax for tracking。

**Goal:** 保持原线性推进，取得批内跳距和精确比较次数模型，证明适用条件后冻结一个 C3 候选，或有证据地停止该路线。

**Architecture:** 先在固定 A+B 程序上冻结扩展 SQL golden，再临时给 token iterator 加私有整数统计；原比较器及索引推进不变。用经过独立穷举校验的 n/d 模型累计六个固定算法组合的比较次数；仅执行一次诊断 benchmark。此计划的交付物是诊断结论及所选算法/常量，不提前实现两个生产算法。

**Tech Stack:** seekdb C++20、DAAT/BMW、ObDatum/ObDocIdExt、Python 3、SQL 回归、现有直接启动脚本和原 benchmark。

## Global Constraints

- 已确认设计：`docs/superpowers/specs/2026-10-30-fts-limit-inbatch-seek-design.md`，文档提交 `272c42058`。
- 分支：`2026-competition-fts-clean-base`；不查看其他分支，不创建 worktree，不调用子代理，不 push。
- 生产基线为 A+B，源码同 `964f20c92`；不恢复 C1、C2，不自动转入 C1b。
- 固定基线：`run_temp/fts-clean-base-final-seekdb`，SHA256 `6a97b9171f4774b8a7a460a734a59b5c7295e5ed1ec33858ee6714d567c0ca7c`。
- `build_release/src/observer/seekdb` 当前可能仍是 C2 取消前构建；必须重建实际源码并另存命名，不充当基线。
- 编译最多 `-j11`；benchmark 运行期间不编译、不挂 profiler。
- 配置原样：`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。
- 实例：端口 12883，memory_budget=2G、datafile_size=1G、datafile_maxsize=4G、log_disk_size=2G；直接启动，不走 OBD。
- 不修改 benchmark、评分脚本、历史 baseline、输入规模；不执行 `telemetry_test.py`。
- 保留比较器、doc-ID 深拷贝、score 槽位对应、BM25、过滤、BMW pivot/threshold 及原换批/EOF/错误流程。
- 本阶段只有一次诊断 benchmark，不是性能 A/B，不据其耗时宣称收益；采集失败停止报告，不静默重复。
- 最佳比较次数减少不足 20% 或安全条件未证明则停止；优先简单方案的 5% 规则和并列选短前缀规则不可事后调整。
- 后续生产候选最多一对完整 A/B；保留门槛：六组回归一致，LIMIT ≥5%，查询类别 ≥0%，每条 COUNT ≥-2%。本计划不启动该 A/B。

## 文件边界与交付接口

| 路径 | 责任 |
|---|---|
| `unittest/storage/fts_query_batch_regression.sql/.result` | 永久行为回归与基线冻结 |
| `src/storage/retrieval/ob_text_retrieval_token_iter.h` | 临时私有诊断状态、校验/输出方法声明 |
| `src/storage/retrieval/ob_text_retrieval_token_iter.cpp` | 临时模型、观察、批次验证及原算法插桩；最后全部移除 |
| `run_temp/test-c3-model.py` | 临时独立模型校验，不代替实际 C3 正确性测试 |
| `run_temp/run-c3-diagnostic.sh` | 原启动/benchmark 包装器的诊断副本，排除 SQL 回归对统计的污染 |
| `run_temp/query-c3-*` | 二进制、日志、分布和选择文件 |
| `docs/superpowers/experiments/2026-10-30-fts-query-c3-diagnostics.md` | 适用条件、诊断结果、选择/停止决定和命令证据 |

不新增生产头文件，不修改 BMW、存储扫描/解码实现或通用表达式。永久正确性覆盖仍放现有 unittest/storage；临时诊断辅助脚本不放 tools/。

最终机器可读选择文件 `run_temp/query-c3-selection.json` 必须包含：

```json
{
  "safe_scope_proven": false,
  "baseline_comparisons": 0,
  "model_totals": {},
  "decision": "stop",
  "algorithm": null,
  "prefix": null,
  "reduction": null
}
```

上述是初始化状态，不是测量结果；任务完成时填入真实值。decision 仅为 stop 或 selected；algorithm 仅为 A、B 或 null。任何模型零分母、诊断错误或缺失调用都不能生成 selected。

---

## Task 1：冻结 SQL 行为及实际路径证据

**Files:** 修改 `unittest/storage/fts_query_batch_regression.sql/.result`；临时计划输出保存 `run_temp/query-c3-plan.tsv`。

**Interfaces:** 输入固定 A+B 二进制；输出两次完全一致的 golden、覆盖用例及 BMW/目标 iterator 的路径证据。此时不能改生产代码。

- [ ] **Step 1：核对分支、工作区、程序及既有测试。**

```bash
git status --short --branch
git diff --exit-code ae5c8f0df -- src/ tools/benchmark/
sha256sum run_temp/fts-clean-base-final-seekdb
bash -n run_temp/run-fts-sql.sh run_temp/run-fts-variant.sh unittest/storage/fts_parser_regression.sh
```

预期源码/benchmark diff 为空，固定哈希匹配。若出现用户改动，不覆盖，不用 git restore 清理未知内容。

- [ ] **Step 2：在现有查询表 DML 之前增加独立稠密/稀疏 token 表及查询。**

下面整段插在 `update fts_query_batch set title=` 之前，原 2051 文档用例全部保留。用 token 分布制造 posting 密度差异，不以业务 ID 值推断内部 doc-ID。

```sql
drop table if exists fts_query_seek;
create table fts_query_seek(id int primary key, en text) default charset=utf8mb4;
insert into fts_query_seek
select id, concat('denseword ',
  if(id % 2 = 0, 'evenword ', ''),
  if(id % 31 = 0, 'sparseword ', ''),
  if(id % 257 = 0, 'rareword rareword ', ''),
  if(id % 7 = 0, 'rankword rankword rankword', 'rankword'))
from fts_query_batch;
alter table fts_query_seek add fulltext index ft_seek(en) with parser beng;
select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 1;
select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 20;
select id, round(match(en) against('denseword evenword sparseword rareword'), 8) as score
from fts_query_seek where match(en) against('denseword evenword sparseword rareword') > 0
order by score desc, id limit 7 offset 31;
select group_concat(concat(id, ':', score) order by score desc, id)
from (select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 1027) r;
prepare fts_seek_reuse from 'select id, round(match(en) against(?), 8) as score from fts_query_seek where match(en) against(?) order by score desc, id limit 5';
set @seek_words='denseword sparseword';
execute fts_seek_reuse using @seek_words, @seek_words;
set @seek_words='no_such_seek_word';
execute fts_seek_reuse using @seek_words, @seek_words;
deallocate prepare fts_seek_reuse;
delete from fts_query_seek where id in (0, 31, 1024, 2048);
update fts_query_seek set en='denseword rareword rareword rankword' where id=1025;
select id, round(match(en) against('denseword sparseword rareword rankword'), 8) as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 20;
drop table fts_query_seek;
```

`group_concat_max_len` 沿用现有 1048576，必须检查无截断；不要因输出较长改成摘要。

- [ ] **Step 3：两次捕获基线，再冻结 golden。**

```bash
bash run_temp/run-fts-sql.sh query-c3-golden-1 run_temp/fts-clean-base-final-seekdb unittest/storage/fts_query_batch_regression.sql
bash run_temp/run-fts-sql.sh query-c3-golden-2 run_temp/fts-clean-base-final-seekdb unittest/storage/fts_query_batch_regression.sql
cmp run_temp/query-c3-golden-1.tsv run_temp/query-c3-golden-2.tsv
cp run_temp/query-c3-golden-1.tsv unittest/storage/fts_query_batch_regression.result
git diff --check
```

预期 cmp 成功；失败则停止调查，不更新成候选输出。

- [ ] **Step 4：单独保存 LIMIT 查询的 EXPLAIN，不加入 golden。**

建立临时 SQL：复制上述建表/插入/索引语句，加以下语句，最后 drop table；用固定基线与现有 run-fts-sql 执行保存输出。

```sql
explain select id, match(en) against('denseword sparseword rareword rankword') as score
from fts_query_seek where match(en) against('denseword sparseword rareword rankword')
order by score desc, id limit 20;
```

随后诊断还须确认实际 advance 调用，EXPLAIN 本身不是跳距证明。若该语句不走目标路径，记录事实；调整测试查询必须在改算法前重新两次冻结，不能改 benchmark 查询。机制确认失败且无法在现有路径内解决时停止。

- [ ] **Step 5：独立提交测试。**

```bash
git add unittest/storage/fts_query_batch_regression.sql unittest/storage/fts_query_batch_regression.result
git diff --cached --check
git commit -m "test(fts): cover dense and sparse LIMIT posting traversal"
```

## Task 2：适用条件审计与临时模型

**Files:** 临时修改 `src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}`；创建 `run_temp/test-c3-model.py`。

**Interfaces:** 临时纯函数 `static int64_t c3_model_count(int64_t n, int64_t d, int64_t prefix, bool gallop)`，输入合法 `0<=d<=n`，返回批内比较次数。A 为 gallop=false，B 为 true；不访问 datum。

- [ ] **Step 1：审计顺序、skip 和生命周期，先记录证据。**

使用 codegraph 聚焦扫描参数构造、scan_order、token skip 及 eval_batch 参数契约。列出源码路径/函数，证明可用范围；特别注意 `get_skip()` 返回可变指针，不能只搜索成员名。追踪实际使用方及可能修改 bit vector 的求值函数。记录 target 的调用期所有权及批次换新时机。

若仍不能证明安全范围，本路线到此停止；将后续插桩/诊断任务标记跳过，转 Task 5 完成清理、基线验证与停止记录。运行时一次成功的有序检查不能替代源码契约。不要为绕过问题修改 skip 处理或存储扫描。

审计通过后将实际源码路径、函数、行号及适用范围写入 `run_temp/query-c3-safe-scope.txt`，并在实验记录引用；未通过不得创建该文件。

- [ ] **Step 2：将下列纯函数临时加入 cpp 命名空间内。**

```cpp
// C3_MODEL_BEGIN
static int64_t c3_model_count(
    const int64_t n, const int64_t d, const int64_t prefix, const bool gallop)
{
  int64_t comparisons = 0;
  int64_t lo = 0;
  for (; lo < n && lo < prefix; ++lo) {
    ++comparisons;
    if (lo >= d) {
      return comparisons;
    }
  }
  int64_t hi = n;
  if (gallop && lo < n) {
    const int64_t base = lo;
    const int64_t remaining = n - base;
    int64_t width = 1;
    while (lo < n) {
      const int64_t probe = base + width - 1;
      ++comparisons;
      if (probe >= d) {
        hi = probe; // probe is known to satisfy; do not compare it twice.
        break;
      }
      lo = probe + 1;
      if (lo == n) {
        return comparisons;
      }
      width = width > remaining / 2 ? remaining : width * 2;
    }
  }
  while (lo < hi) {
    const int64_t mid = lo + (hi - lo) / 2;
    ++comparisons;
    if (mid >= d) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return comparisons;
}
// C3_MODEL_END
```

前缀包含当前项；模型无末项快捷检查。后续若选中方案 B，正式算法必须复用此探测/夹定规则；不能在候选中再增加未建模的比较。

- [ ] **Step 3：先独立测试模型代码，而非完整编译 seekdb。**

创建以下临时脚本，直接提取同一 C++ 函数编译，Python 独立构造数组并实际执行查找作参考。此测试只验证比较次数模型，不声称验证生产 C3。

```python
import bisect
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path('src/storage/retrieval/ob_text_retrieval_token_iter.cpp').read_text()
model = source.split('// C3_MODEL_BEGIN\n', 1)[1].split('// C3_MODEL_END', 1)[0]

def reference(n, d, prefix, gallop):
    values = [2 * i for i in range(n)]
    target = 2 * d - 1
    calls = 0
    def accepts(i):
        nonlocal calls
        calls += 1
        return values[i] >= target
    start = 0
    while start < min(prefix, n):
        if accepts(start):
            return start, calls
        start += 1
    end = n
    if gallop and start < n:
        anchor = start
        span = 1
        while True:
            probe = min(anchor + span, n) - 1
            if accepts(probe):
                end = probe
                break
            start = probe + 1
            if start == n:
                return start, calls
            span *= 2
    while start != end:
        middle = (start + end) // 2
        if accepts(middle):
            end = middle
        else:
            start = middle + 1
    assert start == bisect.bisect_left(values, target)
    return start, calls

sizes = list(range(65)) + [127, 128, 129, 255, 256, 257, 1024]
with tempfile.TemporaryDirectory(dir='run_temp') as tmp:
    root = Path(tmp)
    cpp = root / 'model.cpp'
    binary = root / 'model'
    cpp.write_text('#include <cstdint>\n#include <iostream>\n' + model + r'''
int main() {
  int64_t n, d, p, b;
  while (std::cin >> n >> d >> p >> b) {
    std::cout << c3_model_count(n, d, p, b != 0) << '\n';
  }
}
''')
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)
    ], check=True)
    cases = [(n, d, p, b) for n in sizes for d in range(n + 1)
             for p in (1, 2, 4) for b in (0, 1)]
    payload = ''.join(f'{n} {d} {p} {b}\n' for n, d, p, b in cases)
    result = subprocess.run([str(binary)], input=payload, text=True,
                            capture_output=True, check=True)
    actual = [int(v) for v in result.stdout.splitlines()]
    assert len(actual) == len(cases)
    for (n, d, p, b), count in zip(cases, actual):
        position, expected = reference(n, d, p, bool(b))
        assert position == d, (n, d, p, b, position)
        assert count == expected, (n, d, p, b, count, expected)
print('C3_MODEL_TEST: PASS')
```

运行 `python3 run_temp/test-c3-model.py > run_temp/query-c3-model-test.log 2>&1`，预期最后输出 `C3_MODEL_TEST: PASS`。模型失败时不进入 benchmark，先定位错误，不用缩小用例范围使其通过。

## Task 3：只观察原搜索与批次，不改变推进

**Files:** 同一对 token-iterator 文件，所有本任务代码为临时诊断，结束时全部撤销。

**Interfaces:** iterator-private 统计，初始化全零；`record_segment(n,d,calls,eligible)`、`record_fixed_comparison()`、`flush_c3_diag()`；所有统计不得改变原 ret、索引、datum 或 score。

- [ ] **Step 1：添加以下临时私有状态及观察函数。**

在头文件的 `ObTextRetrievalDaaTTokenIter` private 区域添加状态，数组下标 0..2 为 A1/A2/A4，3..5 为 B1/B2/B4。

```cpp
struct C3SeekDiag {
  int64_t comparisons = 0;
  int64_t model[6] = {};
  int64_t histogram[8] = {};
  int64_t segments = 0;
  int64_t exhausted = 0;
  int64_t storage_advances = 0;
  int64_t already_exhausted = 0;
  int64_t scalar_or_single = 0;
  int64_t errors = 0;
  int64_t eof = 0;
  int64_t batches = 0;
  int64_t holes = 0;
  int64_t inversions = 0;
  int64_t validation_errors = 0;
};
C3SeekDiag c3_diag_;
bool c3_batch_valid_ = false;
void record_segment(int64_t n, int64_t d, int64_t calls, bool eligible);
void record_fixed_comparison();
void validate_c3_batch();
void flush_c3_diag();
```

cpp 实现：

```cpp
void ObTextRetrievalDaaTTokenIter::record_segment(
    const int64_t n, const int64_t d, const int64_t calls, const bool eligible)
{
  ++c3_diag_.segments;
  c3_diag_.comparisons += calls;
  int64_t bucket = d < 2 ? d : 2;
  if (d >= 2) {
    for (int64_t bound = 4; bucket < 7 && d >= bound; bound *= 2) {
      ++bucket;
    }
  }
  ++c3_diag_.histogram[bucket];
  if (d == n) { ++c3_diag_.exhausted; }
  const int64_t prefixes[] = {1, 2, 4};
  for (int64_t i = 0; i < 6; ++i) {
    c3_diag_.model[i] += eligible
        ? c3_model_count(n, d, prefixes[i % 3], i >= 3) : calls;
  }
}

void ObTextRetrievalDaaTTokenIter::record_fixed_comparison()
{
  ++c3_diag_.comparisons;
  for (int64_t i = 0; i < 6; ++i) { ++c3_diag_.model[i]; }
}

void ObTextRetrievalDaaTTokenIter::validate_c3_batch()
{
  c3_batch_valid_ = false;
  if (!eval_ctx_->is_vectorized() || count_ <= 1) { return; }
  ++c3_diag_.batches;
  bool valid = true;
  const auto *skip = token_iter_->get_skip();
  for (int64_t i = 0; i < count_; ++i) {
    if (nullptr == skip || skip->at(i)) {
      ++c3_diag_.holes;
      valid = false;
    }
  }
  if (!valid) { return; }
  for (int64_t i = 1; i < count_; ++i) {
    int result = 0;
    const int err = cmp_func_(doc_id_[i - 1].get_datum(),
                              doc_id_[i].get_datum(), result, nullptr);
    if (OB_SUCCESS != err) {
      ++c3_diag_.validation_errors;
      valid = false;
    } else if (result > 0) {
      ++c3_diag_.inversions;
      valid = false;
    }
  }
  c3_batch_valid_ = valid;
}
```

在 `get_next_row()` 现有 `if (OB_SUCC(ret) && need_load)` 的保存逻辑末尾加下面代码，只观察已成功深拷贝的批次：

```cpp
    if (OB_SUCC(ret)) {
      validate_c3_batch();
    }
```

在本 iterator 的 reuse/reset 函数开头分别加入以下代码，先输出旧计数再置零，不改变原 token_iter 的 reuse/reset 顺序：

```cpp
  flush_c3_diag();
  c3_batch_valid_ = false;
```

输出函数在 cpp 临时包含 `<sstream>` 与 `<cstdio>`：

```cpp
void ObTextRetrievalDaaTTokenIter::flush_c3_diag()
{
  if (c3_diag_.segments || c3_diag_.batches || c3_diag_.storage_advances
      || c3_diag_.errors || c3_diag_.eof || c3_diag_.already_exhausted
      || c3_diag_.scalar_or_single) {
    std::ostringstream out;
    out << "C3_DIAG comparisons=" << c3_diag_.comparisons << " model=";
    for (int i = 0; i < 6; ++i) { out << (i ? "," : "") << c3_diag_.model[i]; }
    out << " histogram=";
    for (int i = 0; i < 8; ++i) { out << (i ? "," : "") << c3_diag_.histogram[i]; }
    out << " segments=" << c3_diag_.segments
        << " exhausted=" << c3_diag_.exhausted
        << " storage_advances=" << c3_diag_.storage_advances
        << " already_exhausted=" << c3_diag_.already_exhausted
        << " scalar_or_single=" << c3_diag_.scalar_or_single
        << " errors=" << c3_diag_.errors << " eof=" << c3_diag_.eof
        << " batches=" << c3_diag_.batches << " holes=" << c3_diag_.holes
        << " inversions=" << c3_diag_.inversions
        << " validation_errors=" << c3_diag_.validation_errors;
    const std::string line = out.str();
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  c3_diag_ = C3SeekDiag{};
}
```

不输出每个 doc-ID、target、token 或每次比较日志；一条完整 fprintf 减少多线程输出交错。

- [ ] **Step 2：将原 advance_to 临时替换为以下等价观察版。**

不在本阶段调用新搜索算法。首段用模型计数，存储推进后首项检查是两种方案共有成本，逐次计入所有模型。

```cpp
int ObTextRetrievalDaaTTokenIter::advance_to(const ObDatum &id_datum)
{
  int ret = OB_SUCCESS;
  int result = 0;
  bool find = false;
  const int64_t start = cur_idx_;
  const int64_t n = cur_idx_ >= 0 && cur_idx_ < count_ ? count_ - cur_idx_ : 0;
  int64_t calls = 0;
  const bool eligible = c3_batch_valid_ && eval_ctx_->is_vectorized() && count_ > 1;
  bool pending = n > 0;
  if (!eval_ctx_->is_vectorized() || count_ <= 1) { ++c3_diag_.scalar_or_single; }
  if (cur_idx_ < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (cur_idx_ >= count_) {
    ++c3_diag_.already_exhausted;
  }
  while (OB_SUCC(ret) && !find) {
    if (cur_idx_ < count_) {
      ++calls;
      if (OB_FAIL(cmp_func_(id_datum, doc_id_[cur_idx_].get_datum(), result, nullptr))) {
        if (pending) { record_segment(n, cur_idx_ - start, calls, false); pending = false; }
      } else if (result <= 0) {
        find = true;
        if (pending) { record_segment(n, cur_idx_ - start, calls, eligible); pending = false; }
      } else {
        ++cur_idx_;
        if (pending && cur_idx_ == count_) {
          record_segment(n, n, calls, eligible);
          pending = false;
        }
      }
    } else {
      ++c3_diag_.storage_advances;
      if (OB_FAIL(token_iter_->advance_to(id_datum))) {
      } else if (OB_FAIL(get_next_row())) {
        if (OB_ITER_END == ret) { find = true; }
      } else if (cur_idx_ != 0) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        record_fixed_comparison();
        if (OB_FAIL(cmp_func_(id_datum, doc_id_[cur_idx_].get_datum(), result, nullptr))) {
        } else if (result <= 0) {
          find = true;
        } else {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
  }
  if (OB_ITER_END == ret) { ++c3_diag_.eof; }
  else if (OB_SUCCESS != ret) { ++c3_diag_.errors; }
  return ret;
}
```

对照原函数逐分支检查：只增加计数和只读校验，原比较/推进调用次数和 ret 不变。若有分支并非等价，先修诊断，不迁就回归结果。

- [ ] **Step 3：构建诊断程序，运行 focused SQL 和六组回归。**

```bash
git diff --check
cmake --build build_release --target seekdb -j11 > run_temp/query-c3-diag-build.log 2>&1
cp --reflink=auto build_release/src/observer/seekdb run_temp/query-c3-diag-seekdb
sha256sum run_temp/query-c3-diag-seekdb > run_temp/query-c3-diag-binary.sha256
bash run_temp/run-fts-sql.sh query-c3-diag-regressions run_temp/query-c3-diag-seekdb --regressions
```

预期六组 PASS。若 golden 不符，停止诊断 benchmark；不要以它只是插桩为由忽略。检查 focused 用例有 advance 调用及无 holes/inversions/validation_errors。统计必须能够归集到结束输出；若对象 reset 未覆盖执行生命周期，应补证据后停止报告，不静默重新采集 benchmark。

## Task 4：一次诊断、聚合与选择

**Files:** 创建临时 `run_temp/run-c3-diagnostic.sh`、`run_temp/query-c3-selection.json` 和实验记录。

**Interfaces:** 输入诊断程序、已校验模型和安全审计；输出冻结的 A/B+prefix 或 stop。本任务不创建任何正式生产候选。

- [ ] **Step 1：创建只运行原 benchmark 的包装器，避免回归调用污染统计。**

只从已有启动包装器去掉回归部分，不改其 benchmark 调用、规模或生命周期处理：

```python
from pathlib import Path
s = Path('run_temp/run-fts-variant.sh').read_text()
a = s.index("$client -e 'CREATE DATABASE IF NOT EXISTS fts_regression'")
b = s.index('MYSQL="$client -N -s -A"')
assert a < b
s = s[:a] + s[b:]
s = s.replace('benchmark and all regressions passed',
              'diagnostic benchmark finished (not acceptance)')
Path('run_temp/run-c3-diagnostic.sh').write_text(s)
```

运行 `bash -n run_temp/run-c3-diagnostic.sh`。实例根目录为 `run_temp/query-c3-diag-instance`，console 为 `run_temp/query-c3-diag-console.txt`，与 focused 回归进程分开。

- [ ] **Step 2：仅运行一次诊断 benchmark。**

```bash
bash run_temp/run-c3-diagnostic.sh query-c3-diag run_temp/query-c3-diag-seekdb
```

确认没有编译或 perf 进程并行；命令失败停止，不重跑。只验证固定配置、hits 和原始样本完整，不将耗时计为收益。诊断 stderr 含 C3_DIAG 行，原 benchmark.log 仍保留九项原始输出。

- [ ] **Step 3：解析统计、验证完整性、应用固定规则。**

用下面脚本聚合 console；`safe_scope_proven` 只有 Task 2 审计完成并将证据写入实验文档后才能设为 true。不要根据 runtime observations 自动将它设为 true。

```python
import json
import re
from pathlib import Path

root = Path('run_temp')
rows = []
for line in (root/'query-c3-diag-console.txt').read_text().splitlines():
    if not line.startswith('C3_DIAG '):
        continue
    row = {}
    for key, value in re.findall(r'(\w+)=([0-9,]+)', line):
        row[key] = [int(x) for x in value.split(',')] if ',' in value else int(value)
    assert len(row['model']) == 6 and len(row['histogram']) == 8
    assert sum(row['histogram']) == row['segments']
    rows.append(row)
assert rows, 'missing diagnostic iterator output'
keys = ['comparisons','segments','exhausted','storage_advances','already_exhausted',
        'scalar_or_single','errors','eof','batches','holes','inversions','validation_errors']
totals = {k: sum(row[k] for row in rows) for k in keys}
models = [sum(row['model'][i] for row in rows) for i in range(6)]
hist = [sum(row['histogram'][i] for row in rows) for i in range(8)]
names = ['A1','A2','A4','B1','B2','B4']
# This file is written only after the source-contract audit, with cited evidence.
safe = (root/'query-c3-safe-scope.txt').is_file()
valid = safe and totals['comparisons'] > 0 and totals['segments'] > 0 \
        and totals['batches'] > 0 and totals['storage_advances'] > 0 \
        and all(totals[k] == 0 for k in ['errors','holes','inversions','validation_errors'])
best = min(range(6), key=lambda i: (models[i], i % 3, i // 3))
best_a = min(range(3), key=lambda i: (models[i], i))
base = totals['comparisons']
global_qualifies = base > 0 and 5 * (base - models[best]) >= base
a_qualifies = base > 0 and 5 * (base - models[best_a]) >= base
if global_qualifies and a_qualifies and 100 * models[best_a] <= 105 * models[best]:
    best = best_a
reduction = 1 - models[best] / base if base else None
selected = valid and global_qualifies and 5 * (base - models[best]) >= base
result = {
    'safe_scope_proven': safe,
    'baseline_comparisons': totals['comparisons'],
    'model_totals': dict(zip(names, models)),
    'decision': 'selected' if selected else 'stop',
    'algorithm': names[best][0] if selected else None,
    'prefix': int(names[best][1:]) if selected else None,
    'reduction': reduction,
    'totals': totals,
    'histogram': hist,
}
(root/'query-c3-selection.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
```

若零 holes 无法在源码层证明，只能 stop。受支持范围内若确有少量资格外调用，应保留其原比较成本；未确认 scope 不得仅通过过滤统计行来过门槛。

脚本用整数验证门槛，避免恰好 20% 或 5% 时的浮点边界误判。先验证全局最佳至少减少 20%，再考虑 A 的 5% 简化规则；若 A 本身不足 20%，仍选原先过线的全局最佳，不降低门槛。

- [ ] **Step 4：写实验记录并冻结结论。**

记录必须包含：源码审计/EXPLAIN/实际调用证据、模型测试、二进制哈希、一次诊断命令和配置、八个跳距桶、原比较总数、六模型总数、固定共有成本、是否有错误和为什么选择/停止。全部九项诊断耗时可保留，但明确不是验收成绩。

若 selected，后续实施计划消费 selection.json 的 algorithm/prefix、固定 probe 规则及安全范围，不再搜索更多常量。若 stop，说明安全条件或 20% 条件为何不足；不自动追加诊断、A/B 或 C1b。

## Task 5：清理诊断、验证真实基线、停在候选实施检查点

**Files:** 删除本轮 token-iterator 临时改动；保留 SQL 回归和实验记录。

**Interfaces:** 输出干净的 A+B 生产源码、实际重建的 A+B 程序、六组回归结果，以及可供下一阶段使用的冻结诊断记录。

- [ ] **Step 1：保存诊断补丁，再逐项移除本轮临时代码。**

```bash
git diff -- src/storage/retrieval/ob_text_retrieval_token_iter.h src/storage/retrieval/ob_text_retrieval_token_iter.cpp > run_temp/query-c3-diagnostics.patch
```

只删除本计划添加的字段、模型、校验、日志和插桩；恢复原 advance_to。无并发用户改动时可针对这两个文件恢复到本轮开始的生产源码；有未知改动则使用精确 edit，不能 git restore 整个 src。

- [ ] **Step 2：核对生产 diff、重建真正的 A+B 并跑回归。**

```bash
git diff --exit-code ae5c8f0df -- src/ tools/benchmark/
git diff --check
cmake --build build_release --target seekdb -j11 > run_temp/query-c3-restored-build.log 2>&1
cp --reflink=auto build_release/src/observer/seekdb run_temp/query-c3-restored-ab-seekdb
sha256sum run_temp/query-c3-restored-ab-seekdb > run_temp/query-c3-restored-binary.sha256
bash run_temp/run-fts-sql.sh query-c3-restored-regressions run_temp/query-c3-restored-ab-seekdb --regressions
```

重建哈希可以因构建元信息不同而变化，不能谎称必然等于固定基线；以源码 diff、构建日志及回归证明状态。固定基线仍保留，不覆盖。

- [ ] **Step 3：检查无临时残留、进程及 benchmark 修改。**

```bash
if rg -n 'C3_DIAG|C3_MODEL_BEGIN|c3_diag_|c3_model_count|validate_c3_batch' src/storage/retrieval/ob_text_retrieval_token_iter.{h,cpp}; then
  printf 'unexpected C3 diagnostic residue\n' >&2
  exit 1
fi
git diff --exit-code ae5c8f0df -- src/ tools/benchmark/
git diff --check
git status --short --branch
```

检查诊断/测试服务与编译进程已结束。不要删除原始日志和模型测试证据。

- [ ] **Step 4：单独提交诊断证据，汇报后停止。**

```bash
git add docs/superpowers/experiments/2026-10-30-fts-query-c3-diagnostics.md
git diff --cached --check
git commit -m "docs(fts): record LIMIT in-batch seek diagnostic decision"
```

本计划结束时没有 C3 生产优化提交、没有正式 A/B。若 selected，下一步基于固定算法和 prefix 写单候选实现计划，包含同一实际查找实现的受控数组/线性 oracle 测试、六组回归、唯一 A/B、独立生产提交与必要 revert，以及最终 HEAD 重建；不得用本阶段 Python 模型测试替代实际实现测试。该计划必须携带已批准的 LIMIT ≥5%、query ≥0%、每条 COUNT ≥-2% 门槛。

## 计划自检

- 本计划完整覆盖设计的阶段 D、SQL golden 冻结、一次诊断、选择/停止及插桩清理；阶段 C3 依赖尚未取得的数据，因此明确分为下一份单候选计划，不提前实现两套生产算法。
- 所有 benchmark 命令使用原配置；包装器只排除 SQL 回归调用，不改 benchmark。
- 错误/标量/资格外比较按原成本计入模型，共有换批首项比较计入六种方案，不放大预测收益。
- 源码契约是 safe_scope 的必要条件；运行时 observations 和零错误本身不能作普遍证明。
- 本轮只编写和自检文档，尚未启动本文的构建、插桩、测试或 benchmark。
