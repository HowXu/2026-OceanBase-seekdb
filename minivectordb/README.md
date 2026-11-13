# MiniVectorDB — local public functional baseline

Python 3.12 hosts the JSONL entrypoint and SQLGlot tokenizer/parser/AST only;
pybind11 calls the handmade C++20 storage, execution, vector and transaction core
in the same process. Runtime does not launch a parser service or use another
database engine. Pins: Python 3.12.10, SQLGlot 30.31.0, pybind11 3.1.0,
pytest 9.2.0 (`requirements*.txt`).

**2026-11-11 LOCAL result: original public driver 20/20 cases (95 SQL), all PASS.**
Actual Docker 28.5.2 has now passed a clean-source, no-cache image build, default
entrypoint/read-only-root checks, original `--image` handshake and 20/20 public
cases. The duplicate-projection ORDER BY fix also has focused and full-suite
regressions. This does **not** establish formal admission: hidden cases, formal
resource admission and performance remain unverified. Initial baseline history is
in `docs/superpowers/experiments/2026-11-11-minivectordb-public-baseline.md`; current
evidence is in `docs/superpowers/experiments/2026-11-11-minivectordb-sql-order-docker.md`.

**2026-11-12 five-fix update:** ordinal ORDER BY, case-insensitive name binding,
table-level primary keys, original expression column labels, float32 underflow,
and incremental table segments have focused regressions. A fresh Docker image
passes the original 20/20 driver. The 8,000-row/64-UPDATE diagnostic reduced
update-stage device writes from 312,475,648 to 5,656,576 bytes; the generated
2,112 MiB/2 GiB recovery probe also passed. See
`docs/superpowers/experiments/2026-11-12-minivectordb-five-fixes-verification.md`.
These are local results, not formal hidden-case approval.

## Local build and focused checks

From the repository root, using a Python 3.12.10 `.venv` with
`pip install -r minivectordb/requirements-dev.txt`. Use a new build directory for
clean validation (the recorded run used `build/task11-fresh`):

```bash
cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Debug \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
  -Dpybind11_DIR="$(.venv/bin/python -m pybind11 --cmakedir)"
cmake --build build -j2
export PYTHONPATH="$PWD/build/python:$PWD/minivectordb/python"
ctest --test-dir build --output-on-failure
# Recovery requires a separate, explicitly fault-enabled TEST build:
cmake -S minivectordb -B build/fault -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF -DMINIVECTORDB_ENABLE_FAULT_INJECTION=ON \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
  -Dpybind11_DIR="$(.venv/bin/python -m pybind11 --cmakedir)"
cmake --build build/fault -j2
export MINIVECTORDB_FAULT_BUILD="$PWD/build/fault/python"
# LOCAL ONLY: explicitly excludes actual-Docker tests, not full-suite acceptance.
.venv/bin/python -m pytest minivectordb/tests \
  --ignore=minivectordb/tests/test_container.py -q
# Never enable fault injection for production.
.venv/bin/python -m minivectordb --data-dir /tmp/my-new-minivectordb-data
```

## Per-SQL diagnostics

`tools/run_public.py` imports the repository's original `Session`, `strict_json`
and `assert_response` without modifying or replacing them. It is a trusted-local
command runner, not a replacement for official validation or a Docker check.
It supports the original meanings of `--command` (JSON argv with `{data_dir}`),
repeatable `--case`, and `--output` (a **new** output directory). `--package` and
`--config` default to the repository's original package and driver config.

Example invocation for a selected case, when targeted integration is appropriate:

```bash
export PYTHONPATH="$PWD/build/python:$PWD/minivectordb/python"
.venv/bin/python minivectordb/tools/run_public.py \
  --command '[".venv/bin/python", "-m", "minivectordb", "--data-dir", "{data_dir}"]' \
  --case sql_crud --keep-going --output data/diagnostics-new
```

