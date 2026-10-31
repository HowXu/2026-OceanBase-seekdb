# GPU BF Snapshot and Query Resource Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove duplicate BruteForce snapshot payloads and reuse per-handle GPU query resources without changing algorithm parameters or the existing thread model.

**Architecture:** The bridge owns cached search params and query/output buffers, growing them only when a request exceeds current capacity. OBCUVS02 uses `graph_bytes=0` for new BruteForce snapshots; the existing outer IDs/vectors remain the sole payload and are used to rebuild BF or recover through VSAG. CAGRA, IVF-Flat and nonzero-graph snapshots keep their existing graph path.

**Tech Stack:** C11 bridge, C++20 adaptor, hipVS C API, mock hipVS C++ tests, Python component-test driver, ASan/UBSan.

## Global Constraints

- Execute inline in the current session; do not use subagents.
- Do not change CAGRA/IVF defaults, IVF `n_lists/n_probes`, query threading, external uint32 result ABI or VSAG fallback semantics.
- Build and load must use the same `SEEKDB_CUVS_ALGO`; no cross-version promise for zero-graph experimental snapshots.
- No remote GPU is available: offline PASS is not a performance claim. Real 250K Recall/QPS/resource validation remains required.
- Keep changes on `hipvs-exact-ivf-contest`; do not alter the shared `hipvs-hnsw-build-fixes` branch.

---

### Task 1: Cache Query Resources in the Bridge Handle

**Files:**
- Modify: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`
- Test: `unittest/oblib/vector/test_hipvs_bridge.cpp`

**Interfaces:**
- Consumes: existing `seekdb_cuvs_build`, `seekdb_cuvs_search`, `seekdb_cuvs_free` ABI.
- Produces: unchanged public ABI; internal cached buffers/params with capacity measured in allocated bytes.

- [ ] **Step 1: Add failing allocation/params reuse assertions**

Add mock counters and increment them only after successful creates/allocations:

```cpp
static int rmm_allocs, cagra_search_param_creates, ivf_search_param_creates;
```

For each of CAGRA, BF and IVF, build a 256-row index and assert this sequence (adjust `after_build` for BF's retained base allocation):

```cpp
const int after_build = rmm_allocs;
assert(seekdb_cuvs_search(h, query.data(), 1, 10, out.data(), dist.data()) == 0);
assert(rmm_allocs == after_build + 3);
assert(seekdb_cuvs_search(h, query.data(), 1, 10, out.data(), dist.data()) == 0);
assert(rmm_allocs == after_build + 3);
assert(seekdb_cuvs_search(h, query.data(), 1, 100, out100.data(), dist100.data()) == 0);
assert(rmm_allocs == after_build + 5);  // IDs + distances grow; query does not.
assert(seekdb_cuvs_search(h, query.data(), 1, 10, out.data(), dist.data()) == 0);
assert(rmm_allocs == after_build + 5);
```

Assert CAGRA/IVF search params are created once across these four searches. Keep existing `buffers.empty()` and `live == 0` checks after `seekdb_cuvs_free(h)` to prove cached resources are released.

- [ ] **Step 2: Run the bridge test and verify RED**

Run:

```bash
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
```

Expected: an allocation-count assertion fails because the current search allocates three device buffers on every call.

- [ ] **Step 3: Implement capacity-based device/host caching**

Extend `sk_cuvs_index` with query/result device pointers and byte capacities, host wide-ID pointer/capacity, and cached search params. Add an internal helper with this behavior:

```c
static int grow_device_buffer(cuvsResources_t res, void** pointer,
                              size_t* capacity, size_t required)
{
  void* replacement = NULL;
  if (required <= *capacity) return 0;
  if (cuvsRMMAlloc(res, &replacement, required) != CUVS_SUCCESS) return 1;
  if (*pointer) cuvsRMMFree(res, *pointer, *capacity);
  *pointer = replacement;
  *capacity = required;
  return 0;
}
```

Use the handle's cached `qd/nd/dd` in `seekdb_cuvs_search`; allocate params only when the corresponding cached pointer is null. Grow `wide_ids` with a temporary `realloc` pointer. Remove only per-search frees/destroys; retain the existing error drain and all tensor/copy/search logic.

- [ ] **Step 4: Release cached resources in `seekdb_cuvs_free`**

After the existing stream drain, destroy cached search params and free each device buffer using its stored allocation capacity, then free `wide_ids`. Preserve destruction of index params, BF retained base, indexes and resources. A partially grown handle must remain safe to free.

- [ ] **Step 5: Run bridge regression and commit**

Run:

```bash
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
```

Expected: `PASS bridge contract and fault-injection cases=40 (mock SDK; no GPU performance claim)` or a higher count if assertions are counted separately, with ASan/UBSan and leak checks clean.

Commit:

```bash
git add tools/hipvs_bridge/seekdb_cuvs_bridge.c unittest/oblib/vector/test_hipvs_bridge.cpp
git commit -m "perf: reuse hipVS query resources per index"
```

### Task 2: Store One Base Copy in New BruteForce Snapshots

**Files:**
- Modify: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`
- Modify: `src/oblib/lib/vector/ob_vsag_adaptor.cpp`
- Test: `unittest/oblib/vector/run_hipvs_adaptor_component.py`

**Interfaces:**
- Consumes: cached build/search handle from Task 1 and existing OBCUVS02 outer snapshot fields.
- Produces: `extern "C" int seekdb_cuvs_raw_snapshot(void* handle)`, returning `1` only for a valid BF handle and `0` otherwise; non-CUVS stub returns `0`.

