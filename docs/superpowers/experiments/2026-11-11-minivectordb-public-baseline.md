# MiniVectorDB local public baseline — 2026-11-11

Actual execution date: 2026-11-11 (+08:00), directly in
`/home/howxu/Projects/2026-OceanBase-seekdb`, `kernel/round2`, HEAD
`f4d4e26653af823a7601668802f80e374ad6643c` plus uncommitted implementation.
This is a NEW genuine experiment, not a replacement for the existing experiment.

## Outcome boundaries

| Check | Actual result | Not established |
|---|---|---|
| LOCAL original public driver | **20/20 PASS, 95 SQL**, no SKIPPED | Hidden/formal admission |
| LOCAL original handshake | PASS | Container linkage |
| Fresh Debug CTest | 10/10 PASS, exit 0 | Hardware fault coverage |
| All locally runnable pytest | 122 passed in 6.97s, exit 0 | Actual Docker (explicitly excluded) |
| Fresh Release recovery | 12 passed in 3.17s, exit 0 | Physical power loss |
| Docker build | **BLOCKED**, executable missing, exit 127 | Image buildability |
| Actual-Docker test_container.py | **2 explicit failures**, exit 1 | Writable/read-only runtime |
| Full Task10/11/M3 | **BLOCKED** by actual Docker | Complete deliverable acceptance |
| Hidden/resource/formal ANN admission | Unverified | Platform acceptance |
| Performance/stress/Task12 | Not executed | Performance scores |

Task10 source review found no Important/Critical/Minor issues and accepted source
quality, but explicitly blocked full Docker acceptance. It is not approval of
Docker behavior. No source defect was exposed by local L2; no core changes needed.

## Environment and reproduction

Python **3.12.10**, SQLGlot **30.31.0**, pybind11 **3.1.0**, pytest **9.2.0**;
C++20 is required by CMake. Full actual compiler/CMake version output and exact
configure/build/test commands/exits are in
`.superpowers/sdd/task-11-local-validation.log`; replay recipe is
`.superpowers/sdd/task-11-validate.sh` (its final exit 1 reflects Docker failure).
Production and test-fault builds are distinct: `build/task11-fresh` (Debug normal),
`build/task11-fault` (explicit fault ON), `build/task11-production` (Release,
BUILD_TESTING OFF, fault OFF), all configured from new empty build roots.

Report-checker fixtures are independent two-case reports, not public answers.
Tests first: 2 failures for missing tool; initial GREEN 2 passed. Added strict
nonfinite-overflow regression: 1 failed/1 passed, then final GREEN 2 passed in
0.99s. Raw logs: `task-11-red.log`, `task-11-green.log`,
`task-11-nonfinite-red.log`, `task-11-final-green.log` under `.superpowers/sdd/`.
The gate is read-only, with real boolean passed and exact unique IDs/all PASS;
CLI rejects malformed JSON/duplicate keys/nonfinite values and bad manifests.
It requires original report/manifest case fields, no invented metadata.

Final local pytest command (Docker exclusion is intentional and **not** full-suite
success):

```bash
PYTHONPATH="$PWD/build/task11-fresh/python:$PWD/minivectordb/python" \
MINIVECTORDB_FAULT_BUILD="$PWD/build/task11-fault/python" \
.venv/bin/python -m pytest minivectordb/tests \
  --ignore=minivectordb/tests/test_container.py -q
```

Output `122 passed in 6.97s`, exit 0 (`task-11-final-local.log`). Separately:
`docker build -t minivectordb:round2 .` exit 127, then
`.venv/bin/python -m pytest minivectordb/tests/test_container.py -q` exit 1,
`2 failed in 0.03s`, both `BLOCKED: actual Docker executable is unavailable`.
No Docker install/alias/Podman or skip/xfail substitute.

## Original unmodified driver: final local commands

From repository root with `PYTHONPATH=$PWD/build/task11-fresh/python:$PWD/minivectordb/python`:

