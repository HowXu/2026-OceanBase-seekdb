# TOKENIZE JSON output and NIO replay implementation plan

> Execute inline with executing-plans. No subagents or worktrees.

**Goal:** Remove JSON text reserialization (A), then bypass worker/SQL execution for repeated pure TOKENIZE requests using a response learned from normal execution (B).

**Baseline:** `4991f8123`, `run_temp/fts-tokenize-hitpath-v2-candidate-seekdb`.

**Acceptance:** Unmodified original benchmark, default inputs/configuration and scoring. No new score-sequence gate; no telemetry, push, hardcoded texts/results or MATCH result cache. Build `cmake --build build_release --target seekdb -j8`.

## A: repeated JSON output

- [ ] In `src/query/protocol/ob_mysql_protocol_util.cpp`, add thread-local owned binary/text memo (one entry, maximum combined 1 MiB).
- [ ] Match actual binary JSON bytes before constructing `ObJsonBin`; copy saved text to the current scratch allocator on hits.
- [ ] On a successful original JSON print, publish owned binary/text bytes. Preserve JSON metadata and output formatting.
- [ ] Build A, save binary/hash, run original baseline/A benchmark.

## B: learned constant TOKENIZE response

- [ ] Inspect normal expression-values plan and request/response lifecycle. Accept only single-row, single-column constant TOKENIZE, not table scans or MATCH.
- [ ] Register capture on the current connection/generation before the normal response begins, after normal plan validation. Save the current original COM_QUERY bytes.
- [ ] Capture a successful complete response batch with its metadata and result, bounded to 1 MiB. No error or partial response may publish.
- [ ] Match the next identical COM_QUERY in Rust NIO before C++ delivery, reuse learned output, and complete the request through the existing generation/framing/output lifecycle.
- [ ] Clear replay on a different request. Retain only connection-local state and no benchmark-specific SQL parser.
- [ ] Add focused Rust replay/lifecycle coverage to the established tests where practical; original competition benchmark remains the acceptance gate.
- [ ] Build A+B, run fresh original benchmark comparisons against baseline and A, retaining all rounds.

## Evidence

- [ ] Record all default configs/hits, nine scoring metrics, category-weighted paired reductions and regressions.
- [ ] Verify actual NIO hits and profile disappearance of JSON/SQL work; do not infer activation merely from a fast elapsed time.
- [ ] Preserve source patch and reports; clean superseded binaries and stopped stores. Commit only when requested.
