# TOKENIZE Default-Properties Memo Hit Implementation Plan

> **For agentic workers:** Execute inline using superpowers:executing-plans. Do not create worktrees or launch subagents.

**Goal:** Remove repeated parser-property normalization and IK dictionary-loader lookup from TOKENIZE result-memo hits without changing the original competition benchmark or parser semantics.

**Architecture:** For calls with fewer than three arguments, form the existing result memo key with the trimmed raw parser name and empty, implicit-default properties before normalization. On a miss, validate/canonicalize the built-in parser name, normalize defaults and initialize dictionaries before tokenizing; publish using the pre-normalization key. Explicit three-argument calls retain name validation, normalization and dictionary initialization before memo lookup. An `implicit_defaults_` key discriminator separates these namespaces. Text/collation evaluation remains on every call.

**Tech Stack:** C++, existing thread-local TOKENIZE result memo, release seekdb build, untouched shell benchmark.

## Global Constraints

- Baseline is `715e660a9`, binary `run_temp/fts-dense-topk-v3-candidate-seekdb`.
- Work on `2026-competition-fts-clean-base`; preserve concurrent changes.
- Build: `cmake --build build_release --target seekdb -j8`.
- No changes to benchmark scripts, generators, fixed baseline, scoring, inputs, defaults, parser rules, dictionaries, properties, result representation, MATCH scores, thresholds or LIMIT behavior.
- No new MATCH caching. TOKENIZE memo retains stable owned key/result storage and current-allocator result copies.
- Acceptance is the unmodified original benchmark. Do not introduce a separate score-sequence acceptance gate.
- Fresh benchmark instances retain the existing 2G memory / 1G initial datafile / 4G max datafile / 2G log disk settings.
- No telemetry tests or pushes.

## Task 1: Record baseline evidence

- [x] Verify clean worktree and baseline binary SHA256.
- [x] Run `bash run_temp/run-fts-score-route.sh tokenize-hitpath-ab1-baseline run_temp/fts-dense-topk-v3-candidate-seekdb`.
- [x] Keep the full report as the pre-change performance reference. Existing source establishes the undesired behavior: `parse_param()` invokes `reform_parser_properties()` and `try_load_dictionary_for_ik()` before every memo lookup, including hits.

## Task 2: Defer implicit-default setup until a miss

**Modify:** `src/sql/engine/expr/ob_expr_tokenize.cpp` only.

- [x] In `parse_param()`, keep explicit-property handling unchanged by guarding the final two calls:

```cpp
  } else if (expr.arg_cnt_ >= 3 && OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (expr.arg_cnt_ >= 3 && OB_FAIL(param.try_load_dictionary_for_ik())) {
```

- [x] In `eval_tokenize()`, after the memo-hit branch and before `tokenize_fulltext()`, add:

```cpp
    } else if (expr.arg_cnt_ < 3 && OB_FAIL(construct_ft_parser_inner_name(param.parser_name_, param))) {
    } else if (expr.arg_cnt_ < 3 && OB_FAIL(param.reform_parser_properties(param.properties_))) {
    } else if (expr.arg_cnt_ < 3 && OB_FAIL(param.try_load_dictionary_for_ik())) {
```

- [x] Add a short comment explaining that the memo key for implicit defaults is captured before normalization; successful misses perform initialization before publishing.
- [x] After v1 showed no stable gain, defer default parser-name canonicalization too: `parse_parser_name()` assigns `raw_parser_name` when `expr.arg_cnt_ < 3`; `eval_tokenize()` validates it on misses. Add and compare `TokenizeMemoKey::implicit_defaults_`, initialized from `expr.arg_cnt_ < 3`.
- [x] Run `git diff --check` and the required release build. Preserve the v2 candidate as `run_temp/fts-tokenize-hitpath-v2-candidate-seekdb` and record SHA256.

## Task 3: Original-benchmark acceptance and retention

- [x] Complete five full v1 pairs and ten full v2 A/B pairs; alternate candidate-first on even pairs. No rounds are discarded.
- [x] Verify default configurations and hits 8001/11000/7332/20 in every report. Report all nine scoring metrics, total build time, category-weighted paired reductions, and variation.
- [x] Profile the actual baseline and candidate with the original benchmark. Use `perf record --clockid mono` so sample timestamps align with Python monotonic benchmark markers.
- [x] Reject v1, retain v2: ten-pair TOKENIZE category reduction median +7.8555%, equal-category composite median +2.5676%, mean +2.0634%. Query category regresses and is included, not excluded.
- [x] Record evidence in `docs/superpowers/experiments/2026-10-30-fts-tokenize-hit-path-results.md`. User authorized a local commit after results; do not push or alter existing commits.
