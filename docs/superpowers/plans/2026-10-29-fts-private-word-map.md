# Private FTS Word Map Implementation Plan

> **For agentic workers:** Execute inline with executing-plans. The user has selected route A and explicitly requires the current branch/session, without worktrees or subagents.
>
> **Current execution plan:** [FTS next optimization roadmap](2026-10-29-fts-next-optimization-roadmap.md). This file retains the original A design. The newer plan governs the one-pair recheck, evidence/experiment commits, conditional revert, and A.5 → B → C sequencing.

**Goal:** Remove synchronization and duplicate lookups from document-private word-frequency aggregation without changing tokenization or MATCH semantics.

**Architecture:** All five word-map instances are stack-local: ObExprTokenize::tokenize_fulltext, ObDASDomainUtils::generate_fulltext_word_rows, ObJoinOrder::get_query_tokens, the DAS text-retrieval merge iterator and its legacy implementation. The latter three spelled the hash type explicitly and must use the shared alias. Their callees use references synchronously; the map itself never escapes. Select NoPthreadDefendMode for this alias and use the existing set_or_update callback to increment an existing entry after a single lookup. Preserve the existing initial count of 1, collation hash/equality, key ownership, and the non-grouping branch.

**Tech Stack:** C++20, existing OceanBase hash table, SQL regression suites, unchanged official FTS benchmark.

## Global Constraints

- No production changes outside route A; do not change the dictionary loader lock.
- Limit all builds to at most eleven jobs (-j11), per the user's 2026-10-30 update.
- Never modify benchmark/scoring/baseline inputs or run telemetry_test.py.
- Preserve all existing IK/BEng segmentation, stopword and collation behavior.
- Use bc6dfc916 as the immediate performance baseline, not the older Fqin binary.
- Correctness comparisons run before interpreting performance. Benchmark comparisons run without perf.

## Task 1: Capture regression evidence and baseline

Files: unittest/storage/fts_word_map_regression.sql and .result; unittest/storage/fts_parser_regression.sh.

- [ ] Add SQL coverage for repeated-word counts, collation-equivalent words, binary collation, stopwords, empty input, long repeated input, IK modes, indexed MATCH and DML.
- [ ] Build/save the current bc6dfc916 binary in run_temp/route-a-baseline-seekdb.
- [ ] Run the new SQL twice on the unchanged binary, retain identical output as the golden result, and run the existing three SQL suites.
- [ ] Extend the existing parser regression runner to include the new SQL/result pair.

These are behavior-preservation tests and must pass before and after the optimization; no artificial behavioral failure is introduced for a performance-only refactor.

## Task 2: Implement the minimal private-map optimization

Files: src/data_plane/api/data_plane/fts/ob_fts_struct.h; src/storage/fts/ob_fts_stop_word.cpp; src/sql/optimizer/ob_join_order.cpp; src/sql/das/iter/ob_das_text_retrieval_merge_iter.cpp; src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp.

- [ ] Document stack-local ownership and change ObFTWordMap's defense mode to common::hash::NoPthreadDefendMode.
- [ ] In groupby_word, preserve argument validation and non-grouping behavior. In the grouping branch create an inline callback taking common::hash::HashMapPair<ObFTWord, int64_t>&, replace its first member with the current word to preserve the last spelling under collation equality, and increment its second member by word_freq.
- [ ] Call word_map_.set_or_update(word, 1, callback); preserve the existing new-entry frequency of 1.
- [ ] Build seekdb and verify existing/new regression outputs against the baseline.

## Task 3: Measure and retain only justified changes

Files: ignored run_temp artifacts only.

- [ ] Recheck with one complete benchmark pair, baseline versus candidate, using default competition settings; do not automatically run multiple full pairs.
- [ ] Compare all nine scored metrics, hit counts and category-weighted timing improvement. Report regressions and noise rather than selecting a best run.
- [ ] Inspect the final diff and error/resource-lifetime paths. Keep benchmark/scoring and global dictionary synchronization unchanged.
- [ ] Follow the newer roadmap: commit baseline-compatible regression tests and measurement evidence separately from the A production experiment. If benefit is near zero, negative, or unproven, commit and then revert the A production experiment while retaining evidence/tests. Do not push without authorization.
