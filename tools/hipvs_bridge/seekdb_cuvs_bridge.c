/* seekdb <-> hipVS bridge. Keep all ROCm state behind the C ABI. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/ivf_flat.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef enum { SK_CAGRA, SK_BRUTE_FORCE, SK_IVF_FLAT } sk_algorithm;
typedef struct {
  cuvsResources_t res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t idx;
  cuvsBruteForceIndex_t bf;
  cuvsIvfFlatIndexParams_t ivf_ip;
  cuvsIvfFlatIndex_t ivf;
  sk_algorithm algorithm;
  long n, dim;
  int device;
  uint32_t ivf_n_probes;
  float* device_base;
  size_t device_bytes;
  pthread_mutex_t search_mutex;
  void *query_device, *neighbors_device, *distances_device;
  size_t query_capacity, neighbors_capacity, distances_capacity;
  int64_t *wide_ids;
  size_t wide_ids_capacity;
  cuvsCagraSearchParams_t search_params;
  cuvsIvfFlatSearchParams_t ivf_search_params;
  enum cuvsCagraSearchAlgo default_cagra_algo;
} sk_cuvs_index;

static int matrix_bytes(long rows, long cols, size_t width, size_t* bytes)
{
  if (rows <= 0 || cols <= 0 || (size_t)rows > SIZE_MAX / width / (size_t)cols) return 0;
  *bytes = (size_t)rows * (size_t)cols * width;
  return 1;
}

static void log_error(const char* operation)
{
  const char* error = cuvsGetLastErrorText();
  fprintf(stderr, "[seekdb_cuvs] %s failed: %s\n", operation, error ? error : "runtime error");
}

static sk_cuvs_index* create_handle(void)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->search_mutex, NULL) != 0) { free(h); return NULL; }
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    log_error("resources create");
    pthread_mutex_destroy(&h->search_mutex);
    free(h);
    return NULL;
  }
  return h;
}

static void clear_workspace(sk_cuvs_index* h)
{
  if (h->search_params) cuvsCagraSearchParamsDestroy(h->search_params);
  if (h->ivf_search_params) cuvsIvfFlatSearchParamsDestroy(h->ivf_search_params);
  if (h->distances_device) cuvsRMMFree(h->res, h->distances_device, h->distances_capacity);
  if (h->neighbors_device) cuvsRMMFree(h->res, h->neighbors_device, h->neighbors_capacity);
  if (h->query_device) cuvsRMMFree(h->res, h->query_device, h->query_capacity);
  free(h->wide_ids);
  h->search_params = NULL; h->ivf_search_params = NULL;
  h->query_device = h->neighbors_device = h->distances_device = NULL;
  h->query_capacity = h->neighbors_capacity = h->distances_capacity = 0;
  h->wide_ids = NULL; h->wide_ids_capacity = 0;
}

static int reserve_device(sk_cuvs_index* h, void** pointer, size_t* capacity, size_t bytes)
{
  void* replacement = NULL;
  if (*capacity >= bytes) return 1;
  if (cuvsRMMAlloc(h->res, &replacement, bytes) != CUVS_SUCCESS) return 0;
  if (*pointer) cuvsRMMFree(h->res, *pointer, *capacity);
  *pointer = replacement;
  *capacity = bytes;
  return 1;
}

/* hipVS's C API retains the cuVS kDLCUDA device convention. */
static DLManagedTensor matrix(void* data, int64_t* shape, DLDeviceType device, int device_id,
                              uint8_t dtype)
{
  DLManagedTensor tensor = {0};
  tensor.dl_tensor.data = data;
  tensor.dl_tensor.device.device_type = device;
  tensor.dl_tensor.device.device_id = device_id;
  tensor.dl_tensor.ndim = 2;
  tensor.dl_tensor.dtype.code = dtype;
  tensor.dl_tensor.dtype.bits = 32;
  tensor.dl_tensor.dtype.lanes = 1;
  tensor.dl_tensor.shape = shape;
  return tensor;
}

static int selected_algorithm(sk_algorithm* algorithm)
{
  const char* name = getenv("SEEKDB_CUVS_ALGO");
  if (!name || !name[0] || strcmp(name, "cagra") == 0) {
    *algorithm = SK_CAGRA;
    return 1;
  }
  if (strcmp(name, "brute_force") == 0) {
    *algorithm = SK_BRUTE_FORCE;
    return 1;
  }
  if (strcmp(name, "ivf_flat") == 0) {
    *algorithm = SK_IVF_FLAT;
    return 1;
  }
  fprintf(stderr, "[seekdb_cuvs] unknown algorithm: %s\n", name);
  return 0;
}

