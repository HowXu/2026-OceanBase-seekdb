# GPU BruteForce / IVF-Flat Contest Backends Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add testable BruteForce and IVF-Flat hipVS backends behind the existing bridge ABI, archive current contest evidence, and evaluate them without 768-dimensional workloads.

**Architecture:** Keep CAGRA as the default and select experimental algorithms with `SEEKDB_CUVS_ALGO`. One tagged opaque handle dispatches build/search/serialization/free operations; the seekdb adaptor and SQL contract remain unchanged.

**Tech Stack:** C17 bridge, hipVS C API, C++ mock SDK tests with ASan/UBSan, Python unittest, paired seekdb SQL evaluation.

## Global Constraints

- Do not change default CAGRA parameters or the exported bridge ABI.
- Do not run 768-dimensional workloads.
- Do not use subagents or modify organization-owned hipVS.
- Use exact Recall and independent single-row SQL for formal comparisons; batch calls are calibration only.
- Do not claim local synthetic results certify Hidden250K.

---

### Task 1: Archive Evidence and Remove 768-D Workloads

**Files:**
- Create: `docs/gpu-vector-index-hipvs-cuvs/validation/results-contest-*-20261031.{json,md}`
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/contest_eval.py`
- Test: `unittest/oblib/vector/test_contest_eval.py`

- [ ] **Step 1: Add a failing matrix test**

Assert that the full matrix contains seven datasets, no specification has `dim == 768`, and the existing 128/256/384 and 1M cases remain.

- [ ] **Step 2: Verify RED**

Run `PYTHONDONTWRITEBYTECODE=1 python3 unittest/oblib/vector/test_contest_eval.py -q`; expect the new no-768/count assertion to fail.

- [ ] **Step 3: Remove only the 768-D specification**

Delete the `gaussian250k-d768` entry. Do not alter queries, ef_search, gates, or remaining datasets.

- [ ] **Step 4: Verify GREEN and archive**

Run the 17 evaluator tests, inspect the four archived report files, and commit the evidence plus matrix change with `test: archive contest baseline and drop 768d workloads`.

### Task 2: BruteForce Backend

**Files:**
- Create: `unittest/oblib/vector/hipvs_mock/cuvs/neighbors/brute_force.h`
- Modify: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`
- Modify: `unittest/oblib/vector/test_hipvs_bridge.cpp`

**Interfaces:**
- Consumes: installed `cuvsBruteForceBuild/Search/Serialize/Deserialize` C API.
- Produces: `SEEKDB_CUVS_ALGO=brute_force` through the unchanged `seekdb_cuvs_*` ABI.

- [ ] **Step 1: Write failing BruteForce contract cases**

Under the mock SDK, set `SEEKDB_CUVS_ALGO=brute_force`; build 256×4 vectors, verify exact Top-3 IDs/distances, serialize/free/deserialize/search, and verify all allocations are released. Add fault-injection calls for BruteForce build/search/load and an unknown-algorithm rejection case.

- [ ] **Step 2: Verify RED**

Run `bash unittest/oblib/vector/run_hipvs_bridge_tests.sh`; expect compilation or assertions to fail because the BruteForce mock/API dispatch is absent.

- [ ] **Step 3: Implement the minimal tagged-handle dispatch**

Add `SK_ALGO_CAGRA`, `SK_ALGO_BRUTE_FORCE`, and a BruteForce pointer. Parse the environment at build/deserialize. Implement BruteForce build/search/serialize/deserialize/free using the existing tensor, allocation, stream, and error paths. Unknown names return `NULL` and log `unknown algorithm`.

- [ ] **Step 4: Verify GREEN**

Run the bridge script under ASan/UBSan, then run it again with default CAGRA to show existing cases remain valid. Commit with `feat: add exact GPU brute-force contest backend`.

### Task 3: IVF-Flat Backend

**Files:**
- Create: `unittest/oblib/vector/hipvs_mock/cuvs/neighbors/ivf_flat.h`
- Modify: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`
- Modify: `unittest/oblib/vector/test_hipvs_bridge.cpp`

**Interfaces:**
- Consumes: `cuvsIvfFlatIndexParamsCreate/Build/SearchParamsCreate/Search/Serialize/Deserialize`.
- Produces: `SEEKDB_CUVS_ALGO=ivf_flat` plus `SEEKDB_CUVS_IVF_N_LISTS` and `SEEKDB_CUVS_IVF_N_PROBES` overrides.

- [ ] **Step 1: Write failing IVF-Flat contract cases**

Verify default `n_lists=min(512,max(1,n/128))` and `n_probes=min(32,n_lists)`, explicit valid overrides, zero/negative/non-numeric/`n_probes>n_lists` rejection, exact mock Top-K, reload, and cleanup at every new failure point.

- [ ] **Step 2: Verify RED**

Run the bridge script; expect missing IVF-Flat declarations or dispatch failures.

- [ ] **Step 3: Implement the minimal IVF-Flat path**

Create index/search params, set L2Expanded, `add_data_on_build=true`, 10 k-means iterations, train fraction 0.25, conservative allocation, parsed list/probe values, and dispatch search/serialization/free. Reject invalid overrides before calling the SDK.

- [ ] **Step 4: Verify GREEN and full offline regression**

Run bridge tests and all Python evaluator/benchmark tests. Commit with `feat: add GPU IVF-Flat contest backend`.

### Task 4: Usage Notes and New-Server Evaluation

**Files:**
- Modify: `docs/gpu-vector-index-hipvs-cuvs/README.md`
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/RUN_CONTEST_EVAL.md`

- [ ] **Step 1: Document exact experimental invocations**

Document default CAGRA compatibility, `SEEKDB_CUVS_ALGO=brute_force` and `ivf_flat`, IVF overrides, matching algorithm environment on snapshot reload, and the absence of 768-D cases.

- [ ] **Step 2: Verify documentation and commit**

Run `git diff --check`, all offline tests, and commit with `docs: describe exact and IVF contest evaluation`.

- [ ] **Step 3: Build and test on the new server**

Using the supplied new SSH target, install no dependencies, reuse its existing ROCm/hipVS/toolchain, build with at most `-j96`, and run quick followed by the seven-dataset matrix separately for BruteForce and IVF-Flat. Preserve each report and compare Recall delta, QPS ratio, build speedup and resources against the same V5 binary.
