# Contest Paired Evaluation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an offline, reproducible V5-versus-candidate GPU CAGRA evaluator with deterministic datasets, exact recall, performance/resource gates, and machine-readable evidence.

**Architecture:** One Python entry point owns deterministic data/cache generation, isolated server lifecycle, paired SQL execution, and reports. Pure metric functions remain independent of the database for standard-library unit testing. NumPy and PyMySQL come from the remote offline virtual environment; large artifacts stay outside Git.

**Tech Stack:** Python 3.12, NumPy, persistent mysql CLI using the seekdb mysql protocol, ROCm SMI, Bash, Git, standard-library `unittest`.

## Global Constraints

- Freeze the baseline at `634888d9b037153fa143d2c913ba4b25503e1d62`.
- Never download dependencies or datasets on the offline server; use `/opt/venv` and frozen `deps/3rd`.
- Compile with at most `-j96`; do not start evaluation while an existing build is active.
- Use `ef_search=64`, 4096 independent single-row SQL queries, and DDL plus first-query timing.
- Never modify hipVS/cuVS or seekdb kernel behavior for this tool.
- Keep datasets, traces, logs, binaries, and results out of Git.
- Stop only evaluator-owned processes whose command lines match the selected binary and base directory.
- Do not claim that local results guarantee Hidden250K or official scoring success.

---

### Task 1: Pure Metrics and Gates

**Files:**
- Create: `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`
- Create: `unittest/oblib/vector/test_contest_eval.py`

**Interfaces:**
- Produces: `recall_at_k(actual, truth)`, `validate_ids(actual, rows, k)`, `percentile(values, q)`, `paired_metrics(baseline, candidate)`, `evaluate_gates(comparison, resources, dataset_kind)`, `write_reports(result, output_dir)`.
- `actual`/`truth` are query-major ID sequences. Metric functions have no database dependency.

- [ ] **Step 1: Write failing metric tests**

```python
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py"
SPEC = importlib.util.spec_from_file_location("contest_eval", MODULE)
contest_eval = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contest_eval)

class MetricsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output_dir = Path(self.temp.name)

    def test_exact_recall_and_duplicate_rejection(self):
        self.assertEqual(contest_eval.recall_at_k([[1, 2]], [[2, 3]]), 0.5)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            contest_eval.validate_ids([[1, 1]], rows=10, k=2)
        with self.assertRaisesRegex(ValueError, "count"):
            contest_eval.validate_ids([[1]], rows=10, k=2)
        with self.assertRaisesRegex(ValueError, "range"):
            contest_eval.validate_ids([[10, 11]], rows=10, k=2)

    def test_percentile_and_pair_ratios(self):
        self.assertEqual(contest_eval.percentile([1, 2, 3, 4], 95), 3.85)
        comparison = contest_eval.paired_metrics(
            {"ready_seconds": 20, "qps": {10: 100, 100: 80},
             "recall": {10: 0.98, 100: 0.99}},
            {"ready_seconds": 10, "qps": {10: 95, 100: 88},
             "recall": {10: 0.977, 100: 0.986}})
        self.assertEqual(comparison["build_speedup"], 2.0)
        self.assertAlmostEqual(comparison["query_speedup"], (0.95 * 1.1) ** 0.5)
        self.assertEqual(comparison["recall_delta"], {10: 0.003, 100: 0.004})

    def test_official_like_gates_include_recall_qps_and_vram(self):
        comparison = {"recall": {10: 0.98, 100: 0.96},
                      "recall_delta": {10: 0.005, 100: 0.005},
                      "query_qps_ratio": {10: 0.90, 100: 0.95}}
        resources = {"candidate_vram_bytes": 125, "baseline_vram_bytes": 100,
                     "candidate_rss_bytes": 96 * 1024**3}
        gates = contest_eval.evaluate_gates(comparison, resources, "sift")
        self.assertTrue(all(item["passed"] for item in gates))
        comparison["recall_delta"][10] = 0.005001
        gates = contest_eval.evaluate_gates(comparison, resources, "sift")
        self.assertFalse(next(x for x in gates if x["name"] == "recall@10_baseline_delta")["passed"])

    def test_failed_evidence_cannot_pass(self):
        result = {"status": "RUNNING", "rounds": [], "errors": ["GPU evidence missing"]}
        contest_eval.write_reports(result, self.output_dir)
        self.assertEqual(json.loads((self.output_dir / "summary.json").read_text())["status"], "FAILED")
```

