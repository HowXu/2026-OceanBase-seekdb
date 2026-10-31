// Real adaptor/VSAG component test with a CPU-only stand-in for the GPU bridge.
#include "lib/vector/ob_vsag_adaptor.h"
#include "vsag/vsag.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <sys/mman.h>
#include <thread>
#include <vector>

static std::atomic<int> thread_creations{0}, fail_creations{0};
static std::atomic<bool> mapping_failure{false}, block_build{false};
static std::atomic<bool> later_mapping_failure{false};
static std::atomic<int> active_mappings{0};
static std::atomic<int> mapping_attempts{0};
static std::mutex block_mutex;
static std::condition_variable block_condition;
static bool build_entered = false, build_released = false;
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*function)(void *), void *arg) {
  ++thread_creations;
  if (fail_creations.load() > 0) { --fail_creations; return EAGAIN; }
  return __real_pthread_create(thread, attr, function, arg);
}
extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" void *__wrap_mmap(void *address, size_t bytes, int prot, int flags, int fd, off_t offset) {
  ++mapping_attempts;
  assert(bytes <= 8UL * 1024 * 1024);
  if (mapping_failure || (later_mapping_failure && offset > 0)) { errno = ENOMEM; return MAP_FAILED; }
  void *result = __real_mmap(address, bytes, prot, flags, fd, offset);
  if (result != MAP_FAILED) ++active_mappings;
  return result;
}
extern "C" int __real_munmap(void *, size_t);
extern "C" int __wrap_munmap(void *address, size_t bytes) {
  int rc = __real_munmap(address, bytes);
  if (rc == 0) --active_mappings;
  return rc;
}

struct FakeIndex { long n, dim; std::vector<float> vectors; };
static constexpr size_t padding_bytes = 10UL * 1024 * 1024 + 33;
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
  if (block_build.exchange(false)) {
    std::unique_lock<std::mutex> guard(block_mutex);
    build_entered = true;
    block_condition.notify_all();
    assert(block_condition.wait_for(guard, std::chrono::seconds(30), [] { return build_released; }));
  }
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
int seekdb_cuvs_serialize(void *handle, const char *path) {
  require_large_stack();
  auto *index = static_cast<FakeIndex *>(handle);
  FILE *file = fopen(path, "wb");
  if (!file) return 1;
  bool ok = fwrite(&index->n, sizeof(long), 1, file) == 1 &&
            fwrite(&index->dim, sizeof(long), 1, file) == 1 &&
            fwrite(index->vectors.data(), sizeof(float), index->vectors.size(), file) == index->vectors.size();
  char chunk[16384]; memset(chunk, 0x6d, sizeof(chunk));
  for (size_t left = padding_bytes; ok && left > 0;) {
    size_t count = std::min(left, sizeof(chunk));
    ok = fwrite(chunk, 1, count, file) == count;
    left -= count;
  }
  return fclose(file) == 0 && ok ? 0 : 1;
}
void *seekdb_cuvs_deserialize(const char *path, long n, long dim) {
  require_large_stack();
  FILE *file = fopen(path, "rb");
  if (!file) return nullptr;
  auto *index = new FakeIndex{};
  bool ok = fread(&index->n, sizeof(long), 1, file) == 1 &&
            fread(&index->dim, sizeof(long), 1, file) == 1 && index->n == n && index->dim == dim;
  if (ok) {
    index->vectors.resize(n * dim);
    ok = fread(index->vectors.data(), sizeof(float), index->vectors.size(), file) == index->vectors.size();
  }
  char chunk[16384];
  for (size_t left = padding_bytes; ok && left > 0;) {
    size_t count = std::min(left, sizeof(chunk));
    ok = fread(chunk, 1, count, file) == count &&
         std::all_of(chunk, chunk + count, [](char value) { return value == 0x6d; });
    left -= count;
  }
  fclose(file);
  if (!ok) { delete index; return nullptr; }
  return index;
}
}

struct Allocator : vsag::Allocator {
  std::string Name() override { return "hotpath-component-test"; }
  void *Allocate(uint64_t bytes) override { return malloc(bytes); }
  void Deallocate(void *p) override { free(p); }
  void *Reallocate(void *p, uint64_t bytes) override { return realloc(p, bytes); }
};
struct CountingBuffer : std::stringbuf {
  size_t calls = 0, largest_write = 0;
  std::streamsize xsputn(const char *data, std::streamsize bytes) override {
    ++calls; largest_write = std::max(largest_write, static_cast<size_t>(bytes));
    return std::stringbuf::xsputn(data, bytes);
  }
};
struct FailingBuffer : std::streambuf {
  std::streamsize xsputn(const char *, std::streamsize bytes) override {
    return bytes > 1024 * 1024 ? bytes - 1 : bytes;
  }
};

