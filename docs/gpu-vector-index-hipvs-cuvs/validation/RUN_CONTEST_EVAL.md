# V5 / candidate paired contest evaluation

`contest_eval.py` compares the frozen V5 revision with a candidate on the same
host. It uses deterministic synthetic datasets because the official SIFT250K
and Hidden250K assets and judge are not available locally. A local pass does
not certify Hidden250K or an official score.

## What it measures

- Official DDL shape with `ef_search=64` and DDL plus first-query timing.
- 4096 independent single-row approximate SQL queries for Top-10 and Top-100.
- QPS, P50/P95/P99, exact Recall, duplicate/missing/illegal IDs.
- Baseline recall deltas, query regression, GPU evidence and CPU fallback.
- GPU utilization, VRAM and observer RSS.
- Two paired rounds with reversed execution order.

`quick` generates 10K x 128 data. `full` generates eight datasets: 250K at
128/256/384/768 dimensions and 1M clustered data at 128/256 dimensions. Full
exact ground truth and repeated data loading can take hours. Data and ground
truth are cached, but a SQL round is never resumed or reused as a performance
result.

## Build two trace-enabled binaries

The official build uses `OB_BUILD_CUVS_TRACE=OFF`. This evaluator requires
trace evidence, so make separate evaluation builds for both versions. Do not
overwrite the official `build_release` artifacts.

The remote server provides CMake at
`/root/cmake-4.5.0-rc2-linux-x86_64/bin/cmake` and an offline Python
environment at `/opt/venv`. No network access is needed.

### Candidate

Wait for any existing build to finish, then run:

```bash
CMAKE=/root/cmake-4.5.0-rc2-linux-x86_64/bin/cmake
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  /root/worker/tools/hipvs_bridge/build.sh \
  /work/eval-bin/candidate/libseekdb_cuvs_bridge.so

"$CMAKE" -S /root/worker -B /root/worker/build_eval -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=ON \
  -DCUVS_BRIDGE_LIB=/work/eval-bin/candidate/libseekdb_cuvs_bridge.so
"$CMAKE" --build /root/worker/build_eval --target seekdb -j96
```

### Frozen V5

Use a detached worktree so the candidate checkout is not switched:

```bash
cd /root/worker
git worktree add --detach /root/worker-v5 \
  634888d9b037153fa143d2c913ba4b25503e1d62
ln -s "$(readlink -f /root/worker/deps/3rd)" /root/worker-v5/deps/3rd

CMAKE=/root/cmake-4.5.0-rc2-linux-x86_64/bin/cmake
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  /root/worker-v5/tools/hipvs_bridge/build.sh \
  /work/eval-bin/v5/libseekdb_cuvs_bridge.so

"$CMAKE" -S /root/worker-v5 -B /root/worker-v5/build_eval -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=ON \
  -DCUVS_BRIDGE_LIB=/work/eval-bin/v5/libseekdb_cuvs_bridge.so
"$CMAKE" --build /root/worker-v5/build_eval --target seekdb -j96
```

If `deps/3rd` already exists in the V5 checkout, inspect it before replacing
it. Both builds must use the same frozen dependency cache.

## Unit and offline checks

```bash
cd /root/worker
PYTHONDONTWRITEBYTECODE=1 /opt/venv/bin/python \
  unittest/oblib/vector/test_contest_eval.py -v
PYTHONDONTWRITEBYTECODE=1 /opt/venv/bin/python \
  unittest/oblib/vector/test_hipvs_benchmark.py -v
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
```

## Quick paired evaluation

Use a new output directory. The runner starts one observer at a time on port
2982 and refuses to use a nonempty output or server directory.

```bash
cd /root/worker
RUN=/work/evals/quick-$(date +%Y%m%d-%H%M%S)
OPENBLAS_NUM_THREADS=24 /opt/venv/bin/python \
  docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py \
  --mode quick \
  --baseline-observer /root/worker-v5/build_eval/src/observer/seekdb \
  --candidate-observer /root/worker/build_eval/src/observer/seekdb \
  --baseline-bridge /work/eval-bin/v5/libseekdb_cuvs_bridge.so \
  --candidate-bridge /work/eval-bin/candidate/libseekdb_cuvs_bridge.so \
  --baseline-revision 634888d9b037153fa143d2c913ba4b25503e1d62 \
  --candidate-revision "$(git rev-parse HEAD)" \
  --output-dir "$RUN" \
  --data-cache /work/evals/datasets
```

A quick run exits zero only with `QUICK_PASS`. Inspect `summary.md`,
`summary.json`, raw answers, traces and logs before starting the full matrix.
Runtime directories under `runs/` are retained for diagnosis; after checking
disk usage, remove only the completed evaluation's own `runs/` directory.

## Full evaluation

Use a different output directory and the same dataset cache:

```bash
cd /root/worker
RUN=/work/evals/full-$(date +%Y%m%d-%H%M%S)
OPENBLAS_NUM_THREADS=24 /opt/venv/bin/python \
  docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py \
  --mode full \
  --baseline-observer /root/worker-v5/build_eval/src/observer/seekdb \
  --candidate-observer /root/worker/build_eval/src/observer/seekdb \
  --baseline-bridge /work/eval-bin/v5/libseekdb_cuvs_bridge.so \
  --candidate-bridge /work/eval-bin/candidate/libseekdb_cuvs_bridge.so \
  --baseline-revision 634888d9b037153fa143d2c913ba4b25503e1d62 \
  --candidate-revision "$(git rev-parse HEAD)" \
  --output-dir "$RUN" \
  --data-cache /work/evals/datasets
```

To rerun selected datasets, repeat `--dataset` in a new output directory:

```bash
  --dataset gaussian250k-d128 --dataset gaussian250k-d256
```

Statuses:

- `QUICK_PASS`: quick paired rounds passed all local gates.
- `OFFICIAL_LIKE_PASS`: the two official-shaped synthetic sets passed; this is
  not an official result.
- `LOCAL_STRICT_PASS`: the complete eight-dataset matrix passed.
- `SUBSET_PASS`: a selected subset passed without claiming full-matrix coverage.
- `FAILED`: a query, quality, evidence, resource or execution gate failed.

## Existing functional suites

The evaluator does not replace the existing smoke tests. Run
`run_gpu_smoke.sh` for DDL contracts, freshness, deletes, hidden-GPU fallback
and batch correctness, and `run_ddl_persistence_smoke.sh` for restart and
snapshot persistence. Their setup requirements are documented in
`docs/gpu-vector-index-hipvs-cuvs/README.md`.
