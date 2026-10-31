/* seekdb <-> hipVS bridge. Keep all ROCm state behind the C ABI. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/brute_force.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef enum { SK_CAGRA, SK_BRUTE_FORCE } sk_algorithm;
typedef struct {
  cuvsResources_t res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t idx;
  cuvsCagraIndex_t graph_owner;
  cuvsBruteForceIndex_t bf;
  cuvsCagraSearchParams_t sp;
  cuvsIvfPqParams_t ivf_params;
  float* qd;
  unsigned int* nd;
  float* dd;
  int64_t* wide_ids;
  size_t query_capacity, neighbor_capacity, distance_capacity, wide_capacity;
  size_t cagra_itopk, cagra_search_width;
  int cagra_search_algo;
  sk_algorithm algorithm;
  long n, dim;
  int device;
  float* device_base;
  size_t device_bytes;
} sk_cuvs_index;

static int matrix_bytes(long rows, long cols, size_t width, size_t* bytes)
{
  if (rows <= 0 || cols <= 0 || (size_t)rows > SIZE_MAX / width / (size_t)cols) return 0;
  *bytes = (size_t)rows * (size_t)cols * width;
  return 1;
}

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

static void log_error(const char* operation)
{
  const char* error = cuvsGetLastErrorText();
  fprintf(stderr, "[seekdb_cuvs] %s failed: %s\n", operation, error ? error : "runtime error");
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

static int configure_cagra_build(sk_cuvs_index* h)
{
  const char* algorithm = getenv("SEEKDB_CUVS_CAGRA_BUILD_ALGO");
  uint32_t niter = 100;
  if (!algorithm || !algorithm[0] || strcmp(algorithm, "nn_descent") == 0) {
    h->ip->build_algo = NN_DESCENT;
  } else if (strcmp(algorithm, "iterative") == 0) {
    h->ip->build_algo = ITERATIVE_CAGRA_SEARCH;
  } else if (strcmp(algorithm, "ivf_pq") == 0) {
    h->ip->build_algo = IVF_PQ;
  } else {
    fprintf(stderr, "[seekdb_cuvs] unknown CAGRA build algorithm: %s\n", algorithm);
    return 0;
  }
  if (!parse_uint_override("SEEKDB_CUVS_CAGRA_NN_NITER", 100, &niter)) return 0;
  h->ip->nn_descent_niter = niter;
  return 1;
}

static int configure_cagra_degrees(sk_cuvs_index* h, long n)
{
  uint32_t graph = 128, intermediate = n > 256 ? 256 : (uint32_t)n - 1;
  if (!parse_uint_override("SEEKDB_CUVS_CAGRA_GRAPH_DEGREE", graph, &graph) ||
      !parse_uint_override("SEEKDB_CUVS_CAGRA_INTERMEDIATE_DEGREE", intermediate, &intermediate) ||
      graph >= intermediate || (uint64_t)intermediate >= (uint64_t)n) {
    fprintf(stderr, "[seekdb_cuvs] invalid CAGRA graph degrees\n");
    return 0;
  }
  h->ip->graph_degree = graph;
  h->ip->intermediate_graph_degree = intermediate;
  return 1;
}

static int configure_cagra_search(sk_cuvs_index* h)
{
  uint32_t itopk = h->dim > 256 ? 1024 : 512;
  uint32_t width = 4;
  const char* algorithm = getenv("SEEKDB_CUVS_CAGRA_SEARCH_ALGO");
  if (!parse_uint_override("SEEKDB_CUVS_CAGRA_ITOPK", itopk, &itopk) ||
      !parse_uint_override("SEEKDB_CUVS_CAGRA_SEARCH_WIDTH", width, &width)) return 0;
  h->cagra_search_algo = MULTI_CTA;
  if (algorithm && algorithm[0]) {
    if (strcmp(algorithm, "single") == 0) h->cagra_search_algo = SINGLE_CTA;
    else if (strcmp(algorithm, "multi") == 0) h->cagra_search_algo = MULTI_CTA;
    else if (strcmp(algorithm, "auto") == 0) h->cagra_search_algo = AUTO;
    else {
      fprintf(stderr, "[seekdb_cuvs] unknown CAGRA search algorithm: %s\n", algorithm);
      return 0;
    }
  }
  if (h->cagra_search_algo == SINGLE_CTA && itopk > 512) {
    fprintf(stderr, "[seekdb_cuvs] SINGLE_CTA requires itopk <= 512\n");
    return 0;
  }
  h->cagra_itopk = itopk;
  h->cagra_search_width = width;
  return 1;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsStreamSync(h->res);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->graph_owner) cuvsCagraIndexDestroy(h->graph_owner);
  if (h->bf) cuvsBruteForceIndexDestroy(h->bf);
  if (h->ivf_params) {
    if (h->ivf_params->ivf_pq_build_params) cuvsIvfPqIndexParamsDestroy(h->ivf_params->ivf_pq_build_params);
    if (h->ivf_params->ivf_pq_search_params) cuvsIvfPqSearchParamsDestroy(h->ivf_params->ivf_pq_search_params);
    free(h->ivf_params);
  }
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->qd) cuvsRMMFree(h->res, h->qd, h->query_capacity);
  if (h->nd) cuvsRMMFree(h->res, h->nd, h->neighbor_capacity);
  if (h->dd) cuvsRMMFree(h->res, h->dd, h->distance_capacity);
  if (h->device_base) cuvsRMMFree(h->res, h->device_base, h->device_bytes);
  free(h->wide_ids);
  cuvsResourcesDestroy(h->res);
  free(h);
}

static int parse_float_override(const char* name, float fallback, float* value)
{
  const char* text = getenv(name);
  char* end = NULL;
  float parsed;
  if (!text || !text[0]) { *value = fallback; return 1; }
  parsed = strtof(text, &end);
  if (!end || *end || !isfinite(parsed) || parsed <= 0) {
    fprintf(stderr, "[seekdb_cuvs] invalid %s: %s\n", name, text);
    return 0;
  }
  *value = parsed;
  return 1;
}

static int configure_ivf_pq(sk_cuvs_index* h, long n, long dim)
{
  cuvsIvfPqParams_t params = h->ip->graph_build_params;
  if (!params) {
    params = (cuvsIvfPqParams_t)calloc(1, sizeof(*params));
    if (!params) return 0;
    h->ivf_params = params;
    h->ip->graph_build_params = params;
    if (cuvsIvfPqIndexParamsCreate(&params->ivf_pq_build_params) != CUVS_SUCCESS ||
        cuvsIvfPqSearchParamsCreate(&params->ivf_pq_search_params) != CUVS_SUCCESS) return 0;
    cuvsIvfPqIndexParams_t build = params->ivf_pq_build_params;
    cuvsIvfPqSearchParams_t search = params->ivf_pq_search_params;
    build->metric = L2Expanded;
    build->n_lists = (uint32_t)(n > 2000 ? n / 2000 : 1);
    build->kmeans_n_iters = 10;
    const double min_fraction = fmin(1.0, 32.0 * build->n_lists / n);
    build->kmeans_trainset_fraction = fmax(min_fraction, fmin(1.0, 1.0 / sqrt(n * 1e-5)));
    build->pq_bits = dim <= 32 ? 8 : 4;
    build->pq_dim = dim <= 32 ? (dim < 16 ? dim : 16) :
      (dim <= 64 ? 32 : (dim <= 128 ? 64 : (dim <= 192 ? 96 : (uint32_t)((dim / 2 + 127) / 128 * 128))));
    build->codebook_kind = PER_SUBSPACE;
    search->n_probes = (uint32_t)lround(sqrt((double)build->n_lists) / 20 + 4);
    search->lut_dtype = HIP_R_16F;
    search->internal_distance_dtype = HIP_R_16F;
    search->coarse_search_dtype = HIP_R_16F;
    search->max_internal_batch_size = 128 * 1024;
    params->refinement_rate = 1;
  }
  if (!params->ivf_pq_build_params || !params->ivf_pq_search_params) return 0;
  const char* conservative = getenv("SEEKDB_CUVS_CAGRA_IVF_CONSERVATIVE");
  params->ivf_pq_build_params->conservative_memory_allocation =
      !(conservative && strcmp(conservative, "0") == 0);
  return parse_uint_override("SEEKDB_CUVS_CAGRA_IVF_NPROBES",
                             params->ivf_pq_search_params->n_probes,
                             &params->ivf_pq_search_params->n_probes) &&
         parse_float_override("SEEKDB_CUVS_CAGRA_IVF_REFINEMENT",
                              params->refinement_rate, &params->refinement_rate);
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
  h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    log_error("resources create"); free(h); return NULL;
  }
  if (!selected_algorithm(&h->algorithm)) { seekdb_cuvs_free(h); return NULL; }
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS) goto fail;
  if (h->algorithm == SK_CAGRA) {
    if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail;
    h->ip->metric = L2Expanded;
    h->ip->build_algo = NN_DESCENT;
    h->ip->graph_degree = 128;
    h->ip->intermediate_graph_degree = n > 256 ? 256 : (size_t)n - 1;
    h->ip->nn_descent_niter = 100;
    if (!configure_cagra_build(h) || !configure_cagra_degrees(h, n) ||
        (h->ip->build_algo == IVF_PQ && !configure_ivf_pq(h, n, dim)) ||
        cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
        cuvsCagraBuild(h->res, h->ip, &dataset, h->idx) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
  } else {
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
  if (h->algorithm != SK_CAGRA && id_bytes > h->wide_capacity) {
    int64_t* replacement = (int64_t*)realloc(h->wide_ids, id_bytes);
    if (!replacement) goto done;
    h->wide_ids = replacement;
    h->wide_capacity = id_bytes;
  }
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
      grow_device_buffer(h->res, (void**)&h->qd, &h->query_capacity, query_bytes) != 0 ||
      grow_device_buffer(h->res, (void**)&h->nd, &h->neighbor_capacity, id_bytes) != 0 ||
      grow_device_buffer(h->res, (void**)&h->dd, &h->distance_capacity, distance_bytes) != 0) goto done;
  if (h->algorithm == SK_CAGRA && !h->sp &&
      cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) goto done;
  if (h->algorithm == SK_CAGRA && !h->cagra_itopk && !configure_cagra_search(h)) goto done;
  qt = matrix(h->qd, qs, kDLCUDA, h->device, kDLFloat);
  nt = matrix(h->nd, rs, kDLCUDA, h->device, h->algorithm == SK_CAGRA ? kDLUInt : kDLInt);
  if (h->algorithm != SK_CAGRA) nt.dl_tensor.dtype.bits = 64;
  dt = matrix(h->dd, rs, kDLCUDA, h->device, kDLFloat);
  filter.type = NO_FILTER;
  if (h->algorithm == SK_CAGRA) {
    h->sp->itopk_size = (size_t)topk * 4;
    if (h->sp->itopk_size < h->cagra_itopk) h->sp->itopk_size = h->cagra_itopk;
    h->sp->itopk_size = (h->sp->itopk_size + 31) / 32 * 32;
    /* hipVS SINGLE_CTA rejects frontiers larger than 512. */
    if (h->cagra_search_algo >= 0) h->sp->algo = (enum cuvsCagraSearchAlgo)h->cagra_search_algo;
    else if (h->sp->itopk_size > 512) h->sp->algo = MULTI_CTA;
    h->sp->search_width = h->cagra_search_width;
  }
  /* Copies and search must use the same resource stream. A default-stream
   * copy does not order work submitted to a nonblocking resource stream. */
  if (cudaMemcpyAsync(h->qd, query, query_bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) goto done;
  if ((h->algorithm == SK_CAGRA &&
       cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, filter) != CUVS_SUCCESS) ||
      (h->algorithm == SK_BRUTE_FORCE &&
       cuvsBruteForceSearch(h->res, h->bf, &qt, &nt, &dt, filter) != CUVS_SUCCESS)) goto done;
  if (cudaMemcpyAsync(h->algorithm == SK_CAGRA ? (void*)out_ids : (void*)h->wide_ids,
                      h->nd, id_bytes, cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
      (out_dist && cudaMemcpyAsync(out_dist, h->dd, distance_bytes,
                                  cudaMemcpyDeviceToHost, stream) != cudaSuccess)) goto done;
  rc = cuvsStreamSync(h->res) == CUVS_SUCCESS ? 0 : 1;
  if (rc == 0 && h->wide_ids) {
    for (int64_t i = 0; i < nq * topk; ++i) out_ids[i] = (unsigned int)h->wide_ids[i];
  }
done:
  if (rc) log_error("search");
  /* Drain before a later search, handle destruction or fallback uses the stream. */
  if (rc && cuvsStreamSync(h->res) != CUVS_SUCCESS) rc = 1;
  return rc;
}

