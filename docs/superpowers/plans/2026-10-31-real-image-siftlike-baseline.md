# Real-Image SIFT-like Dataset and V5 Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generate a deterministic real-image SIFT-like 250K×128 dataset, transfer it to the remote cache, and archive four valid frozen-V5 baseline rounds.

**Architecture:** A Pillow/NumPy descriptor generator creates smoothed 4×4×8 gradient histograms from high-energy image patches. Existing contest cache manifests and exact-L2 truth provide reproducibility; the existing isolated `run_single` path measures V5 without changing its binary. CAGRA optimization is a separate later task.

**Tech Stack:** Python 3, NumPy, Pillow, contest_eval.py, unittest, frozen V5 seekdb/hipVS, SSH/SCP.

## Global Constraints

- Execute inline without subagents; remote compilation, if needed, is limited to `-j48`.
- Do not install dependencies or upload original Pictures images.
- The dataset is a surrogate, not official SIFT250K; FP32+L2 and existing SQL/evidence semantics remain fixed.
- Do not change the frozen V5 source/binary or count evidence-incomplete rounds in averages.
- Keep generated arrays outside Git; commit generator tests and JSON/Markdown reports only.

---

### Task 1: Add and Test Real-Image Descriptors

**Files:**
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`
- Test: `unittest/oblib/vector/test_contest_eval.py`

**Interfaces:**
- Produces: `siftlike_descriptor(patch: np.ndarray) -> np.ndarray | None`, returning a normalized float32 vector of length 128 or `None` for a flat/invalid patch.
- Produces: `siftlike_arrays(spec, images_root) -> tuple[np.ndarray, np.ndarray]` for base/query generation.

- [ ] **Step 1: Write failing descriptor tests**

Add tests using a 32×32 horizontal-gradient patch and a zero patch:

```python
patch = np.tile(np.linspace(0, 1, 32, dtype=np.float32), (32, 1))
descriptor = contest_eval.siftlike_descriptor(patch)
self.assertEqual(descriptor.shape, (128,))
self.assertEqual(descriptor.dtype, np.float32)
self.assertTrue(np.all(descriptor >= 0))
self.assertAlmostEqual(float(np.linalg.norm(descriptor)), 1.0, places=5)
self.assertIsNone(contest_eval.siftlike_descriptor(np.zeros((32, 32), dtype=np.float32)))
```

Also generate the same descriptor twice and assert byte equality.

- [ ] **Step 2: Verify RED**

Run `PYTHONDONTWRITEBYTECODE=1 python3 unittest/oblib/vector/test_contest_eval.py -q`.
Expected: failure because `siftlike_descriptor` does not exist.

- [ ] **Step 3: Implement descriptor and patch sampling**

Implement grayscale FP32 patches resized to 32×32, separable `[1,4,6,4,1]/16` smoothing, x/y gradients, 4×4 cells and eight orientation bins with a 32×32 Gaussian weight. Apply L2 normalization, clipping at 0.2 and renormalization. Reject non-32×32 input, non-finite data, low gradient RMS and zero descriptor norm.

Implement deterministic patch metadata sampling from decoded images at least 128×128, scales `{32,48,64,96}`, high-energy retry, 250000 base rows, 70% held-out perturbed query patches and 30% independent query patches. `siftlike_arrays` returns `(base, queries)` as float32 and dispatches through `generate_arrays` for `spec.distribution == "siftlike"`.

- [ ] **Step 4: Verify GREEN and commit**

Run the evaluator tests and `git diff --check`; expect all tests OK. Commit:

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py unittest/oblib/vector/test_contest_eval.py
git commit -m "test: add real-image SIFT-like descriptor generator"
```

### Task 2: Generate and Validate the Local 250K Cache