static int parse_uint_override(const char* name, uint32_t fallback, uint32_t* value)
{
  const char* text = getenv(name);
  char* end = NULL;
  unsigned long parsed;
  if (!text || !text[0]) { *value = fallback; return 1; }
  parsed = strtoul(text, &end, 10);
  if (!end || *end || parsed == 0 || parsed > UINT32_MAX) {
    fprintf(stderr, "[seekdb_cuvs] invalid %s: %s\n", name, text);
    return 0;
  }
  *value = (uint32_t)parsed;
  return 1;
}

static int configure_ivf(sk_cuvs_index* h, long n)
{
  uint32_t default_lists = (uint32_t)(n / 128);
  uint32_t lists = 0;
  if (default_lists == 0) default_lists = 1;
  if (default_lists > 512) default_lists = 512;
  if (!parse_uint_override("SEEKDB_CUVS_IVF_N_LISTS", default_lists, &lists) || lists > (uint64_t)n) {
    if (lists > (uint64_t)n) fprintf(stderr, "[seekdb_cuvs] IVF n_lists exceeds dataset size\n");
    return 0;
  }
  if (!parse_uint_override("SEEKDB_CUVS_IVF_N_PROBES", lists < 32 ? lists : 32,
                           &h->ivf_n_probes) || h->ivf_n_probes > lists) {
    if (h->ivf_n_probes > lists) fprintf(stderr, "[seekdb_cuvs] IVF n_probes exceeds n_lists\n");
    return 0;
  }
  h->ivf_ip->n_lists = lists;
  return 1;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsStreamSync(h->res);
  clear_workspace(h);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->bf) cuvsBruteForceIndexDestroy(h->bf);
  if (h->ivf) cuvsIvfFlatIndexDestroy(h->ivf);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  if (h->ivf_ip) cuvsIvfFlatIndexParamsDestroy(h->ivf_ip);
  if (h->device_base) cuvsRMMFree(h->res, h->device_base, h->device_bytes);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->search_mutex);
  free(h);
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  size_t bytes = 0;
  float* device_base = NULL;
  cudaStream_t stream = NULL;
  int64_t shape[2] = {n, dim};
  DLManagedTensor dataset = matrix((void*)base, shape, kDLCPU, 0, kDLFloat);
  sk_cuvs_index* h;
  if (!base || n < 256 || (uint64_t)n > UINT32_MAX || dim > INT_MAX ||
      !matrix_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  h = create_handle();
  if (!h) return NULL;
  if (!selected_algorithm(&h->algorithm)) { seekdb_cuvs_free(h); return NULL; }
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS) goto fail;
  if (h->algorithm == SK_CAGRA) {
    if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail;
    h->ip->metric = L2Expanded;
    h->ip->build_algo = NN_DESCENT;
    h->ip->graph_degree = 128;
    h->ip->intermediate_graph_degree = n > 256 ? 256 : (size_t)n - 1;
    h->ip->nn_descent_niter = 100;
    if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
        cuvsCagraBuild(h->res, h->ip, &dataset, h->idx) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
  } else if (h->algorithm == SK_BRUTE_FORCE) {
    if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
        cuvsRMMAlloc(h->res, (void**)&device_base, bytes) != CUVS_SUCCESS ||
        cudaMemcpyAsync(device_base, base, bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) goto fail;
    dataset = matrix(device_base, shape, kDLCUDA, h->device, kDLFloat);
    if (cuvsBruteForceIndexCreate(&h->bf) != CUVS_SUCCESS ||
        cuvsBruteForceBuild(h->res, &dataset, L2Expanded, 0.f, h->bf) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
    h->device_base = device_base;
    h->device_bytes = bytes;
    device_base = NULL;
  } else {
    if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
        cuvsRMMAlloc(h->res, (void**)&device_base, bytes) != CUVS_SUCCESS ||
        cudaMemcpyAsync(device_base, base, bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) goto fail;
    dataset = matrix(device_base, shape, kDLCUDA, h->device, kDLFloat);
    if (cuvsIvfFlatIndexParamsCreate(&h->ivf_ip) != CUVS_SUCCESS) goto fail;
    h->ivf_ip->metric = L2Expanded;
    h->ivf_ip->add_data_on_build = true;
    h->ivf_ip->kmeans_n_iters = 10;
    h->ivf_ip->kmeans_trainset_fraction = 0.25;
    h->ivf_ip->conservative_memory_allocation = true;
    if (!configure_ivf(h, n) ||
        cuvsIvfFlatIndexCreate(&h->ivf) != CUVS_SUCCESS ||
        cuvsIvfFlatBuild(h->res, h->ivf_ip, &dataset, h->ivf) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
  }
  if (device_base) cuvsRMMFree(h->res, device_base, bytes);
  h->n = n; h->dim = dim;
  return h;
fail:
  log_error("build");
  if (device_base) {
    cuvsStreamSync(h->res);
    cuvsRMMFree(h->res, device_base, bytes);
  }
  seekdb_cuvs_free(h);
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  size_t query_bytes = 0, id_bytes = 0, distance_bytes = 0;
  int64_t* wide_ids = NULL;
  cuvsCagraSearchParams_t sp = NULL;
  cuvsIvfFlatSearchParams_t ivf_sp = NULL;
  cudaStream_t stream;
  int rc = 1;
  int64_t qs[2] = {nq, h ? h->dim : 0}, rs[2] = {nq, topk};
  DLManagedTensor qt, nt, dt;
  cuvsFilter filter = {0};
  if (!h || !query || !out_ids || topk > h->n ||
      !matrix_bytes(nq, h->dim, sizeof(float), &query_bytes) ||
      !matrix_bytes(nq, topk, h->algorithm == SK_CAGRA ? sizeof(unsigned int) : sizeof(int64_t),
                    &id_bytes) ||
      !matrix_bytes(nq, topk, sizeof(float), &distance_bytes)) return rc;
  if (pthread_mutex_lock(&h->search_mutex) != 0) return rc;
  if (h->algorithm != SK_CAGRA) {
    if (h->wide_ids_capacity < id_bytes) {
      wide_ids = (int64_t*)realloc(h->wide_ids, id_bytes);
      if (!wide_ids) goto done;
      h->wide_ids = wide_ids;
      h->wide_ids_capacity = id_bytes;
    }
    wide_ids = h->wide_ids;
  }
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
      !reserve_device(h, &h->query_device, &h->query_capacity, query_bytes) ||
      !reserve_device(h, &h->neighbors_device, &h->neighbors_capacity, id_bytes) ||
      !reserve_device(h, &h->distances_device, &h->distances_capacity, distance_bytes)) goto done;
  if (h->algorithm == SK_CAGRA) {
    if (!h->search_params) {
      if (cuvsCagraSearchParamsCreate(&h->search_params) != CUVS_SUCCESS) goto done;
      h->default_cagra_algo = h->search_params->algo;
    }
    sp = h->search_params;
    // A previous large Top-K must not leave MULTI_CTA enabled for a later
    // small query that previously used the library's default algorithm.
    sp->algo = h->default_cagra_algo;
  }
  if (h->algorithm == SK_IVF_FLAT) {
    if (!h->ivf_search_params && cuvsIvfFlatSearchParamsCreate(&h->ivf_search_params) != CUVS_SUCCESS) goto done;
    ivf_sp = h->ivf_search_params;
  }
  qt = matrix(h->query_device, qs, kDLCUDA, h->device, kDLFloat);
  nt = matrix(h->neighbors_device, rs, kDLCUDA, h->device, h->algorithm == SK_CAGRA ? kDLUInt : kDLInt);
  if (h->algorithm != SK_CAGRA) nt.dl_tensor.dtype.bits = 64;
  dt = matrix(h->distances_device, rs, kDLCUDA, h->device, kDLFloat);
  filter.type = NO_FILTER;
  if (h->algorithm == SK_CAGRA) {
    sp->itopk_size = (size_t)topk * 4;
    /* High-dimensional data needs a wider retained frontier to maintain recall.
     * Keep the established 512-candidate path for dimensions up to 256. */
    if (sp->itopk_size < (h->dim > 256 ? 1024UL : 512UL)) {
      sp->itopk_size = h->dim > 256 ? 1024 : 512;
    }
    sp->itopk_size = (sp->itopk_size + 31) / 32 * 32;
    /* hipVS SINGLE_CTA rejects frontiers larger than 512. */
    if (sp->itopk_size > 512) sp->algo = MULTI_CTA;
    sp->search_width = 4;
  } else if (h->algorithm == SK_IVF_FLAT) {
    ivf_sp->n_probes = h->ivf_n_probes;
  }
  /* Copies and search must use the same resource stream. A default-stream
   * copy does not order work submitted to a nonblocking resource stream. */
  if (cudaMemcpyAsync(h->query_device, query, query_bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) goto done;
  if ((h->algorithm == SK_CAGRA &&
       cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, filter) != CUVS_SUCCESS) ||
      (h->algorithm == SK_BRUTE_FORCE &&
       cuvsBruteForceSearch(h->res, h->bf, &qt, &nt, &dt, filter) != CUVS_SUCCESS) ||
      (h->algorithm == SK_IVF_FLAT &&
       cuvsIvfFlatSearch(h->res, ivf_sp, h->ivf, &qt, &nt, &dt, filter) != CUVS_SUCCESS)) goto done;
  if (cudaMemcpyAsync(h->algorithm == SK_CAGRA ? (void*)out_ids : (void*)wide_ids,
                      h->neighbors_device, id_bytes, cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
      (out_dist && cudaMemcpyAsync(out_dist, h->distances_device, distance_bytes,
                                  cudaMemcpyDeviceToHost, stream) != cudaSuccess)) goto done;
  rc = cuvsStreamSync(h->res) == CUVS_SUCCESS ? 0 : 1;
  if (rc == 0 && wide_ids) {
    for (int64_t i = 0; i < nq * topk; ++i) out_ids[i] = (unsigned int)wide_ids[i];
  }
done:
  if (rc) log_error("search");
  /* Successful calls keep their workspace. Failed calls drain pending work
   * before releasing it, so retries cannot reuse partially written buffers. */
  if (rc) {
    cuvsStreamSync(h->res);
    clear_workspace(h);
  }
  pthread_mutex_unlock(&h->search_mutex);
  return rc;
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !filename || !filename[0]) return 1;
  cuvsError_t status;
  if (h->algorithm == SK_CAGRA) status = cuvsCagraSerialize(h->res, filename, h->idx, true);
  else if (h->algorithm == SK_BRUTE_FORCE) status = cuvsBruteForceSerialize(h->res, filename, h->bf);
  else status = cuvsIvfFlatSerialize(h->res, filename, h->ivf);
  if (status != CUVS_SUCCESS || cuvsStreamSync(h->res) != CUVS_SUCCESS) {
    log_error("serialize"); return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  size_t bytes = 0;
  int64_t actual_n = 0, actual_dim = 0;
  sk_cuvs_index* h;
  if (!filename || !filename[0] || n < 256 || (uint64_t)n > UINT32_MAX || dim > INT_MAX ||
      !matrix_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  h = create_handle();
  if (!h) return NULL;
  if (!selected_algorithm(&h->algorithm)) { seekdb_cuvs_free(h); return NULL; }
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS) goto load_fail;
  if (h->algorithm == SK_CAGRA) {
    if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
        cuvsCagraDeserialize(h->res, filename, h->idx) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS ||
        cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) goto load_fail;
  } else if (h->algorithm == SK_BRUTE_FORCE) {
    if (cuvsBruteForceIndexCreate(&h->bf) != CUVS_SUCCESS ||
        cuvsBruteForceDeserialize(h->res, filename, h->bf) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto load_fail;
  } else {
    if (cuvsIvfFlatIndexParamsCreate(&h->ivf_ip) != CUVS_SUCCESS || !configure_ivf(h, n) ||
        cuvsIvfFlatIndexCreate(&h->ivf) != CUVS_SUCCESS ||
        cuvsIvfFlatDeserialize(h->res, filename, h->ivf) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto load_fail;
  }
  h->n = n; h->dim = dim;
  return h;
load_fail:
  log_error("deserialize or snapshot shape check");
  seekdb_cuvs_free(h);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk, unsigned int* out_ids)
{
  size_t bytes = 0;
  void* h;
  int rc;
  if (!query || !out_ids || topk > n || !matrix_bytes(nq, topk, sizeof(unsigned int), &bytes)) return 1;
  h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 1;
  rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
