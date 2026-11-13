# MiniVectorDB local performance baseline

This directory contains an independent, generated workload for the round-two
default performance shape. It is not the official evaluator and does not produce
an official E, rank or score.

## Fixed workload

- 10,000 records, 128 float32 dimensions, seed 20261112.
- 100-row multi-row INSERT per batch, 100 acknowledged autocommit batches.
- 1,000 exact TopK queries, K=10; 500 filtered and 500 unfiltered.
- Query load, index preparation and 20 warmups are timed separately, not included
  in query T. Their database I/O and memory remain in B/M.
- Write and query projects use separate fresh containers/volumes per round.

The standalone C++ oracle scans the generated float32 data, filters before
sorting by `(distance,id)`, and does not link the database core. Python tests
cross-check it on a small independent example.

## Run

From the repository root:

```bash
.venv/bin/python -m pytest tools/benchmark/test_minivectordb_perf.py -q
.venv/bin/python tools/benchmark/minivectordb_perf.py \
  --image minivectordb:five-fixes --rounds 3 \
  --output .superpowers/sdd/performance-baseline/rounds
```

The output directory must be new. Each round writes:

- `round-N-write/result.json`: startup/schema/sync stages, all 100 batch timings,
  T/B/M, cgroup snapshots and complete post-restart content validation.
- `round-N-query/result.json`: load/index/warmup/sync stages, all 1,000 query
  timings, filtered/unfiltered distributions, T/B/M and exact/Recall validation.
- `round-N-query/responses.json`: raw measured responses.
- `summary.json`: complete rounds and medians, without combining best metrics
  from different rounds.

T is request/response wall time. B is the largest per-device cgroup v2
`rbytes+wbytes` delta (raw devices are retained, avoiding partition/mapper
addition). M is `memory.peak`, including page cache. A final `os.sync()` occurs
after measured T and before final B/M. Post-measurement correctness does not
modify the saved metrics.

Local host/image caches are not the platform's frozen cache preparation. New
volumes do not prove official cold-cache behavior. No official T/B/M baseline is
available locally, so only raw values and later like-for-like relative changes
are meaningful.

## perf

The profiling runner uses the same input but a separate host RelWithDebInfo
build. It attaches perf after startup (and after load/index/warmup for queries),
then stops perf before correctness checks and report generation:

```bash
cmake -S minivectordb -B build/perf-release \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
  -DMINIVECTORDB_ENABLE_FAULT_INJECTION=OFF \
  -DPython_EXECUTABLE="$PWD/.venv/bin/python" \
  -Dpybind11_DIR="$(.venv/bin/python -m pybind11 --cmakedir)"
cmake --build build/perf-release -j2

# perf may be a system binary or a locally extracted linux-perf binary.
export LD_LIBRARY_PATH=/tmp/mvdb-perf-tool/root/usr/lib/x86_64-linux-gnu
.venv/bin/python tools/benchmark/profile_minivectordb.py write \
  --perf /tmp/mvdb-perf-tool/root/usr/bin/perf \
  --output .superpowers/sdd/performance-baseline/profile-write
.venv/bin/python tools/benchmark/profile_minivectordb.py query \
  --perf /tmp/mvdb-perf-tool/root/usr/bin/perf \
  --output .superpowers/sdd/performance-baseline/profile-query
```

Profile times are not baseline T/B/M. `perf-*.data`, self-time `report.txt`,
`callgraph.txt`, record logs and independent correctness results are retained.
