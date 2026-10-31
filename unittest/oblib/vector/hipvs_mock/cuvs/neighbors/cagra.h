#pragma once
#include <cuvs/core/c_api.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int cuvsDistanceType;
enum { L2Expanded, NO_FILTER };
enum cuvsCagraGraphBuildAlgo { AUTO_SELECT, IVF_PQ, NN_DESCENT, ITERATIVE_CAGRA_SEARCH };
typedef struct {
  int metric; size_t intermediate_graph_degree, graph_degree;
  int build_algo; size_t nn_descent_niter;
} cuvsCagraIndexParams;
typedef cuvsCagraIndexParams* cuvsCagraIndexParams_t;
enum cuvsCagraSearchAlgo { SINGLE_CTA, MULTI_CTA, MULTI_KERNEL, AUTO };
typedef struct { size_t itopk_size, search_width; enum cuvsCagraSearchAlgo algo; } cuvsCagraSearchParams;
typedef cuvsCagraSearchParams* cuvsCagraSearchParams_t;
typedef struct { void* data; } cuvsCagraIndex;
typedef cuvsCagraIndex* cuvsCagraIndex_t;
typedef struct { uintptr_t addr; int type; } cuvsFilter;
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t*);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t*);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t*);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t);
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t, DLManagedTensor*, cuvsCagraIndex_t);
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t, cuvsCagraIndex_t,
                           DLManagedTensor*, DLManagedTensor*, DLManagedTensor*, cuvsFilter);
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char*, cuvsCagraIndex_t, bool);
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char*, cuvsCagraIndex_t);
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t, int64_t*);
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t, int64_t*);
cuvsError_t cuvsCagraIndexGetGraph(cuvsCagraIndex_t, DLManagedTensor*);
cuvsError_t cuvsCagraIndexFromArgs(cuvsResources_t, cuvsDistanceType, DLManagedTensor*,
                                   DLManagedTensor*, cuvsCagraIndex_t);
#ifdef __cplusplus
}
#endif