Omit `--case` to diagnose the whole package when full integration is authorized;
it is not a per-task unit-test prerequisite. No public inputs or answers are
copied into unit fixtures. Run the small generated-package checks instead:

```bash
.venv/bin/python -m pytest minivectordb/tests/test_public_diagnostics.py -q
```

Each case gets a new empty Session data directory. `--restart` reuses that same
Session/directory; `--mode exact` and `--mode ann` change subsequent request modes.
Only SQL consumes a result-array entry. Directives do not. Input paths and SQL/
result counts are validated before any process starts.

With `--keep-going`, ordinary SQL assertion failures do not stop collection.
Without it, the remaining SQL in that case is marked `NOT_RUN`; subsequent cases
still run. A broken transport (invalid JSON, wrong response ID, exit, etc.) always
ends the current session. Remaining SQL is `NOT_RUN`, never automatically retried.
As in the original driver, the first timeout stops **all later cases**, as do
operator interruption and cleanup failure. Normal restart/shutdown errors also
fail the case even if every attempted SQL assertion passed.

Reports:

- `result.json`: overall `passed`, aggregate `statistics`, and complete cases.
- `cases.json`: completed case diagnostics, updated after each case.
- `<case>.stderr.log`: original Session stderr log.

Each SQL record includes its zero-based assertion `index`, one-based input
`line`, `sql`, `mode`, `expected`, `response` (null if Session returned none),
`attempted`, `transport_ok`, `status`, failure `category`, and `message`.
Session errors additionally retain phase, exception type and message on the case.
`NOT_RUN` records keep their SQL/expectation/mode and a reason, without a response.

Statistics count SQL only, excluding ready/restart/shutdown:

- `attempted`: calls made to `Session.request(sql, mode)`, including transport failures.
- `transport_ok`: execute calls that returned a validated response from Session.
- `sql_passed`: original `assert_response` passed.
- `sql_failed`: attempted SQL with assertion/transport errors.
- `not_run`: SQL never attempted.

Thus `attempted = sql_passed + sql_failed`, and transport success does **not**
mean semantic success. A transport exception may occur before a request reaches
the process; `attempted` does not claim confirmed delivery. If the original
configuration leaves an expected error unmapped, any nonempty error code may
satisfy that assertion. Such passes are reported faithfully, not treated as proof
that the corresponding SQL feature is implemented. Exit code is 1 for any failed
SQL, unexecuted SQL or session error; only wholly passing cases exit 0. Invalid
CLI/package inputs are rejected before execution with argparse exit code 2.

These diagnostics do not replace original-driver integration. The local public
baseline below was measured using the original driver, not this runner. Docker
must actually be available before container validation can be reported as passing.

## Vector index declarations and search modes

`CREATE VECTOR INDEX name ON table(vector_column)` validates and persists a
**FlatScan** descriptor in the same catalog root as table schemas and immutable
table references. Index names are catalog-global and must be unique; the table
and VECTOR column must exist. Multiple distinct declarations may reference the
same VECTOR column. Failed declarations do not publish metadata.

Both `exact` and `ann` currently execute the exact backend against the same
visible table root. `ann` is an explicit **exact fallback**, also available without
an index; no HNSW, approximate structure or ANN acceleration is implemented or
claimed. Descriptors own no copied vector arrays, so INSERT/UPDATE/DELETE,
transaction read-own-write, rollback and reopen cannot leave a separate stale
index row set. Index DDL follows the same transaction/publication rules as tables.

## Original-driver validation and read-only report gate

Run from repository root; `PYTHONPATH` must name the chosen production extension
and `minivectordb/python`. Every output directory must be new. Original Session
allocates fresh empty per-case data directories; restart directives reuse only
that case's directory. The checker accepts/rejects reports; it never rewrites
results. It requires literal boolean `passed: true`, nonempty unique manifest
IDs, exact ID coverage and every status `PASS`; missing/extra/duplicate IDs or
FAIL/SKIPPED/BLOCKED are rejected. No invented version/environment report fields
are required; versions/capabilities are recorded separately.