Run: `python3 -m unittest unittest/oblib/vector/test_contest_eval.py -v`
Expected: FAIL because `contest_eval.py` does not exist.

- [ ] **Step 2: Implement minimal pure metrics**

```python
def recall_at_k(actual, truth):
    if len(actual) != len(truth) or not truth:
        raise ValueError("query count mismatch")
    scores = []
    for got, expected in zip(actual, truth):
        if len(got) != len(expected) or len(set(got)) != len(got):
            raise ValueError("incomplete or duplicate ANN results")
        scores.append(len(set(got).intersection(expected)) / len(expected))
    return statistics.fmean(scores)


def validate_ids(actual, rows, k):
    for answer in actual:
        if len(answer) != k:
            raise ValueError("incorrect result count")
        if len(set(answer)) != k:
            raise ValueError("duplicate result ID")
        if any(not isinstance(value, int) or not 0 <= value < rows for value in answer):
            raise ValueError("result ID outside valid range")


def percentile(values, q):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty latency sample")
    position = (len(ordered) - 1) * q / 100
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)
```

Add `paired_metrics` using baseline/candidate division, absolute recall differences, and geometric mean. Add named gate dictionaries for absolute recall, `0.005` delta, `0.90` QPS, `1.25x` VRAM, `45 GiB` VRAM, and `96 GiB` RSS. `write_reports` writes JSON plus a Markdown table and changes failed/evidence-missing results to `FAILED` before writing.

- [ ] **Step 3: Run metric tests and commit**

Run: `python3 -m unittest unittest/oblib/vector/test_contest_eval.py -v`
Expected: all metric tests PASS.

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py \
  unittest/oblib/vector/test_contest_eval.py
git commit -m "test: add contest evaluation metrics and gates"
```

### Task 2: Deterministic Datasets and Exact Ground Truth

**Files:**
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`
- Modify: `unittest/oblib/vector/test_contest_eval.py`

**Interfaces:**
- Produces: `dataset_specs(mode)`, `generate_dataset(spec, cache_dir)`, `exact_topk(base, queries, k, chunk)`, `load_or_create_dataset(spec, cache_dir)`.
- Dataset result contains `base`, `queries`, `truth`, and `manifest`; all arrays have deterministic shapes and hashes.

- [ ] **Step 1: Write failing dataset tests**

```python
class DatasetTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.cache_dir = Path(self.temp.name)

    def test_full_matrix_has_required_sizes_and_dimensions(self):
        specs = contest_eval.dataset_specs("full")
        shapes = {(s.rows, s.dim) for s in specs}
        self.assertIn((250000, 128), shapes)
        self.assertIn((250000, 256), shapes)
        self.assertIn((250000, 384), shapes)
        self.assertIn((250000, 768), shapes)
        self.assertIn((1000000, 128), shapes)
        self.assertIn((1000000, 256), shapes)
        self.assertTrue(all(s.nq == 4096 for s in specs))

    def test_exact_topk_orders_by_l2_then_id(self):
        base = np.array([[0, 0], [1, 0], [-1, 0], [3, 0]], dtype=np.float32)
        queries = np.array([[0, 0]], dtype=np.float32)
        np.testing.assert_array_equal(contest_eval.exact_topk(base, queries, 3, 2), [[0, 1, 2]])

    def test_regeneration_is_deterministic_and_detects_bad_cache(self):
        spec = contest_eval.DatasetSpec("tiny", 256, 4, 16, "clustered", 7)
        first = contest_eval.load_or_create_dataset(spec, self.cache_dir)
        second = contest_eval.load_or_create_dataset(spec, self.cache_dir)
        np.testing.assert_array_equal(first.base, second.base)
        np.testing.assert_array_equal(first.truth, second.truth)
        self.assertEqual(first.manifest["base_sha256"], second.manifest["base_sha256"])
```

Run: `python3 -m unittest unittest/oblib/vector/test_contest_eval.py -v`
Expected: FAIL because dataset APIs do not exist.

- [ ] **Step 2: Implement deterministic generation and caching**

