#pragma once
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uintptr_t addr; DLDataType dtype; } cuvsBruteForceIndex;
typedef cuvsBruteForceIndex* cuvsBruteForceIndex_t;
cuvsError_t cuvsBruteForceIndexCreate(cuvsBruteForceIndex_t*);
cuvsError_t cuvsBruteForceIndexDestroy(cuvsBruteForceIndex_t);
cuvsError_t cuvsBruteForceBuild(cuvsResources_t, DLManagedTensor*, cuvsDistanceType, float,
                                cuvsBruteForceIndex_t);
cuvsError_t cuvsBruteForceSearch(cuvsResources_t, cuvsBruteForceIndex_t, DLManagedTensor*,
                                 DLManagedTensor*, DLManagedTensor*, cuvsFilter);
cuvsError_t cuvsBruteForceSerialize(cuvsResources_t, const char*, cuvsBruteForceIndex_t);
cuvsError_t cuvsBruteForceDeserialize(cuvsResources_t, const char*, cuvsBruteForceIndex_t);
#ifdef __cplusplus
}
#endif
