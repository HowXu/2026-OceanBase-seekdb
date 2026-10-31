// Real adaptor hot-path test with a CPU-only stand-in for the GPU bridge.
#include "lib/vector/ob_vsag_adaptor.h"
#include "vsag/vsag.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <vector>

static std::atomic<int> thread_creations{0}, omp_threads{0};
extern "C" void __real_omp_set_num_threads(int);
extern "C" void __wrap_omp_set_num_threads(int threads) {
  omp_threads = threads;
  __real_omp_set_num_threads(threads);
}
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*function)(void *), void *arg) {
  ++thread_creations;
  return __real_pthread_create(thread, attr, function, arg);
}

struct FakeIndex { long n, dim; std::vector<float> vectors; };
static void require_large_stack() {
  pthread_attr_t attr;
  size_t bytes = 0;
  assert(pthread_getattr_np(pthread_self(), &attr) == 0);
  assert(pthread_attr_getstacksize(&attr, &bytes) == 0);
  pthread_attr_destroy(&attr);
  assert(bytes >= 32UL * 1024 * 1024);
}
extern "C" {
void *seekdb_cuvs_build(const float *base, long n, long dim) {
  require_large_stack();
  return new FakeIndex{n, dim, std::vector<float>(base, base + n * dim)};
}
int seekdb_cuvs_search(void *handle, const float *query, long nq, long k,
                       unsigned *ids, float *distances) {
  require_large_stack();
  auto *index = static_cast<FakeIndex *>(handle);
  for (long q = 0; q < nq; ++q) {
    std::vector<std::pair<float, unsigned>> ordered;
    for (long row = 0; row < index->n; ++row) {
      float distance = 0;
      for (long col = 0; col < index->dim; ++col) {
        float delta = query[q * index->dim + col] - index->vectors[row * index->dim + col];
        distance += delta * delta;
      }
      ordered.emplace_back(distance, static_cast<unsigned>(row));
    }
    std::sort(ordered.begin(), ordered.end());
    for (long i = 0; i < k; ++i) {
      ids[q * k + i] = ordered[i].second;
      if (distances) distances[q * k + i] = ordered[i].first;
    }
  }
  return 0;
}
void seekdb_cuvs_free(void *handle) { delete static_cast<FakeIndex *>(handle); }
int seekdb_cuvs_cagra_knn(const float *base, long n, long dim, const float *query,
                          long nq, long k, unsigned *ids) {
  void *index = seekdb_cuvs_build(base, n, dim);
  int rc = seekdb_cuvs_search(index, query, nq, k, ids, nullptr);
  seekdb_cuvs_free(index);
  return rc;
}
int seekdb_cuvs_serialize(void *, const char *) { return 1; }
int seekdb_cuvs_raw_snapshot(void *) { return 1; }
void *seekdb_cuvs_deserialize(const char *, long, long) { return nullptr; }
void *seekdb_cuvs_deserialize_with_base(const char *, const float *, long, long) { return nullptr; }
}

struct Allocator : vsag::Allocator {
  std::string Name() override { return "hotpath-test"; }
  void *Allocate(uint64_t bytes) override { return malloc(bytes); }
  void Deallocate(void *p) override { free(p); }
  void *Reallocate(void *p, uint64_t bytes) override { return realloc(p, bytes); }
};

int main(int argc, char **) {
  using namespace oceanbase::common;
  if (argc > 1) assert(setenv("SEEKDB_CUVS_QUERY_WORKER", "0", 1) == 0);
  else assert(unsetenv("SEEKDB_CUVS_QUERY_WORKER") == 0);
  Allocator allocator;
  constexpr long n = 256, dim = 4;
  std::vector<float> base(n * dim);
  std::vector<int64_t> ids(n);
  for (long row = 0; row < n; ++row) {
    ids[row] = 1000 + row * 7;
    for (long col = 0; col < dim; ++col) base[row * dim + col] = row + col * .01f;
  }
  obvsag::VectorIndexPtr index = nullptr;
  assert(obvsag::create_index(index, obvsag::HNSW_TYPE, "float32", "l2", dim, 16, 100, 100, &allocator) == 0);
  obvsag::mark_cuvs_index(index);
  assert(obvsag::add_index(index, base.data(), ids.data(), dim, n) == 0);
  const float *warm_distance = nullptr;
  const int64_t *warm_ids = nullptr;
  const char *warm_extra = nullptr;
  int64_t warm_count = 0;
  assert(obvsag::knn_search(index, base.data(), dim, 10, warm_distance, warm_ids,
                            warm_count, 100, false, warm_extra) == 0);
  allocator.Deallocate(const_cast<int64_t *>(warm_ids));
  allocator.Deallocate(const_cast<float *>(warm_distance));
  assert(omp_threads == 8);
  const int warmed_creations = thread_creations;
  for (int repeat = 0; repeat < 200; ++repeat) {
    const float *distance = nullptr;
    const int64_t *found_ids = nullptr;
    const char *extra = nullptr;
    int64_t count = 0;
    assert(obvsag::knn_search(index, base.data(), dim, 10, distance, found_ids, count, 100, false, extra) == 0);
    assert(count == 10 && found_ids[0] == ids[0] && distance[0] < .0001f);
    allocator.Deallocate(const_cast<int64_t *>(found_ids));
    allocator.Deallocate(const_cast<float *>(distance));
  }
  std::fprintf(stderr, "hotpath worker=%d warmed=%d final=%d\n", argc > 1 ? 0 : 1,
               warmed_creations, thread_creations.load());
  if (argc > 1) assert(thread_creations >= warmed_creations + 200);
  else assert(thread_creations == warmed_creations);
  assert(obvsag::delete_index(index) == 0);
  std::printf("PASS actual adaptor hotpath worker=%d thread_creations=%d (mock GPU)\n",
              argc > 1 ? 0 : 1, thread_creations.load());
}
