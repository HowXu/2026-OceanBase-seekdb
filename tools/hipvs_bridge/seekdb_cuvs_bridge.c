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

typedef enum { SK_CAGRA, SK_BRUTE_FORCE } sk_algorithm;
typedef struct {
  cuvsResources_t res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t idx;
  cuvsBruteForceIndex_t bf;
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

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsStreamSync(h->res);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->bf) cuvsBruteForceIndexDestroy(h->bf);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  if (h->device_base) cuvsRMMFree(h->res, h->device_base, h->device_bytes);
  cuvsResourcesDestroy(h->res);
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
  float *qd = NULL, *dd = NULL;
  unsigned int* nd = NULL;
  int64_t* wide_ids = NULL;
  cuvsCagraSearchParams_t sp = NULL;
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
  if (h->algorithm != SK_CAGRA) {
    wide_ids = (int64_t*)malloc(id_bytes);
    if (!wide_ids) goto done;
  }
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&qd, query_bytes) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&nd, id_bytes) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&dd, distance_bytes) != CUVS_SUCCESS) goto done;
  if (h->algorithm == SK_CAGRA && cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) goto done;
  qt = matrix(qd, qs, kDLCUDA, h->device, kDLFloat);
  nt = matrix(nd, rs, kDLCUDA, h->device, h->algorithm == SK_CAGRA ? kDLUInt : kDLInt);
  if (h->algorithm != SK_CAGRA) nt.dl_tensor.dtype.bits = 64;
  dt = matrix(dd, rs, kDLCUDA, h->device, kDLFloat);
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
  }
  /* Copies and search must use the same resource stream. A default-stream
   * copy does not order work submitted to a nonblocking resource stream. */
  if (cudaMemcpyAsync(qd, query, query_bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess) goto done;
  if ((h->algorithm == SK_CAGRA &&
       cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, filter) != CUVS_SUCCESS) ||
      (h->algorithm == SK_BRUTE_FORCE &&
       cuvsBruteForceSearch(h->res, h->bf, &qt, &nt, &dt, filter) != CUVS_SUCCESS)) goto done;
  if (cudaMemcpyAsync(h->algorithm == SK_CAGRA ? (void*)out_ids : (void*)wide_ids,
                      nd, id_bytes, cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
      (out_dist && cudaMemcpyAsync(out_dist, dd, distance_bytes,
                                  cudaMemcpyDeviceToHost, stream) != cudaSuccess)) goto done;
  rc = cuvsStreamSync(h->res) == CUVS_SUCCESS ? 0 : 1;
  if (rc == 0 && wide_ids) {
    for (int64_t i = 0; i < nq * topk; ++i) out_ids[i] = (unsigned int)wide_ids[i];
  }
done:
  if (rc) log_error("search");
  /* Drain before releasing buffers, including partially enqueued failures. */
  if (rc && cuvsStreamSync(h->res) != CUVS_SUCCESS) rc = 1;
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, distance_bytes);
  if (nd) cuvsRMMFree(h->res, nd, id_bytes);
  if (qd) cuvsRMMFree(h->res, qd, query_bytes);
  free(wide_ids);
  return rc;
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !filename || !filename[0]) return 1;
  cuvsError_t status = h->algorithm == SK_CAGRA
      ? cuvsCagraSerialize(h->res, filename, h->idx, true)
      : cuvsBruteForceSerialize(h->res, filename, h->bf);
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
  } else if (cuvsBruteForceIndexCreate(&h->bf) != CUVS_SUCCESS ||
             cuvsBruteForceDeserialize(h->res, filename, h->bf) != CUVS_SUCCESS ||
             cuvsStreamSync(h->res) != CUVS_SUCCESS) goto load_fail;
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
