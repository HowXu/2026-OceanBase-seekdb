# MiniVectorDB

A single-process vector database for the `2026-competition-round2` task. Go 1.22,
standard library only (no SQL, storage or vector-index libraries).

## Build and run

```bash
docker build -t minivectordb:latest .          # from the repository root
docker run --rm -i -v minivectordb-data:/data minivectordb:latest
```

Local build without Docker: `cd minivectordb && go build -o mvdb ./cmd/minivectordb`.

Flags (each also has an environment variable):

| Flag | Env | Default | Meaning |
| --- | --- | --- | --- |
| `--data-dir` | `MVDB_DATA_DIR` | `/data` | all data files |
| `--mode` | `MVDB_SEARCH_MODE` | `exact` | `exact` or `ann`, used only when a request omits `search_mode` |
| `--sync` | `MVDB_SYNC` | `fdatasync` | `none` skips fdatasync (survives kill -9, not power loss) |
| `--index-mem-mb` | `MVDB_INDEX_MEM_MB` | `256` | cap on in-memory vectors per table; also capped to 1/4 of the cgroup limit |

## Protocol

JSON Lines over stdin/stdout, as specified in the repository root
`STDIO_PROTOCOL.md`. Code: `internal/proto` and `cmd/minivectordb`.

* After recovery and index rebuild, stdout gets `{"type":"ready"}` before the
  first request is read.
* Each stdin line is one request; each gets exactly one response line,
  flushed immediately, echoing `id` with its JSON type:
  * `{"id":1,"op":"execute","sql":"...","search_mode":"exact"}` runs the SQL
    in `sql` with the given search mode (`exact` or `ann`).
  * Query: `{"id":1,"ok":true,"columns":[...],"rows":[[...]]}`. Write:
    `{"id":1,"ok":true,"affected_rows":N}`. Other statements: `{"id":1,"ok":true}`.
  * Failure: `{"id":1,"ok":false,"error":{"code":"duplicate_key","message":"..."}}`;
    the process keeps serving requests.
  * `{"id":9,"op":"shutdown"}` rolls back an open transaction, closes the log,
    replies `{"id":9,"ok":true}` and exits 0 with no further output.
* End of stdin, SIGTERM and SIGINT also shut down cleanly (no reply).
* BIGINT values are JSON integers, vectors are JSON arrays of float32 values
  (shortest round-trip form), distances are finite JSON numbers.
* Logs go to stderr only.
* `ORDER BY ... APPROXIMATE` forces ANN for that query. Every request carries
  its own `search_mode`, so `SET search_mode` has no effect beyond itself.

Error codes: `syntax_error`, `unsupported_sql`, `duplicate_key`,
`dimension_mismatch`, `invalid_vector`, `data_too_long`, `out_of_range`,
`invalid_value`, `table_exists`, `table_not_found`, `column_not_found`,
`column_count_mismatch`, `missing_value`, `type_mismatch`, `transaction_error`;
protocol errors use `invalid_request` and `unsupported_op`. The message
carries the MySQL error number and SQLSTATE.

## SQL subset

`CREATE TABLE` (BIGINT, VARCHAR(n), VECTOR(n); one BIGINT primary key, at most
one vector column), `DROP TABLE`, `CREATE VECTOR INDEX name ON t(col)`,
`INSERT` (multi-row, optional column list), `SELECT` (`*`, columns, aliases,
`L2_DISTANCE`, `WHERE` with `= <> < <= > >=` and `AND`, `ORDER BY` columns,
aliases, ordinals or expressions, `LIMIT n [OFFSET m]`), `UPDATE`, `DELETE`,
`BEGIN`/`START TRANSACTION`, `COMMIT`, `ROLLBACK`.

Rejected with `unsupported_sql`: NULL, JOIN, aggregates, subqueries, OR/NOT,
ALTER, savepoints, primary key updates and other statements outside the subset.