int main(int argc, char **) {
  using namespace oceanbase::common;
  Allocator allocator;
  constexpr long n = 256, dim = 4;
  std::vector<float> base(n * dim);
  std::vector<int64_t> ids(n);
  for (long row = 0; row < n; ++row) {
    ids[row] = 1000 + row * 7;
    for (long col = 0; col < dim; ++col) base[row * dim + col] = row + col * .01f;
  }
  obvsag::VectorIndexPtr index = nullptr;
  auto create = [&](obvsag::VectorIndexPtr &handle) {
    assert(obvsag::create_index(handle, obvsag::HNSW_TYPE, "float32", "l2", dim, 16, 100, 100, &allocator) == 0);
  };
  create(index);
  obvsag::mark_cuvs_index(index);
  assert(obvsag::add_index(index, base.data(), ids.data(), dim, n) == 0);
  if (argc > 1) fail_creations = 1; // Cached-worker startup failure must use a dedicated thread.
  CountingBuffer mapped_buffer;
  std::ostream mapped(&mapped_buffer);
  assert(obvsag::fserialize(index, mapped) == 0);
  assert(active_mappings == 0 && mapping_attempts == 2);
  assert(mapped_buffer.calls < 16 && mapped_buffer.largest_write > 1024UL * 1024);
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
  assert(thread_creations == warmed_creations);
  mapping_failure = true;
  CountingBuffer copied_buffer;
  std::ostream copied(&copied_buffer);
  assert(obvsag::fserialize(index, copied) == 0);
  assert(active_mappings == 0 && mapping_attempts == 3);
  assert(copied_buffer.largest_write <= 1024UL * 1024);
  assert(copied_buffer.calls < 24 && copied_buffer.str() == mapped_buffer.str());
  mapping_failure = false;
  later_mapping_failure = true;
  CountingBuffer partial_buffer;
  std::ostream partial(&partial_buffer);
  assert(obvsag::fserialize(index, partial) == 0);
  assert(active_mappings == 0 && mapping_attempts == 5);
  assert(partial_buffer.str() == mapped_buffer.str());
  later_mapping_failure = false;
  // Independently construct the pre-optimization OBCUVS02 stream layout.
  std::stringstream legacy;
  uint64_t graph_bytes = 2 * sizeof(long) + base.size() * sizeof(float) + padding_bytes;
  int32_t dim32 = dim, reserved = 0;
  int64_t n64 = n;
  legacy.write("OBCUVS02", 8);
  legacy.write(reinterpret_cast<const char *>(&graph_bytes), sizeof(graph_bytes));
  legacy.write(reinterpret_cast<const char *>(&dim32), sizeof(dim32));
  legacy.write(reinterpret_cast<const char *>(&reserved), sizeof(reserved));
  legacy.write(reinterpret_cast<const char *>(&n64), sizeof(n64));
  legacy.write(reinterpret_cast<const char *>(ids.data()), ids.size() * sizeof(int64_t));
  legacy.write(reinterpret_cast<const char *>(base.data()), base.size() * sizeof(float));
  legacy.write(reinterpret_cast<const char *>(&n), sizeof(n));
  legacy.write(reinterpret_cast<const char *>(&dim), sizeof(dim));
  legacy.write(reinterpret_cast<const char *>(base.data()), base.size() * sizeof(float));
  legacy << std::string(padding_bytes, char(0x6d));
  assert(legacy.str() == mapped_buffer.str());
  mapping_failure = false;
  FailingBuffer failing_buffer;
  std::ostream failed(&failing_buffer);
  assert(obvsag::fserialize(index, failed) != 0);
  assert(active_mappings == 0);
  obvsag::VectorIndexPtr restored = nullptr;
  create(restored);
  std::stringstream input(mapped_buffer.str());
  assert(obvsag::fdeserialize(restored, input) == 0);
  int64_t count = 0;
  assert(obvsag::get_index_number(restored, count) == 0 && count == n);
  const float *restored_distances = nullptr;
  const int64_t *restored_ids = nullptr;
  const char *restored_extra = nullptr;
  assert(obvsag::knn_search(restored, base.data(), dim, 10, restored_distances,
                            restored_ids, count, 100, false, restored_extra) == 0);
  assert(count == 10 && restored_ids[0] == ids[0] && restored_distances[0] < .0001f);
  allocator.Deallocate(const_cast<int64_t *>(restored_ids));
  allocator.Deallocate(const_cast<float *>(restored_distances));
  std::string short_snapshot = mapped_buffer.str(); short_snapshot.pop_back();
  obvsag::VectorIndexPtr invalid = nullptr;
  create(invalid);
  std::stringstream truncated(short_snapshot);
  assert(obvsag::fdeserialize(invalid, truncated) != 0);
  assert(obvsag::delete_index(invalid) == 0);

  // A long raw build must not queue unrelated work behind the cached worker.
  block_build = true;
  std::thread first([&] {
    unsigned offset = 999; float distance = -1;
    assert(obvsag::cuvs_batch_knn(base.data(), n, dim, base.data(), 1, 1, &offset, &distance) == 1);
    assert(offset == 0 && distance == 0);
  });
  {
    std::unique_lock<std::mutex> guard(block_mutex);
    assert(block_condition.wait_for(guard, std::chrono::seconds(30), [] { return build_entered; }));
  }
  unsigned offset = 999; float distance = -1;
  assert(obvsag::cuvs_batch_knn(base.data(), n, dim, base.data(), 1, 1, &offset, &distance) == 1);
  assert(offset == 0 && distance == 0);
  {
    std::lock_guard<std::mutex> guard(block_mutex);
    build_released = true; block_condition.notify_all();
  }
  first.join();
  assert(obvsag::delete_index(restored) == 0 && obvsag::delete_index(index) == 0);
  printf("PASS actual adaptor hotpath: 200 queries create no extra workers; snapshot writes=%zu/%zu; mmap fallback bytes identical; errors/reload/concurrency/startup-failure checked (mock GPU)\n",
         mapped_buffer.calls, copied_buffer.calls);
}