```bash
out=$(mktemp -d /tmp/minivectordb-local-XXXXXXXX)
command="[\"$PWD/.venv/bin/python\",\"-m\",\"minivectordb\",\"--data-dir\",\"{data_dir}\"]"
.venv/bin/python protocol_check/run.py --command "$command" \
  --handshake-only --output "$out/handshake"
.venv/bin/python protocol_check/run.py --command "$command" --output "$out/local"
.venv/bin/python minivectordb/tools/check_delivery.py \
  --report "$out/local/result.json" --manifest mysqltest/manifest.json
```

**Actual-Docker verification commands:**

```bash
docker build -t minivectordb:round2 .
VECTORDB_IMAGE=minivectordb:round2 .venv/bin/python -m pytest \
  minivectordb/tests/test_container.py -q
.venv/bin/python protocol_check/run.py --image minivectordb:round2 \
  --handshake-only --output "$out/docker-handshake"
.venv/bin/python protocol_check/run.py --image minivectordb:round2 \
  --output "$out/container"
.venv/bin/python minivectordb/tools/check_delivery.py \
  --report "$out/container/result.json" --manifest mysqltest/manifest.json
```

No Podman substitute or skip/xfail counts as Docker acceptance. Dockerfile uses
multi-stage compilation, no runtime installs/network, direct JSONL entrypoint,
`/data` storage/temp files and disabled production fault injection. Its behavior
has actual Docker evidence for the scopes recorded above, not hidden-case or
formal platform approval.

## Storage, correctness and bounded limitations

- Single process/client and at most one explicit transaction. Supported MySQL-style
  subset: scalar CRUD, AND comparisons, deterministic ordering/LIMIT (including
  1-based projection ordinals), vector L2, vector-index descriptors and
  BEGIN/COMMIT/ROLLBACK. Table/column/index/alias lookup is ASCII case-insensitive;
  original spelling is retained for output. Inline and table-level single BIGINT
  primary keys, validated single-table qualifiers/aliases and redundant
  parentheses are accepted. Unsupported syntax is rejected.
- A logical table has one immutable PK-sorted base plus at most 64 ordered delta
  segments. INSERT/UPDATE/DELETE write only new rows/full replacement rows or
  tombstones; the next actual modification beyond the limit compacts the logical
  table into a new base. Readers merge segments by PK and newest segment wins.
  Catalog/transaction snapshots copy metadata, not whole tables. Input batches
  and a single oversized row still require memory proportional to their sizes.
  Storage input uses a bounded 64 KiB buffer per open segment and bulk float32
  vector decoding. Derived PK membership sets use copy-on-write for INSERT and
  DELETE, so duplicate checks and old-root isolation do not require a full-row scan.
- Publication validates/syncs only new segments, writes a new catalog and
  atomically replaces one CURRENT for all tables before success. Recovery
  validates the complete logical tables reachable from CURRENT, syncs CURRENT
  before GC, rejects any missing/corrupt committed segment and reclaims
  unreachable owned files. Shutdown/EOF rolls back active work.
- Exact search scans visible records and filters before bounded O(K) TopK;
  `ann` uses the same exact fallback, not approximate index acceleration.
  Unbounded scalar sorting uses disk runs/merges; final result rows and JSON
  response are materialized in memory and subject to protocol response limits.
  Streaming tables do not imply unlimited result-size or resource admission.
- Normal restart tests and deterministic SIGKILL tests are distinct. Acknowledged
  writes, uncommitted work, multi-table publication and interrupted cleanup have
  process-crash evidence. Real failed rename is tested. SIGKILL is not physical
  power loss, nor coverage of every hardware/fsync fault.
- Local public success does not establish hidden-case correctness, formal ANN
  structure/resource admission or platform acceptance. Generated resource probes
  are not official workloads; no formal T/B/M benchmark score is claimed.