**Files:**
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`
- Test: `unittest/oblib/vector/test_contest_eval.py`
- Create outside Git: `/home/howxu/eval-data/siftlike250k-d128/{base,queries,truth}.npy` and `manifest.json`

**Interfaces:**
- Consumes: `siftlike_arrays` from Task 1.
- Produces: DatasetSpec `siftlike250k-d128` with seed `20261109`, recognized by cache loading and SIFT-like gates.

- [ ] **Step 1: Add failing spec and cache tests**

Assert the full specs contain `DatasetSpec("siftlike250k-d128", 250000, 128, 4096, "siftlike", 20261109)` and `dataset_kind("siftlike250k-d128") == "sift"`. Update the expected matrix count without removing existing datasets.

- [ ] **Step 2: Verify RED, implement spec, then GREEN**

Run the evaluator tests and confirm the new spec assertions fail. Add the spec/dispatch/kind mapping and rerun; expect all tests OK.

- [ ] **Step 3: Generate local arrays and exact truth**

Run with local BLAS threads bounded:

```bash
OPENBLAS_NUM_THREADS=24 PYTHONDONTWRITEBYTECODE=1 python3 - <<'PY'
import importlib.util, sys
from pathlib import Path
p=Path('docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py')
s=importlib.util.spec_from_file_location('contest_eval',p)
m=importlib.util.module_from_spec(s);sys.modules[s.name]=m;s.loader.exec_module(m)
spec=next(x for x in m.dataset_specs('full') if x.name=='siftlike250k-d128')
data=m.load_or_create_dataset(spec,Path('/home/howxu/eval-data'))
print(data.base.shape,data.base.dtype,data.queries.shape,data.truth.shape,data.manifest)
PY
```

Expected: `(250000,128) float32`, `(4096,128)`, `(4096,100)` and no hash errors. Assert finite/nonnegative/unit-norm base and queries, truth IDs in `[0,250000)`, and exactly 100 IDs per query.

- [ ] **Step 4: Commit spec metadata changes**

Commit only source/tests, not `.npy` files:

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py unittest/oblib/vector/test_contest_eval.py
git commit -m "test: register SIFT-like 250K dataset cache"
```

### Task 3: Transfer Cache and Run Four Valid V5 Rounds

**Files:**
- Create outside Git on remote: `/work/evals/datasets/siftlike250k-d128/`
- Create outside Git on remote: `/work/evals/v5-siftlike-four-*/`

**Interfaces:**
- Consumes: local cache and frozen V5 observer/bridge already built.
- Produces: exactly four valid runs for the single SIFT-like dataset and one arithmetic average.

- [ ] **Step 1: Transfer and independently verify the cache**

Use `scp -r` to `/work/evals/datasets/siftlike250k-d128/`. On remote, call `load_or_create_dataset` and print all array shapes and manifest hashes; a mismatch is a hard stop.

- [ ] **Step 2: Run four V5 rounds**

After no other observer/build is active, run:

```bash
RUN=/work/evals/v5-siftlike-four-$(date +%Y%m%d-%H%M%S)
OPENBLAS_NUM_THREADS=24 env -u SEEKDB_CUVS_ALGO /opt/venv/bin/python /work/run_four_average.py \
  --role baseline --revision 634888d9b037153fa143d2c913ba4b25503e1d62 \
  --observer /root/worker-v5/build_eval/src/observer/seekdb \
  --bridge /work/eval-bin/v5/libseekdb_cuvs_bridge.so \
  --cache /work/evals/datasets --output-dir "$RUN" \
  --rounds 4 --dataset siftlike250k-d128
```

Expected: four `DONE` lines and `COMPLETE`. If evidence is incomplete, exclude that run and start a fresh replacement output directory as done for the gaussian baseline.

- [ ] **Step 3: Retrieve the raw summary**

SCP the final `summary.json` locally. Verify four runs, all `evidence.passed=true`, no fallback, and average fields for ready/QPS/Recall.

### Task 4: Archive the Frozen SIFT-like Baseline

**Files:**
- Create: `docs/gpu-vector-index-hipvs-cuvs/validation/results-contest-v5-siftlike-four-20261031.json`
- Create: `docs/gpu-vector-index-hipvs-cuvs/validation/results-contest-v5-siftlike-four-20261031.md`

**Interfaces:**
- Produces: immutable baseline for later CAGRA optimization comparisons.

- [ ] **Step 1: Write JSON and Markdown reports**

Include V5 revision, dataset manifest hashes, all four raw runs, average ready/QPS/Recall, remote report path, surrogate warning, external `23s、147/143 QPS、Recall≈1` reference, and enhanced reference `20.9091s、161.7/157.3 QPS`. Record any discarded evidence-incomplete attempts.

- [ ] **Step 2: Run full offline regression**

Run bridge, adaptor component, evaluator, benchmark tests and `git diff --check`. Expect bridge/component PASS, evaluator and benchmark OK, no whitespace errors.

- [ ] **Step 3: Commit and push**

```bash
git add docs/gpu-vector-index-hipvs-cuvs/validation/results-contest-v5-siftlike-four-20261031.*
git commit -m "test: archive SIFT-like V5 four-round baseline"
git push origin hipvs-exact-ivf-contest
```

After this baseline is frozen, start a separate CAGRA-only design/plan; do not use BruteForce or IVF query implementations in subsequent candidates.
