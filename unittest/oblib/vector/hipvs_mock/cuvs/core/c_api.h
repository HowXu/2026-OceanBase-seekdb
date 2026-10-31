#pragma once
/* Minimal test double for the public hipVS 26.03 C API, not a GPU emulator. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CUVS_ERROR, CUVS_SUCCESS } cuvsError_t;
typedef uintptr_t cuvsResources_t;
typedef void* cudaStream_t;
typedef enum { cudaSuccess, cudaErrorUnknown } cudaError_t;
typedef enum { cudaMemcpyHostToDevice, cudaMemcpyDeviceToHost } cudaMemcpyKind;
typedef enum { kDLCPU = 1, kDLCUDA = 2 } DLDeviceType;
enum { kDLUInt = 1, kDLFloat = 2, kDLInt = 0 };
typedef struct { DLDeviceType device_type; int device_id; } DLDevice;
typedef struct { uint8_t code, bits; uint16_t lanes; } DLDataType;
typedef struct {
  void* data; DLDevice device; int ndim; DLDataType dtype;
  int64_t *shape, *strides; uint64_t byte_offset;
} DLTensor;
typedef struct DLManagedTensor {
  DLTensor dl_tensor; void* manager_ctx; void (*deleter)(struct DLManagedTensor*);
} DLManagedTensor;
const char* cuvsGetLastErrorText(void);
cuvsError_t cuvsResourcesCreate(cuvsResources_t*);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t);
cuvsError_t cuvsDeviceIdGet(cuvsResources_t, int*);
cuvsError_t cuvsStreamGet(cuvsResources_t, cudaStream_t*);
cuvsError_t cuvsStreamSync(cuvsResources_t);
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void**, size_t);
cuvsError_t cuvsRMMFree(cuvsResources_t, void*, size_t);
cudaError_t cudaMemcpyAsync(void*, const void*, size_t, cudaMemcpyKind, cudaStream_t);
#ifdef __cplusplus
}
#endif
