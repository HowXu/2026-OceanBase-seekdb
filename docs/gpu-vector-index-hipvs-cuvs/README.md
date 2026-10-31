# hipVS/cuVS GPU vector backend

Status: optional CAGRA snapshot backend implemented. This branch includes
persistence and CPU recovery support. A compatible AMD GPU and ROCm/hipVS
installation are required to validate real acceleration; offline tests do not
establish a GPU speedup.

## Scope

The backend adds optional hipVS/cuVS acceleration behind seekdb's existing
vector adaptor. It does not replace VSAG globally.

- GPU support is compiled only with `OB_BUILD_CUVS=ON`; the default is `OFF`.
- A dense L2 HNSW/HGRAPH index opts in with `WITH (lib=cuvs)`.
- `lib=vsag` indexes keep their existing CPU behavior.
- Cosine and inner-product metrics are rejected for `lib=cuvs` at DDL time.
- Filters, deletes, stale GPU indexes, and unavailable GPUs fall back to VSAG.
- `dbms_vector.batch_knn` exposes one-call batched GPU ANN for explicit batch
  workloads.

The product integration lives in:

- [`ob_vsag_adaptor.cpp`](../../src/oblib/lib/vector/ob_vsag_adaptor.cpp):
  per-index marking, buffered vectors, lazy CAGRA build/search, freshness and
  safety fallback, trace points, and batch entry points.
- [`ob_plugin_vector_index_adaptor.cpp`](../../src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp):
  maps declarative `lib=cuvs` indexes to the adaptor handle.
- [`ob_vector_index_util.cpp`](../../src/observer/vector_index/ob_vector_index_util.cpp):
  validates the L2-only DDL contract.
- [`ob_dbms_vector_mysql.cpp`](../../src/pl/sys_package/ob_dbms_vector_mysql.cpp):
  implements `dbms_vector.batch_knn`.

## Bridge

seekdb links a small C ABI bridge instead of importing hipVS/cuVS C++ types into
the server. The maintained bridge source and build entry point are under
[`tools/hipvs_bridge`](../../tools/hipvs_bridge/):

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
```

The bridge exports:

```text
seekdb_cuvs_build
seekdb_cuvs_search
seekdb_cuvs_free
seekdb_cuvs_cagra_knn
seekdb_cuvs_serialize
seekdb_cuvs_raw_snapshot
seekdb_cuvs_deserialize
```

### Contest algorithm experiments

The contest binary uses fixed hipVS CAGRA Search by default; no algorithm
environment variable is required. `SEEKDB_CUVS_ALGO=brute_force` is retained
only for non-contest experiments and is not submission-compliant. Use the same
algorithm environment when serializing and reloading a snapshot; the OBCUVS02
header does not record the algorithm tag. New BruteForce snapshots store IDs and the base vectors only
once, use `graph_bytes=0`, and rebuild the GPU index on load; older binaries are
not guaranteed to read this experimental form. CAGRA and older nonzero-graph
snapshots retain the graph path. Each index handle also reuses its query/output
buffers and search parameters, releasing them when the handle is destroyed.
These changes require real GPU A/B validation before any performance benefit is
claimed.

Build seekdb with the resulting library:

```bash
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  --make -j64
```

For a CPU-only build, leave the defaults or pass:

```bash
./build.sh release \
  -DOB_BUILD_CUVS=OFF \
  -DOB_BUILD_CUVS_TRACE=OFF \
  --make -j64
```

## SQL usage

Per-index GPU opt-in:

```sql
CREATE TABLE items(
  id BIGINT PRIMARY KEY,
  embedding VECTOR(128),
  VECTOR INDEX idx_embedding(embedding)
    WITH (distance=l2, type=hnsw, lib=cuvs)
);
```

Explicit batch ANN writes results to a caller-created output table:

```sql
CALL dbms_vector.batch_knn(
  "items",
  "query_vectors",
  10,
  "batch_results"
);
```

See [`examples/batch_knn.sql`](examples/batch_knn.sql) for the table contract.

## Validation

The self-contained GPU smoke runner generates deterministic vectors and ground
truth; it does not use private `/work/datasets` or `/work/bench` assets:

```bash
LD_LIBRARY_PATH=/work/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
    --observer /path/to/trace-build/seekdb \
    --bridge /work/bridge/libseekdb_cuvs_bridge.so \
    --base-dir /tmp/seekdb-cuvs-smoke \
    --port 2981 \
    --render /dev/dri/renderD135 \
    --evidence /tmp/seekdb-cuvs-evidence
