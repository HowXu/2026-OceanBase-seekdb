// Exercise the real bridge against a queued, failure-injectable C API test double.
#include "seekdb_cuvs_bridge.h"
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/ivf_flat.h>
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <thread>
#include <vector>

struct Resource { std::vector<std::function<void()>> pending; };
struct Index { int64_t n = 0, dim = 0; std::vector<float> base; };
static Index saved;
static int calls, fail_at, live;
static int device_allocations, search_param_allocations;
static int bf_builds, bf_searches, bf_saves, bf_loads;
static const void* bf_base_ptr;
static int ivf_builds, ivf_searches, ivf_saves, ivf_loads;
static uint32_t ivf_lists, ivf_probes, ivf_iters;
static double ivf_train_fraction;
static std::set<void*> buffers;
static bool fail() { return ++calls == fail_at; }
static Resource* resource(cuvsResources_t r) { return reinterpret_cast<Resource*>(r); }
static Index* index(cuvsCagraIndex_t i) { return static_cast<Index*>(i->data); }
static Index* bf_index(cuvsBruteForceIndex_t i) { return reinterpret_cast<Index*>(i->addr); }
static Index* ivf_index(cuvsIvfFlatIndex_t i) { return reinterpret_cast<Index*>(i->addr); }
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
  *p = std::malloc(bytes); assert(*p); buffers.insert(*p); ++device_allocations; return CUVS_SUCCESS;
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
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsCagraSearchParams{}; ++live; ++search_param_allocations; return CUVS_SUCCESS;
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
  int64_t n = t->dl_tensor.shape[0], dim = t->dl_tensor.shape[1];
  // The supplied task branch fixes these build parameters.
  assert(p->graph_degree == 128 && p->nn_descent_niter == 100);
  assert(p->intermediate_graph_degree == (n > 256 ? 256u : static_cast<size_t>(n) - 1));
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
  assert(p->itopk_size % 32 == 0 && p->itopk_size >= static_cast<size_t>(k));
  assert(p->itopk_size >= (index(i)->dim > 256 ? 1024u : 512u));
  assert(p->itopk_size <= 512 || p->algo == MULTI_CTA);
  assert(p->itopk_size > 512 || p->algo == SINGLE_CTA);
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
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char*, cuvsCagraIndex_t i, bool) {
  if (fail()) return CUVS_ERROR; saved = *index(i); return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraDeserialize(cuvsResources_t r, const char*, cuvsCagraIndex_t i) {
  if (fail()) return CUVS_ERROR;
  resource(r)->pending.push_back([=] { *index(i) = saved; }); return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t i, int64_t* n) {
  if (fail()) return CUVS_ERROR; *n = index(i)->n; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t i, int64_t* dim) {
  if (fail()) return CUVS_ERROR; *dim = index(i)->dim; return CUVS_SUCCESS;
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
cuvsError_t cuvsIvfFlatIndexParamsCreate(cuvsIvfFlatIndexParams_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsIvfFlatIndexParams{}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatIndexParamsDestroy(cuvsIvfFlatIndexParams_t p) {
  delete p; --live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatSearchParamsCreate(cuvsIvfFlatSearchParams_t* p) {
  if (fail()) return CUVS_ERROR; *p = new cuvsIvfFlatSearchParams{}; ++live; ++search_param_allocations; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatSearchParamsDestroy(cuvsIvfFlatSearchParams_t p) {
  delete p; --live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatIndexCreate(cuvsIvfFlatIndex_t* out) {
  if (fail()) return CUVS_ERROR;
  *out = new cuvsIvfFlatIndex{reinterpret_cast<uintptr_t>(new Index), {}}; ++live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatIndexDestroy(cuvsIvfFlatIndex_t i) {
  delete ivf_index(i); delete i; --live; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatBuild(cuvsResources_t r, cuvsIvfFlatIndexParams_t p,
                             DLManagedTensor* t, cuvsIvfFlatIndex_t i) {
  if (fail()) return CUVS_ERROR;
  assert(p->metric == L2Expanded && p->add_data_on_build && p->conservative_memory_allocation);
  assert(t->dl_tensor.device.device_type == kDLCUDA && t->dl_tensor.device.device_id == 3);
  ivf_lists = p->n_lists; ivf_iters = p->kmeans_n_iters;
  ivf_train_fraction = p->kmeans_trainset_fraction;
  int64_t n = t->dl_tensor.shape[0], dim = t->dl_tensor.shape[1];
  auto data = static_cast<float*>(t->dl_tensor.data);
  resource(r)->pending.push_back([=] { ivf_index(i)->n = n; ivf_index(i)->dim = dim;
    ivf_index(i)->base.assign(data, data + n * dim); });
  ++ivf_builds; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatSearch(cuvsResources_t r, cuvsIvfFlatSearchParams_t p,
    cuvsIvfFlatIndex_t i, DLManagedTensor* q, DLManagedTensor* ids,
    DLManagedTensor* dist, cuvsFilter) {
  if (fail()) return CUVS_ERROR;
  ivf_probes = p->n_probes; assert(ivf_probes > 0 && ivf_probes <= ivf_lists);
  assert(ids->dl_tensor.dtype.code == kDLInt && ids->dl_tensor.dtype.bits == 64);
  auto query = static_cast<float*>(q->dl_tensor.data);
  auto output = static_cast<int64_t*>(ids->dl_tensor.data);
  auto distances = static_cast<float*>(dist->dl_tensor.data);
  auto nq = q->dl_tensor.shape[0], k = ids->dl_tensor.shape[1];
  resource(r)->pending.push_back([=] {
    for (int64_t row = 0; row < nq; ++row) {
      std::vector<std::pair<float, unsigned>> all;
      for (int64_t b = 0; b < ivf_index(i)->n; ++b) {
        float d = 0;
        for (int64_t col = 0; col < ivf_index(i)->dim; ++col) {
          float delta = query[row * ivf_index(i)->dim + col] - ivf_index(i)->base[b * ivf_index(i)->dim + col];
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
  ++ivf_searches; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatSerialize(cuvsResources_t, const char*, cuvsIvfFlatIndex_t i) {
  if (fail()) return CUVS_ERROR; saved = *ivf_index(i); ++ivf_saves; return CUVS_SUCCESS;
}
cuvsError_t cuvsIvfFlatDeserialize(cuvsResources_t r, const char*, cuvsIvfFlatIndex_t i) {
  if (fail()) return CUVS_ERROR;
  resource(r)->pending.push_back([=] { *ivf_index(i) = saved; }); ++ivf_loads; return CUVS_SUCCESS;
}
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
  void* h = seekdb_cuvs_build(base.data(), 256, 4); assert(h);
  int build_calls = calls;
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  assert(ids[0] == 0 && ids[3] == 1 && distances[0] == 0 && distances[3] == 0);
  int warmed_allocations = device_allocations, warmed_params = search_param_allocations;
  for (int repeat = 0; repeat < 50; ++repeat) {
    assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
    assert(ids[0] == 0 && ids[3] == 1);
  }
  assert(device_allocations == warmed_allocations && search_param_allocations == warmed_params);
  std::vector<unsigned> larger_ids(129);
  std::vector<float> larger_distances(129);
  assert(seekdb_cuvs_search(h, base.data(), 1, 129, larger_ids.data(), larger_distances.data()) == 0);
  assert(larger_ids.front() == 0 && larger_ids.back() == 128);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  assert(search_param_allocations == warmed_params);
  assert(device_allocations == warmed_allocations + 2);
  std::thread first([&] {
    for (int row = 0; row < 20; ++row) {
      unsigned result = 999; float distance = -1;
      assert(seekdb_cuvs_search(h, base.data() + row * 4, 1, 1, &result, &distance) == 0);
      assert(result == static_cast<unsigned>(row) && distance == 0);
    }
  });
  std::thread second([&] {
    for (int row = 20; row < 40; ++row) {
      unsigned result = 999; float distance = -1;
      assert(seekdb_cuvs_search(h, base.data() + row * 4, 1, 1, &result, &distance) == 0);
      assert(result == static_cast<unsigned>(row) && distance == 0);
    }
  });
  first.join(); second.join();
  std::vector<unsigned> grown_ids(9);
  std::vector<float> grown_distances(9);
  calls = 0; fail_at = 2; // Fail allocation while growing an existing workspace.
  assert(seekdb_cuvs_search(h, base.data(), 3, 3, grown_ids.data(), grown_distances.data()) != 0);
  assert(live == 3 && buffers.empty());
  fail_at = 0;
  assert(seekdb_cuvs_search(h, base.data(), 3, 3, grown_ids.data(), grown_distances.data()) == 0);
  assert(grown_ids[0] == 0 && grown_ids[3] == 1 && grown_ids[6] == 2);
  int before = calls;
  assert(seekdb_cuvs_search(h, base.data(), 1, 257, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, base.data(), -1, 3, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, base.data(), LONG_MAX, 3, ids, distances) != 0);
  assert(seekdb_cuvs_search(h, nullptr, 1, 3, ids, distances) != 0);
  assert(calls == before);
  assert(seekdb_cuvs_serialize(h, "mock.snapshot") == 0);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  h = seekdb_cuvs_deserialize("mock.snapshot", 256, 4); assert(h);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, nullptr) == 0);
  assert(ids[3] == 1); seekdb_cuvs_free(h);
  assert(!seekdb_cuvs_deserialize("mock.snapshot", 256, 5));
  std::vector<float> wide_base(256 * 384);
  for (size_t i = 0; i < wide_base.size(); ++i) wide_base[i] = static_cast<float>(i);
  h = seekdb_cuvs_build(wide_base.data(), 256, 384); assert(h);
  assert(seekdb_cuvs_search(h, wide_base.data(), 2, 3, ids, distances) == 0);
  assert(ids[0] == 0 && ids[3] == 1);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  int cases = 17;
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
    calls = 0; fail_at = 0;
    h = seekdb_cuvs_build(base.data(), 256, 4); assert(h);
    calls = 0; fail_at = point;
    assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) != 0);
    assert(live == 3 && buffers.empty());
    fail_at = 0;
    assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
    assert(ids[0] == 0 && ids[3] == 1);
    seekdb_cuvs_free(h); assert(live == 0 && buffers.empty()); ++cases;
  }
  fail_at = 0;
  calls = 0; h = seekdb_cuvs_deserialize("mock.snapshot", 256, 4); assert(h);
  int load_calls = calls; seekdb_cuvs_free(h);
  for (int point = 1; point <= load_calls; ++point) {
    calls = 0; fail_at = point;
    assert(!seekdb_cuvs_deserialize("mock.snapshot", 256, 4));
    assert(live == 0 && buffers.empty()); ++cases;
  }
  assert(setenv("SEEKDB_CUVS_ALGO", "brute_force", 1) == 0);
  fail_at = 0; calls = 0; h = seekdb_cuvs_build(base.data(), 256, 4); assert(h && bf_builds == 1);
  assert(buffers.count(const_cast<void*>(bf_base_ptr)) == 1);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0 && bf_searches == 1);
  assert(ids[0] == 0 && ids[3] == 1 && distances[0] == 0);
  warmed_allocations = device_allocations;
  for (int repeat = 0; repeat < 5; ++repeat) {
    assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
    assert(ids[0] == 0 && ids[3] == 1);
  }
  assert(device_allocations == warmed_allocations);
  assert(seekdb_cuvs_serialize(h, "mock.snapshot") == 0 && bf_saves == 1);
  seekdb_cuvs_free(h); assert(live == 0);
  assert(buffers.count(const_cast<void*>(bf_base_ptr)) == 0);
  h = seekdb_cuvs_deserialize("mock.snapshot", 256, 4); assert(h && bf_loads == 1);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  assert(setenv("SEEKDB_CUVS_ALGO", "invalid", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(unsetenv("SEEKDB_CUVS_ALGO") == 0);
  cases += 2;

  assert(setenv("SEEKDB_CUVS_ALGO", "ivf_flat", 1) == 0);
  fail_at = 0; calls = 0; h = seekdb_cuvs_build(base.data(), 256, 4);
  assert(h && ivf_builds == 1 && ivf_lists == 2 && ivf_iters == 10 && ivf_train_fraction == 0.25);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
  assert(ivf_searches == 1 && ivf_probes == 2 && ids[0] == 0 && ids[3] == 1);
  warmed_allocations = device_allocations; warmed_params = search_param_allocations;
  for (int repeat = 0; repeat < 5; ++repeat) {
    assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0);
    assert(ids[0] == 0 && ids[3] == 1 && ivf_probes == 2);
  }
  assert(device_allocations == warmed_allocations && search_param_allocations == warmed_params);
  assert(seekdb_cuvs_serialize(h, "mock.snapshot") == 0 && ivf_saves == 1);
  seekdb_cuvs_free(h); assert(live == 0);
  h = seekdb_cuvs_deserialize("mock.snapshot", 256, 4); assert(h && ivf_loads == 1);
  seekdb_cuvs_free(h); assert(live == 0 && buffers.empty());
  assert(setenv("SEEKDB_CUVS_IVF_N_LISTS", "64", 1) == 0);
  assert(setenv("SEEKDB_CUVS_IVF_N_PROBES", "16", 1) == 0);
  h = seekdb_cuvs_build(base.data(), 256, 4); assert(h && ivf_lists == 64);
  assert(seekdb_cuvs_search(h, base.data(), 2, 3, ids, distances) == 0 && ivf_probes == 16);
  seekdb_cuvs_free(h); assert(live == 0);
  assert(setenv("SEEKDB_CUVS_IVF_N_PROBES", "65", 1) == 0);
  assert(!seekdb_cuvs_build(base.data(), 256, 4));
  assert(unsetenv("SEEKDB_CUVS_IVF_N_LISTS") == 0);
  assert(unsetenv("SEEKDB_CUVS_IVF_N_PROBES") == 0);
  assert(unsetenv("SEEKDB_CUVS_ALGO") == 0);
  cases += 2;
  std::printf("PASS bridge contract and fault-injection cases=%d (mock SDK; no GPU performance claim)\n", cases);
}