```python
@dataclasses.dataclass(frozen=True)
class DatasetSpec:
    name: str
    rows: int
    dim: int
    nq: int
    seed: int
    distribution: str


def exact_topk(base, queries, k, chunk=8):
    output = []
    for start in range(0, len(queries), chunk):
        block = queries[start:start + chunk].astype(np.float32)
        distances = ((block[:, None, :] - base[None, :, :]) ** 2).sum(axis=2)
        nearest = np.argpartition(distances, k - 1, axis=1)[:, :k]
        for row, ids in enumerate(nearest):
            ordered = sorted(ids.tolist(), key=lambda i: (float(distances[row, i]), i))
            output.append(ordered)
    return np.asarray(output, dtype=np.int32)
```

Generate normalized Gaussian data with `np.random.default_rng(seed)`. Generate clustered data with 64 centers, per-dimension scales, anisotropic within-cluster noise, and deterministic shifted/boundary queries. Save uncompressed `.npy` arrays and a manifest containing seed, distribution, shapes, dtypes, and SHA-256. On cache load, require exact manifest and array hashes; reject mismatches instead of silently regenerating a comparison dataset. Compute exact Top-100 in bounded query chunks and never allocate the full `4096 x rows` distance matrix.

- [ ] **Step 3: Run dataset tests and real small-data verification**

Run: `/opt/venv/bin/python -m unittest unittest/oblib/vector/test_contest_eval.py -v` on the server when SSH is available; locally run `python3` if NumPy is installed.
Expected: all tests PASS and tiny clustered data is byte-identical after regeneration.

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py \
  unittest/oblib/vector/test_contest_eval.py
git commit -m "feat: add deterministic contest evaluation datasets"
```

### Task 3: Isolated SQL Runner and Paired Execution

**Files:**
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`
- Modify: `unittest/oblib/vector/test_contest_eval.py`

**Interfaces:**
- Consumes: dataset arrays/manifests and pure metric functions from Tasks 1–2.
- Produces: `ServerConfig`, `ObserverServer`, `run_single(spec, data, server, output_dir)`, `run_pair(args)`, `main(argv)`.
- CLI: `contest_eval.py --mode quick|full --dataset NAME --baseline-observer PATH --candidate-observer PATH --baseline-bridge PATH --candidate-bridge PATH --output-dir PATH --data-cache PATH`.

- [ ] **Step 1: Write failing lifecycle and SQL-contract tests**

```python
class RunnerContractTest(unittest.TestCase):
    def test_official_index_sql_is_fixed(self):
        sql = contest_eval.create_index_sql("items", "cuvs")
        self.assertIn("distance=l2", sql)
        self.assertIn("type=hnsw", sql)
        self.assertIn("lib=cuvs", sql)
        self.assertIn("m=16", sql)
        self.assertIn("ef_construction=200", sql)
        self.assertIn("ef_search=64", sql)

    def test_observer_uses_private_runtime_and_trace(self):
        config = contest_eval.ServerConfig("candidate", Path("/opt/candidate/seekdb"),
            Path("/opt/candidate/bridge.so"), 2982, Path("/work/run/candidate"))
        command, env = contest_eval.observer_command(config)
        self.assertIn("--nodaemon", command)
        self.assertIn("--base-dir", command)
        self.assertIn(str(config.base_dir), command)
        self.assertEqual(env["OB_VSAG_TRACE"], "1")
        self.assertEqual(env["OB_VSAG_TRACE_FILE"], str(config.base_dir / "hipvs.trace"))

    def test_cleanup_requires_owned_binary_and_base_dir(self):
        self.assertFalse(contest_eval.owned_process_cmdline("/bin/other --base-dir /tmp/other",
            Path("/opt/candidate/seekdb"), Path("/work/run/candidate")))
        self.assertTrue(contest_eval.owned_process_cmdline(
            "/opt/candidate/seekdb --nodaemon --base-dir /work/run/candidate",
            Path("/opt/candidate/seekdb"), Path("/work/run/candidate")))
```

Run: `python3 -m unittest unittest/oblib/vector/test_contest_eval.py -v`
Expected: FAIL because runner APIs do not exist.

- [ ] **Step 2: Implement server lifecycle and data loading**

Use `subprocess.Popen` with `--nodaemon`, `--port`, `--data-dir`, `--base-dir`, `cpu_count=24`, and private trace/log paths. Set `LD_LIBRARY_PATH` to the selected bridge directory plus `/opt/hipvs/lib:/opt/rocm/lib`. Wait for `SELECT 1`, fail with log paths on timeout, and terminate only matching owned PIDs. Sample process RSS and `rocm-smi` VRAM/utilization on a background thread.