```

The runner validates:

- cuVS L2 build/serve and same-process VSAG isolation;
- non-L2 DDL rejection;
- filter, delete, and freshness fallback;
- threshold-triggered CAGRA rebuild;
- one-call `batch_knn`, row cardinality, and recall for Top-10 and Top-100;
- GPU device file descriptors and hidden-GPU fallback;
- bounded observer cleanup and closed ports.

The runner writes a machine-readable `summary.json` into its evidence directory.
Results for a specific candidate belong in that pull request's description or a
validation comment rather than in the source tree.
The no-GPU DDL contract remains covered by
[`vector_index_cuvs_ddl.test`](../../tools/deploy/mysql_test/test_suite/vector_index/t/vector_index_cuvs_ddl.test).

## Build-time benchmark

Use an isolated trace-enabled server (`OB_BUILD_CUVS_TRACE=ON`,
`OB_VSAG_TRACE=1`, `OB_VSAG_TRACE_FILE=/tmp/hipvs.trace`). Load the same vectors
before creating each index, alternate backend order, and repeat at least three
times. Install `numpy` and `pymysql` in the benchmark environment, then run:

```bash
python3 docs/gpu-vector-index-hipvs-cuvs/validation/benchmark_build.py \
  --host 127.0.0.1 --port 2881 --rows 100000 --dim 128 \
  --queries 100 --k 10 --repeats 3 --minimum-recall 0.95 --ef-search 16000 --m 32 \
  --trace-file /tmp/hipvs.trace --server-log /path/to/log/seekdb.log \
  --output /tmp/hipvs-evidence/build-benchmark.json