Semantics: VARCHAR length counts Unicode code points, strings compare by code
point, vectors are float32 and reject NaN, Infinity, overflow and wrong
dimensions, ties in any ORDER BY break by primary key ascending, filtered TopK
runs inside the filtered set.

## Design

**Storage.** One append-only log, `data.log`, is both the redo log and the data
store. Records are `[len][crc32c][type][txn][payload]`: PUT (full row), DEL,
COMMIT, ABORT, DDL. Each table keeps an in-memory map from primary key to the
row's offset in the log; rows are read in place through a read-only mmap, so
table data does not have to fit in memory. Row bytes use a fixed layout (pk,
other BIGINTs, vector, then VARCHARs) so most columns sit at constant offsets.

**Transactions.** A transaction is an overlay map (pk to new offset or deleted)
on top of the committed maps. Its records are appended as statements run, but
only take effect when its COMMIT record is in the log. COMMIT appends the
record, calls `fdatasync`, merges the overlay, and only then replies. Every
statement is validated in full before anything is written, so a failed
statement leaves nothing behind; an autocommit statement is a one-statement
transaction. ROLLBACK drops the overlay and writes an unsynced ABORT.

**Recovery.** Replay the log, applying a transaction's records only when its
COMMIT is found, then truncate after the last complete committed record. This
removes torn tails and uncommitted work, and replaying twice gives the same
result, so a crash during recovery is harmless. If dead data exceeds both
256 MiB and the live data, the log is rewritten to a temp file, synced and
renamed. Vector indexes are rebuilt before the ready line.

**Vector search.** Each table keeps a contiguous float32 copy of its committed
vectors (within the memory budget). TopK scans it with a bounded max-heap,
abandons a distance early once its partial sum exceeds the current K-th best,
and evaluates the WHERE filter only for rows that would enter the heap.
Uncommitted rows of the current transaction are scanned from the overlay.
Distances are accumulated in float64. An IVF partition (k-means, sqrt(N)
lists) exists for `ann` mode but is used only from 200k rows up, without
filters and outside transactions with pending changes; below that, `ann`
returns exact results (see the measurements below). If the vectors exceed the
budget, queries fall back to a heap scan over the log.

## Tests

```bash
cd minivectordb
go test ./...                       # atomicity, rollback, crash at every byte offset, compaction
python3 ../protocol_check/run.py --command '["./mvdb","--data-dir","{data_dir}"]' --output /tmp/pc-$(date +%s)   # public cases, official checker
python3 tools/crash_test.py --bin ./mvdb --rounds 40             # kill -9 rounds, including during recovery
python3 tools/bench.py --bin ./mvdb                              # local stand-in for the benchmark
```

With Docker: `python3 protocol_check/run.py --image minivectordb:latest --output ...`
from the repository root (see `protocol_check/README.md`).

Local results (macOS arm64, reference only; the official run is Linux x86_64
with cgroup v2 accounting):

| Check | Result |
| --- | --- |
| public cases (`protocol_check/run.py`, handshake + 20 cases) | 20 / 20 |
| crash test, seeds 1-3, 40 rounds each | no lost commits, no partial or leaked transactions |
| bench, uniform random data, exact | write 0.71 s, query 0.67 s, peak RSS 26 MB, log 5.2 MB, Recall@10 1.0 / 1.0 |
| same, IVF nprobe = nlist/4 (before disabling it at 10k rows) | query 0.44 s, unfiltered Recall@10 0.61 |
| clustered data (32 centers), index + `ann` | write 0.69 s, query 0.34 s, Recall@10 1.0 / 1.0 |

All bench rows use 10k rows x 128 dims, 100-row commits and 1000 queries with
K=10, half filtered. Early-exit distance helps on clustered data and gives no
measurable gain on uniform random data, where the K-th best distance stays
close to the average.

## Known limits

* The full error code mapping has not been published (`protocol_check/config.json`
  has empty `error_codes`), so the semantic names above may need adjusting.
* The primary key map stays in memory (about 50 bytes per row).
* Single connection and at most one explicit transaction, as in the task.
* DDL inside an explicit transaction commits that transaction first.