- [ ] **Step 1: Add a failing real-adaptor raw-snapshot component case**

Extend the GPU-enabled mock bridge with a `raw_snapshot_mode` flag and call counters. In raw mode, return a non-null opaque build handle, return `1` from `seekdb_cuvs_raw_snapshot`, and count calls without creating real GPU resources.

After the existing fallback cases, add a GPU-enabled test that inserts `n=256` vectors and serializes them. Assert:

```cpp
const std::string raw = raw_stream.str();
uint64_t graph_bytes = UINT64_MAX;
std::memcpy(&graph_bytes, raw.data() + 8, sizeof(graph_bytes));
assert(raw.substr(0, 8) == "OBCUVS02");
assert(graph_bytes == 0);
assert(raw.size() == 32 + n * sizeof(int64_t) + n * dim * sizeof(float));
assert(serialize_calls == 0);
```

Reset the stream and deserialize it. Assert `build_calls` increases, `deserialize_calls` remains zero, and the index count is `n`. Truncate the raw payload by one byte and assert deserialization fails. With raw mode disabled, load a complete zero-graph snapshot and assert successful VSAG recovery with count `n`.

- [ ] **Step 2: Run the component test and verify RED**

Run:

```bash
python3 unittest/oblib/vector/run_hipvs_adaptor_component.py
```

Expected: GPU-enabled compilation fails because `seekdb_cuvs_raw_snapshot` is not defined, or the graph-bytes assertion fails while the old serializer writes a graph.

- [ ] **Step 3: Add the bridge predicate and BF raw serialization branch**

Implement in the bridge and declare in both OB_BUILD_CUVS and stub sections:

```c
int seekdb_cuvs_raw_snapshot(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  return h && h->algorithm == SK_BRUTE_FORCE ? 1 : 0;
}
```

In `ob_cuvs_serialize`, build a stale/missing index exactly as before, then call the predicate. For BF set `graph_bytes=0` and skip graph temp-file creation, `seekdb_cuvs_serialize`, and graph copying. For other algorithms retain the existing nonzero-graph code. Allocate `ObCuvsSnapshotFile` with `std::unique_ptr` only on graph paths so BF does not even create a temporary file.

- [ ] **Step 4: Rebuild BF on zero-graph load and preserve fallback**

Allow zero in the OBCUVS02 header validation. After reading outer IDs/vectors:

```cpp
std::shared_ptr<void> loaded;
if (graph_bytes == 0) {
  if (ob_cuvs_enabled()) {
    loaded = ob_cuvs_build_big_stack(ent->buf_vecs_.data(),
                                     static_cast<long>(n), ent->dim_);
  }
} else {
  // Existing temp-file copy, big-stack deserialize and shape checks.
}
```

If loading is disabled or `loaded` is null, swap IDs/vectors into the existing VSAG fallback outputs and return `OB_SUCCESS`. Otherwise assign `ent->bridge_`, `built_n_` and `ids_`, register/mark the handle, and preserve the existing evidence log string. Nonzero graph loading and truncated-graph errors must remain unchanged.

- [ ] **Step 5: Run focused regressions and commit**

Run:

```bash
python3 unittest/oblib/vector/run_hipvs_adaptor_component.py
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
```

Expected: component prints its PASS line for CPU and GPU-enabled variants; bridge prints PASS with clean sanitizers.

Commit:

```bash
git add tools/hipvs_bridge/seekdb_cuvs_bridge.c src/oblib/lib/vector/ob_vsag_adaptor.cpp unittest/oblib/vector/run_hipvs_adaptor_component.py
git commit -m "perf: store BruteForce snapshot base only once"
```

### Task 3: Full Offline Verification and Validation Handoff

**Files:**
- Modify: `docs/gpu-vector-index-hipvs-cuvs/README.md`
- Modify: `docs/gpu-vector-index-hipvs-cuvs/validation/RUN_CONTEST_EVAL.md`

**Interfaces:**
- Consumes: Tasks 1–2 behavior.
- Produces: reproducible offline evidence and explicit next-server A/B requirements.

- [ ] **Step 1: Document the experimental snapshot/cache behavior**

State that new BF snapshots use `graph_bytes=0` and rebuild from their single outer base copy, require the same algorithm environment, and are not promised to load in older binaries. State that query buffers/params are handle-owned and released at index destruction; do not claim a measured speedup.

- [ ] **Step 2: Run all offline checks**

Run:

```bash
bash unittest/oblib/vector/run_hipvs_bridge_tests.sh
python3 unittest/oblib/vector/run_hipvs_adaptor_component.py
PYTHONDONTWRITEBYTECODE=1 python3 unittest/oblib/vector/test_contest_eval.py -q
PYTHONDONTWRITEBYTECODE=1 python3 unittest/oblib/vector/test_hipvs_benchmark.py -q
git diff --check
```

Expected: bridge/component PASS, evaluator 17 tests OK, benchmark 11 tests OK, no whitespace errors. Record that these are mock/component results, not GPU performance evidence.

- [ ] **Step 3: Commit documentation and push the experimental branch**

```bash
git add docs/gpu-vector-index-hipvs-cuvs/README.md docs/gpu-vector-index-hipvs-cuvs/validation/RUN_CONTEST_EVAL.md
git commit -m "docs: record BF raw snapshots and query cache validation"
git push origin hipvs-exact-ivf-contest
```

When a server becomes available, compare the resulting candidate against the archived V5 baseline for BF and IVF separately. Require real GPU trace, complete IDs, Recall delta at most `0.005`, QPS ratio at least `0.90`, and resource gates; do not substitute these offline checks for that run.