Load each dataset once per fresh server using `LOAD DATA LOCAL INFILE` from a deterministic TSV cache. Verify row count before DDL. Use one persistent mysql CLI process with autocommit and explicit query timeout so per-query process startup is not included. Start timing immediately before the fixed DDL; execute the first fixed query immediately afterward.

- [ ] **Step 3: Implement 4096-query execution and GPU evidence**

For Top-10 then Top-100, execute every query independently with `APPROXIMATE`, record wall latency and IDs, and calculate QPS/P50/P95/P99/Recall. Reject missing, duplicate, or out-of-range IDs. Require snapshot `built=true`, positive `cuvs_serve`, and absence of known CPU fallback markers in newly appended log/trace slices. Preserve raw answers and latency arrays in result files.

- [ ] **Step 4: Implement paired ordering and reports**

Run `V5 -> candidate` then `candidate -> V5` for each dataset. Keep data hashes identical, serialize every server before starting the next, and calculate per-round plus median build/query ratios. Include revisions, binary/bridge hashes, ROCm/hipVS environment, dataset manifest, resources, errors, and gate results. Support `--mode`, `--dataset`, `--rounds` (default 2), and `--resume` only for complete compatible dataset caches, never for partially completed SQL rounds.

- [ ] **Step 5: Run all unit tests and commit**

Run: `python3 -m unittest unittest/oblib/vector/test_contest_eval.py -v`
Expected: all unit tests PASS; no real service is needed for contract tests.

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py \
  unittest/oblib/vector/test_contest_eval.py
git commit -m "feat: add paired seekdb contest evaluation runner"
```

### Task 4: Documentation, Offline Regression, and Remote Evaluation

**Files:**
- Create: `docs/gpu-vector-index-hipvs-cuvs/validation/RUN_CONTEST_EVAL.md`

**Interfaces:**
- Documents complete V5/candidate build commands, quick/full invocations, artifact paths, status meanings, and limitations.

- [ ] **Step 1: Document reproducible execution**

Document separate V5/candidate worktrees and bridge paths, frozen dependency symlink, `-j96`, trace-enabled local builds, `/opt/venv/bin/python`, `quick`, `full`, individual dataset reruns, output files, and the fact that Hidden250K cannot be locally certified. Include commands to inspect current builds and avoid port/GPU conflicts.

- [ ] **Step 2: Run fresh local verification**

```bash
python3 -m unittest unittest/oblib/vector/test_contest_eval.py -v
python3 -m unittest unittest/oblib/vector/test_hipvs_benchmark.py -v
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
bash -n tools/hipvs_bridge/build.sh
git diff --check
```

Expected: all commands exit 0. Do not claim the existing adaptor component test passes unless its linker dependency is resolved and it is rerun successfully.

- [ ] **Step 3: Commit and push the tool**

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/RUN_CONTEST_EVAL.md
git commit -m "docs: add paired contest evaluation guide"
git push docker HEAD:hipvs-hnsw-build-fixes
```

Expected: remote branch advances without force-push. Confirm `/root/worker` contains the same revision after the user's sync mechanism completes.

- [ ] **Step 4: Verify the remote candidate build and build V5**

Wait for the existing candidate build to finish; check `build_release/src/observer/seekdb`. Create a detached V5 source directory at `634888d9b`, link the same frozen dependency cache, build its bridge and a trace-enabled `seekdb` with `-j96`. Do not overwrite candidate artifacts.

Expected: both binaries and both bridges exist; record hashes and revisions.

- [ ] **Step 5: Run quick and small paired evaluation**

Run existing offline hipVS tests, then `contest_eval.py --mode quick` against V5 and candidate. Inspect summary, GPU evidence, exact recall, QPS, latency, and cleanup. If quick fails, stop and diagnose before starting full evaluation.

Expected: quick summary has no missing evidence and no evaluator-owned service remains active.

- [ ] **Step 6: Run the full matrix and report evidence**

Run all eight datasets in two reversed-order rounds. Monitor disk, memory, GPU and logs without starting another GPU workload. Save all outputs under `/work/evals/<timestamp>/`. Report actual pass/fail tables, worst Hidden-like recall delta, build/query speedups, resource maxima, failures, and remaining official Hidden250K uncertainty. Do not convert synthetic-data results into an official score.

Expected: `summary.json` and `summary.md` contain every requested dataset and round; incomplete runs are marked `INCOMPLETE` or `FAILED`, never presented as passed.