int seekdb_cuvs_raw_snapshot(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  return h && h->algorithm == SK_BRUTE_FORCE ? 1 : 0;
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !filename || !filename[0]) return 1;
  cuvsError_t status;
  if (h->algorithm == SK_CAGRA) status = cuvsCagraSerialize(h->res, filename, h->idx, false);
  else status = cuvsBruteForceSerialize(h->res, filename, h->bf);
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
  h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (!selected_algorithm(&h->algorithm)) { seekdb_cuvs_free(h); return NULL; }
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS) goto load_fail;
  if (h->algorithm == SK_CAGRA) {
    if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
        cuvsCagraDeserialize(h->res, filename, h->idx) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS ||
        cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) goto load_fail;
  } else {
    if (cuvsBruteForceIndexCreate(&h->bf) != CUVS_SUCCESS ||
        cuvsBruteForceDeserialize(h->res, filename, h->bf) != CUVS_SUCCESS ||
        cuvsStreamSync(h->res) != CUVS_SUCCESS) goto load_fail;
  }
  h->n = n; h->dim = dim;
  return h;
load_fail:
  log_error("deserialize or snapshot shape check");
  seekdb_cuvs_free(h);
  return NULL;
}

void* seekdb_cuvs_deserialize_with_base(const char* filename, const float* base, long n, long dim)
{
  size_t bytes = 0;
  int64_t shape[2] = {n, dim};
  cudaStream_t stream = NULL;
  sk_algorithm algorithm;
  sk_cuvs_index* h;
  DLManagedTensor graph = {0};
  DLManagedTensor dataset;
  if (!filename || !filename[0] || !base || n < 256 || (uint64_t)n > UINT32_MAX ||
      dim <= 0 || !matrix_bytes(n, dim, sizeof(float), &bytes)) return NULL;
  if (!selected_algorithm(&algorithm)) return NULL;
  if (algorithm != SK_CAGRA) return seekdb_cuvs_deserialize(filename, n, dim);
  h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->algorithm = algorithm;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS ||
      cuvsCagraIndexCreate(&h->graph_owner) != CUVS_SUCCESS ||
      cuvsCagraDeserialize(h->res, filename, h->graph_owner) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS ||
      cuvsCagraIndexGetGraph(h->graph_owner, &graph) != CUVS_SUCCESS ||
      graph.dl_tensor.ndim != 2 || !graph.dl_tensor.shape || graph.dl_tensor.shape[0] != n ||
      graph.dl_tensor.dtype.code != kDLUInt || graph.dl_tensor.dtype.bits != 32 ||
      cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&h->device_base, bytes) != CUVS_SUCCESS) goto load_fail;
  h->device_bytes = bytes;
  if (cudaMemcpyAsync(h->device_base, base, bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) goto load_fail;
  dataset = matrix(h->device_base, shape, kDLCUDA, h->device, kDLFloat);
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
      cuvsCagraIndexFromArgs(h->res, L2Expanded, &graph, &dataset, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS) goto load_fail;
  h->n = n;
  h->dim = dim;
  return h;
load_fail:
  log_error("deserialize with external dataset");
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
