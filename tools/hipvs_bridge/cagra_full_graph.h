#ifndef SEEKDB_CAGRA_FULL_GRAPH_H
#define SEEKDB_CAGRA_FULL_GRAPH_H

#include <hip/hip_runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds exact top-256 base-base neighbors on the GPU. The output rows are
// unordered, contain no self ID, and own n*256 entries. This is construction
// only; it never accepts queries and never replaces CAGRA Search.
int seekdb_full_graph_candidates(const float* device_base, long n, long dim,
                                 unsigned int* device_ids, float* device_distances,
                                 hipStream_t stream);

#ifdef __cplusplus
}
#endif
#endif
