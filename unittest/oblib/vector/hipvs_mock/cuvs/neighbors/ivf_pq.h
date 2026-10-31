#pragma once
#include <cuvs/core/c_api.h>
#ifdef __cplusplus
extern "C" {
#endif
enum codebook_gen { PER_SUBSPACE = 0, PER_CLUSTER = 1 };
typedef struct {
  int metric;
  uint32_t n_lists, kmeans_n_iters;
  double kmeans_trainset_fraction;
  uint32_t pq_bits, pq_dim;
  int codebook_kind;
  bool conservative_memory_allocation;
} cuvsIvfPqIndexParams;
typedef cuvsIvfPqIndexParams* cuvsIvfPqIndexParams_t;
typedef int cudaDataType_t;
enum { CUDA_R_16F = 2 };
typedef struct {
  uint32_t n_probes;
  cudaDataType_t lut_dtype, internal_distance_dtype, coarse_search_dtype;
  uint32_t max_internal_batch_size;
} cuvsIvfPqSearchParams;
typedef cuvsIvfPqSearchParams* cuvsIvfPqSearchParams_t;
cuvsError_t cuvsIvfPqIndexParamsCreate(cuvsIvfPqIndexParams_t*);
cuvsError_t cuvsIvfPqIndexParamsDestroy(cuvsIvfPqIndexParams_t);
cuvsError_t cuvsIvfPqSearchParamsCreate(cuvsIvfPqSearchParams_t*);
cuvsError_t cuvsIvfPqSearchParamsDestroy(cuvsIvfPqSearchParams_t);
#ifdef __cplusplus
}
#endif
