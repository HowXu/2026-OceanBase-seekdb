#pragma once
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  cuvsDistanceType metric; float metric_arg; bool add_data_on_build;
  uint32_t n_lists, kmeans_n_iters; double kmeans_trainset_fraction;
  bool adaptive_centers, conservative_memory_allocation;
} cuvsIvfFlatIndexParams;
typedef cuvsIvfFlatIndexParams* cuvsIvfFlatIndexParams_t;
typedef struct { uint32_t n_probes; } cuvsIvfFlatSearchParams;
typedef cuvsIvfFlatSearchParams* cuvsIvfFlatSearchParams_t;
typedef struct { uintptr_t addr; DLDataType dtype; } cuvsIvfFlatIndex;
typedef cuvsIvfFlatIndex* cuvsIvfFlatIndex_t;
cuvsError_t cuvsIvfFlatIndexParamsCreate(cuvsIvfFlatIndexParams_t*);
cuvsError_t cuvsIvfFlatIndexParamsDestroy(cuvsIvfFlatIndexParams_t);
cuvsError_t cuvsIvfFlatSearchParamsCreate(cuvsIvfFlatSearchParams_t*);
cuvsError_t cuvsIvfFlatSearchParamsDestroy(cuvsIvfFlatSearchParams_t);
cuvsError_t cuvsIvfFlatIndexCreate(cuvsIvfFlatIndex_t*);
cuvsError_t cuvsIvfFlatIndexDestroy(cuvsIvfFlatIndex_t);
cuvsError_t cuvsIvfFlatBuild(cuvsResources_t, cuvsIvfFlatIndexParams_t, DLManagedTensor*,
                             cuvsIvfFlatIndex_t);
cuvsError_t cuvsIvfFlatSearch(cuvsResources_t, cuvsIvfFlatSearchParams_t, cuvsIvfFlatIndex_t,
                              DLManagedTensor*, DLManagedTensor*, DLManagedTensor*, cuvsFilter);
cuvsError_t cuvsIvfFlatSerialize(cuvsResources_t, const char*, cuvsIvfFlatIndex_t);
cuvsError_t cuvsIvfFlatDeserialize(cuvsResources_t, const char*, cuvsIvfFlatIndex_t);
#ifdef __cplusplus
}
#endif
