# File SQL pre-submission fixes

> Execute inline with systematic-debugging, test-driven-development and verification-before-completion. The user approved fixing the four findings from the final contest review. This is a focused correction to the existing design, not a new feature.

## Goal and constraints

Reject malformed CSV/JSONL before returning a schema or rows, make CSV typed values use the same trimming policy as inference, and expose bounded diagnostics with a file and record/byte position through SQL.

Work directly on `kernel/2026-competition`. CMake/CTest only; every build uses `-j3`, with no concurrent builds. Stop only `/home/howxu/seekdb-file-sql-smoke.QDaFgE` before rebuilding its server binary. Leave the original eight file_sql fixtures/tests unchanged, and do not change or stage `docs/ai-function-enhancement-progress.md`. No worktree or push.

## Chosen approach

Keep the existing readers and native executor. A CSV field may end only at its configured delimiter, a line terminator, or EOF; whitespace after a quoted field retains its current behavior. Normalize only inferred non-string CSV values when producing cells, preserving VARCHAR bytes and the exact quoted/unquoted NULL-marker distinction. Parse JSON numbers using `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`, without converting the complete input line to an untracked temporary buffer. Track record number and start byte in ByteSource, reset those counters on rescan, and construct contextual parser errors explicitly; do not catch and replace resource/cancellation exceptions. Forward existing exception text as the argument to OB_ERR_UNEXPECTED's built-in `%s` message.

Alternatives rejected: a permissive CSV mode would retain incorrect rows; trimming all strings would change user data and COPY round trips; replacing the JSON/parser libraries would exceed the scope and disturb memory accounting.

## Tasks

### 1. CSV record boundaries

Files: `unittest/sql/engine/basic/test_file_scan_utils.cpp`, `src/sql/engine/basic/ob_file_scan_utils.cpp`.

- [x] Add a regression where `v\n"one"garbage\n` and the same input with whitespace after the quote fail inference. Keep valid doubled quotes, quoted newlines and trailing whitespace controls.
- [x] Build `test_file_scan_utils -j3` and run the focused test; confirm failure before editing the reader.
- [x] Reject a separator other than delimiter, CR/LF or EOF after a field. Re-run the focused regression and scan suite.

### 2. CSV typed whitespace

Files: the same scan library/test files, plus the established mysqltest regression pair.

- [x] Add a scan regression covering padded BIGINT, DOUBLE, BOOLEAN, DATE and DATETIME values, while preserving padded VARCHAR and quoted NULL-marker text through rescan and reordered projection.
- [x] Observe the regression fail on untrimmed typed cells.
- [x] Normalize only non-string, non-NULL typed cells using the existing budgeted trim helper; preserve NULL detection against the original bytes.
- [x] Add SQL assertions for typed values, metadata, COUNT, row/batch execution and a NULL/string control.

### 3. Strict JSON numbers

Files: `unittest/sql/engine/basic/test_file_scan_utils.cpp`, `src/sql/engine/basic/ob_file_scan_utils.cpp` and the mysqltest regression pair.

- [x] Add rejecting cases for `+1`, `01`, `-01`, `.1`, `1.`, `1e`, `1e+`, `1e-`, `1.e2`, `--1` and `1e.2`, plus valid signed/exponent/zero controls.
- [x] Observe the rejecting cases fail before replacing the permissive number loop.
- [x] Consume integer, fraction and exponent components separately, requiring the appropriate digits; reject tokens with an invalid following character.
- [x] Cover invalid input through both COUNT and value projection so projection cannot determine JSON validity.

### 4. Contextual and correctly forwarded diagnostics

Files: the scan library/tests; `src/sql/resolver/dml/ob_file_table_resolver.cpp`, `src/sql/engine/basic/ob_file_table_op.cpp`, `src/sql/engine/basic/ob_select_into_op.cpp`; added mysqltest expectations only.

- [x] Add diagnostic assertions for a CSV column mismatch, an unterminated quoted multiline record and malformed JSON on a later line, including blank-line/CRLF input.
- [x] Observe diagnostic assertions fail. Record the old SQL `%s` failures in the focused SQL test before rebuilding.
- [x] Track record/start-byte counters in ByteSource and construct contextual syntax/shape errors explicitly in CSV inference/scanning and JSON inference/scanning. Preserve the 1024-byte error bound and native cancellation/error codes.
- [x] Remove redundant format arguments from file SQL exception forwarding; use plain messages for native typed-conversion failures rather than treating OB_ERR_UNEXPECTED as a printf API.
- [x] Update the two existing added expectations that currently assert literal `%s`, using only stable normalized content. Do not modify official expectations.

### 5. Verification and documentation

- [x] Stop the dedicated instance, build `seekdb test_file_table_op test_file_sql_parser test_file_scan_utils -j3`, and run the three file CTests.
- [x] Restart the same dedicated instance and run official file_sql_basic plus all six existing special suites and the new diagnostic suite, without bypassing SSH or original include scripts.
- [x] Rebuild/run the independent scanner ASan/UBSan CTest (`-j3`, leak detection and halt-on-error enabled); repeat COPY writer-race coverage.
- [x] Verify original eight files against the competition baseline, source/header ownership checks, `git diff --check`, and unchanged user-document hash.
- [x] Update file SQL progress/usage documentation with failures and fresh results. Commit only this work and the pending file SQL acceptance docs after examining the staged diff; do not push.

## Evidence

Initial live reproductions: `/home/howxu/seekdb-file-sql-final-evidence/contest-final-review.json` (11 probes). New RED/GREEN/build/SQL logs will be retained under that evidence directory. Passing earlier tests does not close these newly discovered findings.
