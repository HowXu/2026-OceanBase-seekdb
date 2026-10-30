/* seekdb <-> hipVS bridge. Keep all ROCm state behind the C ABI. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  cuvsResources_t res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t idx;
  long n, dim;
  int device;
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

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsStreamSync(h->res);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  size_t bytes = 0;
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
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS ||
      cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail;
  h->ip->metric = L2Expanded;
  h->ip->build_algo = NN_DESCENT;
  /* Spend less time constructing a dense graph; compensate with a wider
   * query frontier. Validated on 128/384-D data at >=95% Recall@10/100. */
  h->ip->graph_degree = 32;
  h->ip->intermediate_graph_degree = 64;
  h->ip->nn_descent_niter = 6;
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
      cuvsCagraBuild(h->res, h->ip, &dataset, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
  h->n = n; h->dim = dim;
  return h;
fail:
  log_error("build");
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
  cuvsCagraSearchParams_t sp = NULL;
  cudaStream_t stream;
  int rc = 1;
  int64_t qs[2] = {nq, h ? h->dim : 0}, rs[2] = {nq, topk};
  DLManagedTensor qt, nt, dt;
  cuvsFilter filter = {0};
  if (!h || !query || !out_ids || topk > h->n ||
      !matrix_bytes(nq, h->dim, sizeof(float), &query_bytes) ||
      !matrix_bytes(nq, topk, sizeof(unsigned int), &id_bytes) ||
      !matrix_bytes(nq, topk, sizeof(float), &distance_bytes)) return rc;
  if (cuvsStreamGet(h->res, &stream) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&qd, query_bytes) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&nd, id_bytes) != CUVS_SUCCESS ||
      cuvsRMMAlloc(h->res, (void**)&dd, distance_bytes) != CUVS_SUCCESS ||
      cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) goto done;
  qt = matrix(qd, qs, kDLCUDA, h->device, kDLFloat);
  nt = matrix(nd, rs, kDLCUDA, h->device, kDLUInt);
  dt = matrix(dd, rs, kDLCUDA, h->device, kDLFloat);
  sp->itopk_size = (size_t)topk * 4;
  /* The sparse, short-build graph trades a wider retained query frontier
   * for lower construction cost. High dimensions need twice the frontier. */
  if (sp->itopk_size < (h->dim > 256 ? 4096UL : 2048UL)) {
    sp->itopk_size = h->dim > 256 ? 4096 : 2048;
  }
  sp->itopk_size = (sp->itopk_size + 31) / 32 * 32;
  /* hipVS SINGLE_CTA rejects frontiers larger than 512. */
  if (sp->itopk_size > 512) sp->algo = MULTI_CTA;
  sp->search_width = 4;
  filter.type = NO_FILTER;
  /* Copies and search must use the same resource stream. A default-stream
   * copy does not order work submitted to a nonblocking resource stream. */
  if (cudaMemcpyAsync(qd, query, query_bytes, cudaMemcpyHostToDevice, stream) != cudaSuccess ||
      cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, filter) != CUVS_SUCCESS ||
      cudaMemcpyAsync(out_ids, nd, id_bytes, cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
      (out_dist && cudaMemcpyAsync(out_dist, dd, distance_bytes,
                                  cudaMemcpyDeviceToHost, stream) != cudaSuccess)) goto done;
  rc = cuvsStreamSync(h->res) == CUVS_SUCCESS ? 0 : 1;
done:
  if (rc) log_error("search");
  /* Drain before releasing buffers, including partially enqueued failures. */
  if (rc && cuvsStreamSync(h->res) != CUVS_SUCCESS) rc = 1;
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, distance_bytes);
  if (nd) cuvsRMMFree(h->res, nd, id_bytes);
  if (qd) cuvsRMMFree(h->res, qd, query_bytes);
  return rc;
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !filename || !filename[0]) return 1;
  if (cuvsCagraSerialize(h->res, filename, h->idx, true) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS) {
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
  if (cuvsDeviceIdGet(h->res, &h->device) != CUVS_SUCCESS ||
      cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS ||
      cuvsCagraDeserialize(h->res, filename, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS ||
      cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
      cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
      actual_n != n || actual_dim != dim) {
    log_error("deserialize or snapshot shape check");
    seekdb_cuvs_free(h);
    return NULL;
  }
  h->n = n; h->dim = dim;
  return h;
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