```

Set `SEEKDB_PASSWORD` for password authentication. The script creates a unique
database and removes only that database on exit; `--keep-database` preserves it
for investigation. Run on the server host so evidence files refer to the tested
process, and avoid unrelated server traffic during measurement. Keep GPU, CPU,
ROCm/hipVS versions, CPU count, memory budget and source revision with evidence.
Set host parallelism on the observer process to match its CPU allocation. For
the tested 16-CPU container, the observer used `cpu_count=16`, an 8 GiB memory
budget, and `OMP_NUM_THREADS=8`; its host exposed 128 logical CPUs, so relying
on host-wide OpenMP auto-detection would oversubscribe the container.

The JSON reports DDL time and DDL plus first-query time separately, so lazy
construction is included. GPU results require both a snapshot build log and a
`cuvs_serve` trace, plus Recall@K above the configured threshold. A fallback,
failed quality gate, or missing trace cannot produce an accepted speedup.
`--prepare-only` checks dataset generation without a database and explicitly
labels its output as having no GPU measurement. Run sizes 10K, 100K and 1M at
dimensions 128/384/768 as hardware memory permits; keep complete raw results.

This compares VSAG HNSW with CAGRA in seekdb's HNSW SQL route. It does **not**
convert CAGRA into a VSAG HNSW graph. CAGRA uses graph degree 128, intermediate
degree 256 (255 at the 256-row boundary) and 100 NN-Descent iterations; VSAG uses
`m=16,ef_construction=200` by default (the example raises `m` to 32). The algorithms have different graph parameters, so
assess build time together with recall and first-ready time. Query batch
throughput is a separate metric and must not be reported as index-build speedup.

SQL HNSW queries use the session variable `ob_hnsw_ef_search`; setting only the
DDL `ef_search` parameter leaves the query session at its default. The benchmark
sets both with `--ef-search` (default 200) and records the effective session
value. Raise it until the CPU baseline meets the same recall threshold; the
example uses 16000 for random high-dimensional vectors. This changes query
effort. CPU graph parameters are separately configurable with `--m` (default 16)
and `--ef-construction` (default 200), and are recorded in the JSON. If recall
plateaus even at high query effort, calibrate graph quality before measuring.
The example uses `m=32`; report the chosen graph parameters with every speedup.
Compare DDL time independently of first-query time.

The input branch's GPU construction parameters are fixed at graph degree 128,
intermediate degree 256 and 100 NN-Descent iterations. The 256-row boundary uses
255 intermediate neighbors because hipVS requires the degree to be smaller than
the dataset. Do not reduce these parameters to obtain a build-speed claim.
The bridge contract tests assert the actual values passed to the hipVS C API.

Queries retain at least 512 candidates, or 1024 above 256 dimensions, and select
`MULTI_CTA` whenever the retained frontier exceeds the default `SINGLE_CTA`
limit of 512. These are the search settings from before the sparse experiment.

## Validation on AMD hardware (2026-10-30)

On AMD `gfx1100` with ROCm 7.2.4 / hipVS 1.0.0, the GPU/TRACE Release build,
GPU routing, filtering/deletes, restart persistence and unavailable-GPU recovery
checks passed. Three-trial median DDL speedups were **1.64x** for 10K x 128
vectors and **9.34x** for 100K x 128 vectors, with both backends above 95% Recall@10.
CPU parameters were `m=16` and `m=32` respectively; both used
`ef_construction=200` and session `ob_hnsw_ef_search=16000`.

For 100K x 384 vectors, CPU calibration at `m=64,ef_construction=200`
and session `ob_hnsw_ef_search=16000` reached 97.6% Recall@10 on the original
100 queries and 97.7% on 100 additional queries. This CPU profile was explicitly
calibrated to exceed the 95% gate; it is recorded independently of GPU settings.

**Correction on 2026-10-31:** the later sparse-GPU experiment changed construction
parameters to 32/64 edges and six iterations. Its **69.63x claim is withdrawn**
from the accepted contest/fixed-parameter results, despite passing the synthetic
recall checks. The original 128/256/100 construction parameters and pre-sparse
search settings have been restored. A fresh 384-D comparison with these settings
has not run: the previously supplied remote SSH connection is closed.

No official test harness has been supplied or run. Current accepted results and
the historical, explicitly withdrawn experiment are recorded in
[`validation/results-20261030.json`](validation/results-20261030.json) and
[`validation/results-384-20261030.json`](validation/results-384-20261030.json).
The tuning archive is historical and must not be used as the submission profile.

## Offline checks

```bash
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
python3 unittest/oblib/vector/test_hipvs_benchmark.py
# Requires an installed VSAG SDK (SEEKDB_DEPS can override the deps prefix):
python3 unittest/oblib/vector/run_hipvs_adaptor_component.py
```

The bridge tests use a queued C API test double with injected allocation,
copy, stream, build and reload failures under ASan/UBSan. They check resource
cleanup and host-buffer completion, not kernel correctness or real ROCm ABI
compatibility. The existing adaptor test suite also covers loading complete
GPU snapshots in CPU-only builds and rejecting truncated snapshots.
The optional component runner compiles the real adaptor against installed VSAG
with small replacements for OceanBase logging and clock dependencies. Its two
variants test CPU-only recovery and GPU-unavailable recovery; it is not a full
server test. CPU-restored handles stay on VSAG when the plugin marks them again
before subsequent inserts, so their GPU buffers cannot omit historical rows.

## Current limitations

- Phase 1 supports dense float32 L2 indexes only.
- CAGRA snapshots use `OBCUVS02` and include graph bytes, float32 vectors and
  external IDs. When GPU loading is unavailable, complete snapshots rebuild a
  CPU VSAG index. GPU build/serialization failures before writing a snapshot
  prefix also fall back to VSAG serialization.
- The explicit batch API rebuilds its CAGRA index per call.
- GPU validation is manual because upstream CI has no compatible GPU runner.
- Multi-GPU and long-duration validation remain future work. Snapshot-reload
  smoke coverage is provided by `validation/run_ddl_persistence_smoke.sh`.
- New hardware or configuration changes still require GPU correctness,
  DDL/restart smoke and end-to-end measurements on compatible hardware;
  offline tests alone do not validate performance.