```bash
command='["/home/howxu/Projects/2026-OceanBase-seekdb/.venv/bin/python","-m","minivectordb","--data-dir","{data_dir}"]'
.venv/bin/python protocol_check/run.py --command "$command" \
  --handshake-only --output /tmp/minivectordb-task11-local-ui47qdV7/handshake
.venv/bin/python protocol_check/run.py --command "$command" \
  --output /tmp/minivectordb-task11-local-ui47qdV7/local
.venv/bin/python minivectordb/tools/check_delivery.py \
  --report /tmp/minivectordb-task11-local-ui47qdV7/local/result.json --manifest mysqltest/manifest.json
```

All exit **0**; checker: `delivery report accepted: 20 unique PASS cases`.
Actual original reports: `/tmp/minivectordb-task11-local-ui47qdV7/handshake/result.json` and
`/tmp/minivectordb-task11-local-ui47qdV7/local/result.json`. Exact stdout/exits: `.superpowers/sdd/task-11-final-l2.log`.
An earlier distinct local run also passed at
`/tmp/minivectordb-task11-local-sstyuC2F`; neither run overwrote a report.
The unchanged Session uses `TemporaryDirectory` per case and substitutes its
fresh empty path into `{data_dir}`; restart reuses that case directory only.
Driver cleanup removes those data directories normally. No public assertions,
answers, resource configs or results were edited. Manifest version:
`minivectordb-public-v4`; IDs/counts checked individually:

| Manifest ID | SQL | Actual final status |
|---|---:|---|
| sql_crud | 7 | PASS |
| sql_comparisons | 4 | PASS |
| sql_ordering | 4 | PASS |
| sql_empty | 3 | PASS |
| sql_primary_key | 4 | PASS |
| sql_unicode | 3 | PASS |
| sql_bigint | 3 | PASS |
| sql_unsupported | 3 | PASS |
| storage_restart | 3 | PASS |
| vector_euclidean | 3 | PASS |
| transaction_commit | 6 | PASS |
| transaction_rollback | 6 | PASS |
| storage_mutations | 6 | PASS |
| storage_multiple_tables | 6 | PASS |
| vector_ties_and_filter | 5 | PASS |
| vector_float32 | 3 | PASS |
| vector_index_maintenance | 9 | PASS |
| transaction_failed_insert | 8 | PASS |
| transaction_autocommit_atomic | 4 | PASS |
| transaction_shutdown_rollback | 5 | PASS |

No FAIL/SKIPPED/BLOCKED/missing/extra/repeated IDs in the LOCAL public report.
Expected-error passes retain original driver rules, including permissive code
matching where the original config leaves mappings empty; public success is not
proof of all error-code semantics or hidden behavior.

## Recovery evidence, limitations and safety

Normal reopen tests verify committed data and rollback behavior. Separate
Task10 recovery tests stop at deterministic checkpoints then SIGKILL, covering
acknowledged versus uncommitted writes, multi-table publication and interrupted
actual cleanup. Real failed rename/fatal exit and missing-reference no-ready are
also covered. Production ignores test fault flags. These are process-crash and
specific syscall tests, **not physical power-loss** or exhaustive hardware/fsync
failure coverage.

Immutable table scans/rewrites are streamed; each mutation can rewrite a whole
table (O(table size) I/O). Metadata stays resident; input batches/single oversized
rows need proportional memory. Exact vector scans are O(N dimension), with O(K)
selection memory. Unbounded ordering uses disk merges, but final rows/JSON are
materialized and bounded by protocol response limits; no unlimited-result claim.
`ann` is exact FlatScan fallback, not approximate index acceleration or proof of
formal ANN structure/resource admission. Single process/client/transaction only.
No performance/stress work, Task12, platform submission or conversation material.

Safety evidence `.superpowers/sdd/task-11-safety.log`: branch/HEAD unchanged,
no staged files, protected-file SHA256 checks all OK, protected tracked diff empty,
`git diff --check` exit 0. Original hashes preserved:

- `.gitignore`: `3239c3a9f04beaa73cf85233829805f4b7f2a83761220e024369f6bbd8bb84cb`
- Original source plan: `e7eafebe9a731813e1460e6a67cef75e04741596c6ed921adc64d4728bfb178b`
- Existing unrelated experiment: `9f38c47be24db822f0a060123cb9bfff2d136815210ddd56522452c30b71f968`

No staging/commit, branch/worktree operation, reset/stash/clean/push/merge,
protected-path modification or fabricated conversation. Independent final source
review remains required; actual Docker checks must succeed before full M3 claim.
