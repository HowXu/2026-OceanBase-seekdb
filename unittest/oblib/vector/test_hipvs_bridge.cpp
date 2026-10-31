// Exercise the real bridge against a queued, failure-injectable C API test double.
#include "seekdb_cuvs_bridge.h"
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/brute_force.h>
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <vector>

struct Resource { std::vector<std::function<void()>> pending; };
struct Index { int64_t n = 0, dim = 0; std::vector<float> base; };
static Index saved;
static int calls, fail_at, live;
static bool cagra_include_dataset;
static int from_args_calls;
static size_t cagra_last_niter, cagra_last_graph_degree, cagra_last_intermediate_degree;
static size_t cagra_last_itopk, cagra_last_search_width;
static int cagra_last_build_algo, cagra_last_search_algo;
static unsigned ivf_last_nprobes, ivf_last_nlists, ivf_last_batch;
static int ivf_last_lut, ivf_last_internal, ivf_last_coarse;
static float ivf_last_refinement;
static bool ivf_last_conservative;
static int rmm_allocs, cagra_search_param_creates;
static int bf_builds, bf_searches, bf_saves, bf_loads;
static const void* bf_base_ptr;
static std::set<void*> buffers;
static bool fail() { return ++calls == fail_at; }
static Resource* resource(cuvsResources_t r) { return reinterpret_cast<Resource*>(r); }
static Index* index(cuvsCagraIndex_t i) { return static_cast<Index*>(i->data); }
static Index* bf_index(cuvsBruteForceIndex_t i) { return reinterpret_cast<Index*>(i->addr); }
extern "C" {
const char* cuvsGetLastErrorText() { return "injected failure"; }
cuvsError_t cuvsResourcesCreate(cuvsResources_t* r) {
  if (fail()) return CUVS_ERROR;
  *r = reinterpret_cast<uintptr_t>(new Resource); ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsResourcesDestroy(cuvsResources_t r) {
  assert(resource(r)->pending.empty()); delete resource(r); --live; return CUVS_SUCCESS;
}
cuvsError_t cuvsDeviceIdGet(cuvsResources_t, int* id) {
  if (fail()) return CUVS_ERROR; *id = 3; return CUVS_SUCCESS;
}
cuvsError_t cuvsStreamGet(cuvsResources_t r, cudaStream_t* s) {
  if (fail()) return CUVS_ERROR; *s = resource(r); return CUVS_SUCCESS;
}
cuvsError_t cuvsStreamSync(cuvsResources_t r) {
  bool error = fail();
  for (auto& job : resource(r)->pending) job();
  resource(r)->pending.clear(); return error ? CUVS_ERROR : CUVS_SUCCESS;
}
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void** p, size_t bytes) {
  if (fail()) return CUVS_ERROR;
  *p = std::malloc(bytes); assert(*p); buffers.insert(*p); ++rmm_allocs; return CUVS_SUCCESS;
}
cuvsError_t cuvsRMMFree(cuvsResources_t r, void* p, size_t) {
  assert(resource(r)->pending.empty()); assert(buffers.erase(p)); std::free(p); return CUVS_SUCCESS;
}
cudaError_t cudaMemcpyAsync(void* dst, const void* src, size_t bytes, cudaMemcpyKind, cudaStream_t s) {
  if (fail()) return cudaErrorUnknown;
  assert(s); static_cast<Resource*>(s)->pending.push_back([=] { std::memcpy(dst, src, bytes); });
  return cudaSuccess;
}
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsCagraIndexParams{}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { delete p; --live; return CUVS_SUCCESS; }
cuvsError_t cuvsIvfPqIndexParamsCreate(cuvsIvfPqIndexParams_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsIvfPqIndexParams{}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfPqIndexParamsDestroy(cuvsIvfPqIndexParams_t p) { delete p; --live; return CUVS_SUCCESS; }
cuvsError_t cuvsIvfPqSearchParamsCreate(cuvsIvfPqSearchParams_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsIvfPqSearchParams{}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfPqSearchParamsDestroy(cuvsIvfPqSearchParams_t p) { delete p; --live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t* p) {
  if (fail()) return CUVS_ERROR;
  *p = new cuvsCagraSearchParams{}; ++live; ++cagra_search_param_creates; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { delete p; --live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsCagraIndex{new Index}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { delete index(p); delete p; --live; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraBuild(cuvsResources_t r, cuvsCagraIndexParams_t p, DLManagedTensor* t, cuvsCagraIndex_t i) {
  if (fail()) return CUVS_ERROR;
  assert(!t->manager_ctx && !t->deleter && !t->dl_tensor.strides);
  assert(p->metric == L2Expanded && p->graph_degree < p->intermediate_graph_degree);
  cagra_last_graph_degree = p->graph_degree;
  cagra_last_intermediate_degree = p->intermediate_graph_degree;
  int64_t n = t->dl_tensor.shape[0], dim = t->dl_tensor.shape[1];
  cagra_last_niter = p->nn_descent_niter;
  cagra_last_build_algo = p->build_algo;
  if (p->graph_build_params) {
    ivf_last_nprobes = p->graph_build_params->ivf_pq_search_params->n_probes;
    ivf_last_nlists = p->graph_build_params->ivf_pq_build_params->n_lists;
    ivf_last_lut = p->graph_build_params->ivf_pq_search_params->lut_dtype;
    ivf_last_internal = p->graph_build_params->ivf_pq_search_params->internal_distance_dtype;
    ivf_last_coarse = p->graph_build_params->ivf_pq_search_params->coarse_search_dtype;
    ivf_last_batch = p->graph_build_params->ivf_pq_search_params->max_internal_batch_size;
    ivf_last_refinement = p->graph_build_params->refinement_rate;
    ivf_last_conservative = p->graph_build_params->ivf_pq_build_params->conservative_memory_allocation;
  }
  assert(p->graph_degree > 0 && p->nn_descent_niter > 0);
  assert(p->intermediate_graph_degree < static_cast<size_t>(n));
  auto data = static_cast<float*>(t->dl_tensor.data);
  resource(r)->pending.push_back([=] { index(i)->n = n; index(i)->dim = dim;
    index(i)->base.assign(data, data + n * dim); });
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearch(cuvsResources_t r, cuvsCagraSearchParams_t p, cuvsCagraIndex_t i,
    DLManagedTensor* q, DLManagedTensor* ids, DLManagedTensor* dist, cuvsFilter) {
  if (fail()) return CUVS_ERROR;
  assert(q->dl_tensor.device.device_id == 3 && ids->dl_tensor.device.device_id == 3);
  assert(!q->manager_ctx && !ids->deleter);
  auto query = static_cast<float*>(q->dl_tensor.data);
  auto output = static_cast<unsigned*>(ids->dl_tensor.data);
  auto distances = static_cast<float*>(dist->dl_tensor.data);
  auto nq = q->dl_tensor.shape[0], k = ids->dl_tensor.shape[1];
  cagra_last_itopk = p->itopk_size; cagra_last_search_width = p->search_width;
  cagra_last_search_algo = p->algo;
  assert(p->itopk_size % 32 == 0 && p->itopk_size >= static_cast<size_t>(k));
  if (p->algo == SINGLE_CTA) assert(p->itopk_size <= 512);
  resource(r)->pending.push_back([=] {
    for (int64_t row = 0; row < nq; ++row) {
      std::vector<std::pair<float, unsigned>> all;
      for (int64_t b = 0; b < index(i)->n; ++b) {
        float d = 0;
        for (int64_t col = 0; col < index(i)->dim; ++col) {
          float delta = query[row * index(i)->dim + col] - index(i)->base[b * index(i)->dim + col];
          d += delta * delta;
        }
        all.emplace_back(d, b);
      }
      std::sort(all.begin(), all.end());
      for (int64_t col = 0; col < k; ++col) {
        distances[row*k+col] = all[col].first; output[row*k+col] = all[col].second;
      }
    }
  });
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char*, cuvsCagraIndex_t i, bool include_dataset) {
  if (fail()) return CUVS_ERROR;
  cagra_include_dataset = include_dataset; saved = *index(i); return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraDeserialize(cuvsResources_t r, const char*, cuvsCagraIndex_t i) {
  if (fail()) return CUVS_ERROR;
  resource(r)->pending.push_back([=] {
    *index(i) = saved;
    if (!cagra_include_dataset) index(i)->dim = 0;
  });
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t i, int64_t* n) {
  if (fail()) return CUVS_ERROR; *n = index(i)->n; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t i, int64_t* dim) {
  if (fail()) return CUVS_ERROR; *dim = index(i)->dim; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetGraph(cuvsCagraIndex_t i, DLManagedTensor* graph) {
  if (fail()) return CUVS_ERROR;
  static uint32_t neighbors = 0;
  static int64_t shape[2];
  shape[0] = index(i)->n; shape[1] = 128;
  *graph = {};
  graph->dl_tensor.data = &neighbors;
  graph->dl_tensor.device.device_type = kDLCUDA;
  graph->dl_tensor.device.device_id = 3;
  graph->dl_tensor.ndim = 2;
  graph->dl_tensor.dtype.code = kDLUInt;
  graph->dl_tensor.dtype.bits = 32;
  graph->dl_tensor.dtype.lanes = 1;
  graph->dl_tensor.shape = shape;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexFromArgs(cuvsResources_t r, cuvsDistanceType metric,
    DLManagedTensor* graph, DLManagedTensor* dataset, cuvsCagraIndex_t i) {
  if (fail()) return CUVS_ERROR;
  assert(metric == L2Expanded && graph->dl_tensor.data);
  assert(dataset->dl_tensor.device.device_type == kDLCUDA && dataset->dl_tensor.device.device_id == 3);
  int64_t n = dataset->dl_tensor.shape[0], dim = dataset->dl_tensor.shape[1];
  auto data = static_cast<float*>(dataset->dl_tensor.data);
  resource(r)->pending.push_back([=] { index(i)->n = n; index(i)->dim = dim;
    index(i)->base.assign(data, data + n * dim); });
  ++from_args_calls; return CUVS_SUCCESS;
}
cuvsError_t cuvsBruteForceIndexCreate(cuvsBruteForceIndex_t* out) {
  if (fail()) return CUVS_ERROR;
  *out = new cuvsBruteForceIndex{reinterpret_cast<uintptr_t>(new Index), {}}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsBruteForceIndexDestroy(cuvsBruteForceIndex_t i) {
  delete bf_index(i); delete i; --live; return CUVS_SUCCESS;
}
cuvsError_t cuvsBruteForceBuild(cuvsResources_t r, DLManagedTensor* t, cuvsDistanceType metric,
                                float, cuvsBruteForceIndex_t i) {
  if (fail()) return CUVS_ERROR;
  assert(metric == L2Expanded);
  assert(t->dl_tensor.device.device_type == kDLCUDA && t->dl_tensor.device.device_id == 3);
  bf_base_ptr = t->dl_tensor.data;
  int64_t n = t->dl_tensor.shape[0], dim = t->dl_tensor.shape[1];
  auto data = static_cast<float*>(t->dl_tensor.data);
  resource(r)->pending.push_back([=] { bf_index(i)->n = n; bf_index(i)->dim = dim;
    bf_index(i)->base.assign(data, data + n * dim); });
  ++bf_builds; return CUVS_SUCCESS;
}
cuvsError_t cuvsBruteForceSearch(cuvsResources_t r, cuvsBruteForceIndex_t i,
    DLManagedTensor* q, DLManagedTensor* ids, DLManagedTensor* dist, cuvsFilter) {
  if (fail()) return CUVS_ERROR;
  assert(ids->dl_tensor.dtype.code == kDLInt && ids->dl_tensor.dtype.bits == 64);
  auto query = static_cast<float*>(q->dl_tensor.data);
  auto output = static_cast<int64_t*>(ids->dl_tensor.data);
  auto distances = static_cast<float*>(dist->dl_tensor.data);
  auto nq = q->dl_tensor.shape[0], k = ids->dl_tensor.shape[1];
  resource(r)->pending.push_back([=] {
    for (int64_t row = 0; row < nq; ++row) {
      std::vector<std::pair<float, unsigned>> all;
      for (int64_t b = 0; b < bf_index(i)->n; ++b) {
        float d = 0;
        for (int64_t col = 0; col < bf_index(i)->dim; ++col) {
          float delta = query[row * bf_index(i)->dim + col] - bf_index(i)->base[b * bf_index(i)->dim + col];
          d += delta * delta;
        }
        all.emplace_back(d, b);
      }
      std::sort(all.begin(), all.end());
      for (int64_t col = 0; col < k; ++col) {
        distances[row*k+col] = all[col].first; output[row*k+col] = all[col].second;
      }
    }
  });
  ++bf_searches; return CUVS_SUCCESS;
}
cuvsError_t cuvsBruteForceSerialize(cuvsResources_t, const char*, cuvsBruteForceIndex_t i) {
  if (fail()) return CUVS_ERROR; saved = *bf_index(i); ++bf_saves; return CUVS_SUCCESS;
}
cuvsError_t cuvsBruteForceDeserialize(cuvsResources_t r, const char*, cuvsBruteForceIndex_t i) {
  if (fail()) return CUVS_ERROR;
  resource(r)->pending.push_back([=] { *bf_index(i) = saved; }); ++bf_loads; return CUVS_SUCCESS;
}
}

static void check_query_resource_reuse(void* h, const float* query, int after_build)
{
  std::vector<unsigned> out10(10), out100(100);
  std::vector<float> dist10(10), dist100(100);
  assert(seekdb_cuvs_search(h, query, 1, 10, out10.data(), dist10.data()) == 0);
  assert(cagra_last_search_algo == MULTI_CTA);
  assert(rmm_allocs == after_build + 3);
  assert(seekdb_cuvs_search(h, query, 1, 10, out10.data(), dist10.data()) == 0);
  assert(rmm_allocs == after_build + 3);
  assert(seekdb_cuvs_search(h, query, 1, 100, out100.data(), dist100.data()) == 0);
  assert(rmm_allocs == after_build + 5);
  assert(seekdb_cuvs_search(h, query, 1, 10, out10.data(), dist10.data()) == 0);
  assert(rmm_allocs == after_build + 5);
}

int main()
{
  std::vector<float> base(256 * 4);
  for (size_t i = 0; i < base.size(); ++i) base[i] = static_cast<float>(i);
  unsigned ids[6] = {}; float distances[6] = {};
  assert(!seekdb_cuvs_build(nullptr, 256, 4));
  assert(!seekdb_cuvs_build(base.data(), 255, 4));
  assert(!seekdb_cuvs_build(base.data(), 256, 0));
  assert(!seekdb_cuvs_build(base.data(), LONG_MAX, 4));
  assert(calls == 0);
  assert(unsetenv("SEEKDB_CUVS_ALGO") == 0);
  fail_at = 0; calls = 0;
  void* h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && bf_builds == 0 && seekdb_cuvs_raw_snapshot(h) == 0);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  assert(setenv("SEEKDB_CUVS_ALGO", "cagra", 1) == 0);
  calls = 0; h = seekdb_cuvs_build(base.data(), 256, 4); assert(h);
  assert(seekdb_cuvs_raw_snapshot(nullptr) == 0 && seekdb_cuvs_raw_snapshot(h) == 0);
  int build_calls = calls;
  const int cagra_after_build = rmm_allocs;
  check_query_resource_reuse(h, base.data(), cagra_after_build);
  assert(cagra_search_param_creates == 1);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  assert(ids[0] == 0 && ids[3] == 1 && distances[0] == 0 && distances[3] == 0);
  int before = calls;
  assert(seekdb_cuvs_search(h, base.data(), 1, 257, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, base.data(), -1, 3, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, base.data(), LONG_MAX, 3, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, nullptr, 1, 3, ids, distances) != 0);
  assert(calls == before);
  assert(seekdb_cuvs_serialize(h, "mock.snapshot") == 0);
  assert(!cagra_include_dataset);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  h = seekdb_cuvs_deserialize_with_base("mock.snapshot", base.data(), 256, 4);
  assert(h && from_args_calls == 1);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, nullptr) == 0);
  assert(ids[3] == 1); seekdb_cuvs_free(h);
  assert(!seekdb_cuvs_deserialize("mock.snapshot", 256, 5));
  std::vector<float> wide_base(256 * 384);
  for (size_t i = 0; i < wide_base.size(); ++i) wide_base[i] = static_cast<float>(i);
  h = seekdb_cuvs_build(wide_base.data(), 256, 384); assert(h);
  assert(seekdb_cuvs_search(h, wide_base.data(), 2, 3, ids, distances) == 0);
  assert(ids[0] == 0 && ids[3] == 1);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  int cases = 14;
  for (int point = 1; point <= build_calls; ++point) {
    calls = 0; fail_at = point;
    h = seekdb_cuvs_build(base.data(), 256, 4); assert(!h);
    assert(live == 0 && buffers.empty()); ++cases;
  }
  calls = 0; fail_at = 0; h = seekdb_cuvs_build(base.data(), 256, 4); assert(h);
  calls = 0; assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  int search_calls = calls;
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  for (int point = 1; point <= search_calls; ++point) {
    fail_at = 0; calls = 0; h = seekdb_cuvs_build(base.data(), 256, 4); assert(h);
    calls = 0; fail_at = point;
    assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) != 0);
    seekdb_cuvs_free(h); assert(live == 0 && buffers.empty()); ++cases;
  }
  fail_at = 0;
  calls = 0; h = seekdb_cuvs_deserialize_with_base("mock.snapshot", base.data(), 256, 4); assert(h);
  int load_calls = calls; seekdb_cuvs_free(h);
  for (int point = 1; point <= load_calls; ++point) {
    calls = 0; fail_at = point;
    assert(!seekdb_cuvs_deserialize_with_base("mock.snapshot", base.data(), 256, 4));
    assert(live == 0 && buffers.empty()); ++cases;
  }
  assert(setenv("SEEKDB_CUVS_ALGO", "brute_force", 1) == 0);
  fail_at = 0; calls = 0; h = seekdb_cuvs_build(base.data(), 256, 4); assert(h && bf_builds == 1);
  assert(seekdb_cuvs_raw_snapshot(h) == 1);
  const int bf_after_build = rmm_allocs;
  assert(buffers.count(const_cast<void*>(bf_base_ptr)) == 1);
  check_query_resource_reuse(h, base.data(), bf_after_build);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0 && bf_searches == 5);
  assert(ids[0] == 0 && ids[3] == 1 && distances[0] == 0);
  assert(seekdb_cuvs_serialize(h, "mock.snapshot") == 0 && bf_saves == 1);
  seekdb_cuvs_free(h); assert(live == 0);
  assert(buffers.count(const_cast<void*>(bf_base_ptr)) == 0);
  h = seekdb_cuvs_deserialize("mock.snapshot", 256, 4); assert(h && bf_loads == 1);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  assert(unsetenv("SEEKDB_CUVS_ALGO") == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_NN_NITER", "25", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_BUILD_ALGO", "iterative", 1) == 0);
  fail_at = 0; h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && cagra_last_niter == 25 && cagra_last_build_algo == ITERATIVE_CAGRA_SEARCH);
  seekdb_cuvs_free(h);
  assert(setenv("SEEKDB_CUVS_CAGRA_BUILD_ALGO", "ivf_pq", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_IVF_NPROBES", "20", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_IVF_REFINEMENT", "2.5", 1) == 0);
  fail_at = 0; h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && cagra_last_build_algo == IVF_PQ && ivf_last_nprobes == 20 &&
         ivf_last_nlists == 1 && ivf_last_refinement == 2.5f && ivf_last_conservative &&
         ivf_last_lut == CUDA_R_16F && ivf_last_internal == CUDA_R_16F &&
         ivf_last_coarse == CUDA_R_16F && ivf_last_batch == 128 * 1024);
  seekdb_cuvs_free(h);
  assert(setenv("SEEKDB_CUVS_CAGRA_IVF_REFINEMENT", "0", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(unsetenv("SEEKDB_CUVS_CAGRA_IVF_NPROBES") == 0);
  assert(unsetenv("SEEKDB_CUVS_CAGRA_IVF_REFINEMENT") == 0);
  cases += 1;
  assert(setenv("SEEKDB_CUVS_CAGRA_NN_NITER", "0", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(setenv("SEEKDB_CUVS_CAGRA_BUILD_ALGO", "invalid", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(unsetenv("SEEKDB_CUVS_CAGRA_NN_NITER") == 0);
  assert(unsetenv("SEEKDB_CUVS_CAGRA_BUILD_ALGO") == 0);
  cases += 4;
  assert(setenv("SEEKDB_CUVS_CAGRA_BUILD_ALGO", "nn_descent", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_NN_NITER", "50", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_GRAPH_DEGREE", "64", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_INTERMEDIATE_DEGREE", "128", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_ITOPK", "256", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_SEARCH_WIDTH", "2", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_SEARCH_ALGO", "multi", 1) == 0);
  h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && cagra_last_graph_degree == 64 && cagra_last_intermediate_degree == 128);
  unsigned config_ids[10]; float config_distances[10];
  assert(seekdb_cuvs_search(h, base.data(), 1, 10, config_ids, config_distances) == 0);
  assert(cagra_last_itopk == 256 && cagra_last_search_width == 2 && cagra_last_search_algo == MULTI_CTA);
  seekdb_cuvs_free(h);
  assert(setenv("SEEKDB_CUVS_CAGRA_SEARCH_ALGO", "auto", 1) == 0);
  h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && seekdb_cuvs_search(h, base.data(), 1, 10, config_ids, config_distances) == 0);
  assert(cagra_last_search_algo == AUTO);
  seekdb_cuvs_free(h);
  assert(setenv("SEEKDB_CUVS_CAGRA_SEARCH_ALGO", "single", 1) == 0);
  assert(setenv("SEEKDB_CUVS_CAGRA_ITOPK", "1024", 1) == 0);
  h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && seekdb_cuvs_search(h, base.data(), 1, 10, config_ids, config_distances) != 0);
  seekdb_cuvs_free(h);
  assert(setenv("SEEKDB_CUVS_CAGRA_SEARCH_ALGO", "invalid", 1) == 0);
  h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && seekdb_cuvs_search(h, base.data(), 1, 10, config_ids, config_distances) != 0);
  seekdb_cuvs_free(h);
  assert(setenv("SEEKDB_CUVS_CAGRA_INTERMEDIATE_DEGREE", "32", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  for (const char* name : {"SEEKDB_CUVS_CAGRA_BUILD_ALGO", "SEEKDB_CUVS_CAGRA_NN_NITER",
       "SEEKDB_CUVS_CAGRA_GRAPH_DEGREE", "SEEKDB_CUVS_CAGRA_INTERMEDIATE_DEGREE",
       "SEEKDB_CUVS_CAGRA_ITOPK", "SEEKDB_CUVS_CAGRA_SEARCH_WIDTH",
       "SEEKDB_CUVS_CAGRA_SEARCH_ALGO"}) unsetenv(name);
  cases += 5;

  assert(setenv("SEEKDB_CUVS_ALGO", "invalid", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(setenv("SEEKDB_CUVS_ALGO", "ivf_flat", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(unsetenv("SEEKDB_CUVS_ALGO") == 0);
  cases += 3;
  std::printf("PASS bridge contract and fault-injection cases=%d (mock SDK; no GPU performance claim)\n", cases);
}
