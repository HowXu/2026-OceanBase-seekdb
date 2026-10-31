#include "cagra_full_graph.h"
#include <hipblas/hipblas.h>
#include <algorithm>
#include <cstdio>

namespace {
constexpr int kNeighbors = 256;
constexpr int kRowBlock = 256;
constexpr int kColBlock = 8192;
constexpr int kWarps = 1;

__device__ inline unsigned ordered_float(float value) {
  const unsigned bits = __float_as_uint(value);
  return (bits & 0x80000000U) ? ~bits : (bits | 0x80000000U);
}

__global__ void norms_kernel(const float* base, long n, long dim, float* norms) {
  const long row = blockIdx.x * static_cast<long>(blockDim.x) + threadIdx.x;
  if (row >= n) return;
  float sum = 0;
  for (long col = 0; col < dim; ++col) {
    const float value = base[row * dim + col];
    sum += value * value;
  }
  norms[row] = sum;
}

__global__ void distances_kernel(const float* dots, const float* norms,
                                 long row0, long col0, int rows, int cols,
                                 long n, float* distances) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= rows * cols) return;
  const int row = index / cols;
  const int col = index - row * cols;
  const long global_row = row0 + row;
  const long global_col = col0 + col;
  float value = norms[global_row] + norms[global_col] - 2.0f * dots[index];
  value = fmaxf(value, 0.0f);
  distances[index] = global_row == global_col ? __int_as_float(0x7f800000) : value;
}

// One warp selects one row. Four 8-bit radix passes select exactly k values
// without sorting the 8192-wide input block.
__global__ void local_select_kernel(const float* distances, long col0,
                                    int rows, int cols,
                                    unsigned* local_ids, float* local_distances) {
  __shared__ unsigned histogram[kWarps][256];
  __shared__ unsigned thresholds[kWarps];
  __shared__ int ranks[kWarps];
  __shared__ unsigned counts[kWarps];
  const int warp = threadIdx.x / 32;
  const int lane = threadIdx.x % 32;
  const int row = blockIdx.x * kWarps + warp;
  if (row >= rows) return;
  if (lane == 0) { ranks[warp] = kNeighbors - 1; thresholds[warp] = 0; counts[warp] = 0; }
  __syncthreads();
  for (int pass = 0; pass < 4; ++pass) {
    for (int bin = lane; bin < 256; bin += 32) histogram[warp][bin] = 0;
    __syncthreads();
    const int shift = 24 - pass * 8;
    const unsigned prefix_mask = pass == 0 ? 0U : (0xffffffffU << (32 - pass * 8));
    const unsigned prefix = thresholds[warp] & prefix_mask;
    for (int col = lane; col < cols; col += 32) {
      const unsigned key = ordered_float(distances[row * cols + col]);
      if ((key & prefix_mask) == prefix) {
        atomicAdd(&histogram[warp][(key >> shift) & 0xffU], 1U);
      }
    }
    __syncthreads();
    if (lane == 0) {
      int bin = 0;
      int lower = 0;
      const int rank = ranks[warp];
      while (bin < 255 && lower + static_cast<int>(histogram[warp][bin]) <= rank) {
        lower += histogram[warp][bin++];
      }
      thresholds[warp] = prefix | (static_cast<unsigned>(bin) << shift);
      ranks[warp] = rank - lower;
    }
    __syncthreads();
  }
  const unsigned threshold = thresholds[warp];
  for (int col = lane; col < cols; col += 32) {
    const float value = distances[row * cols + col];
    const unsigned key = ordered_float(value);
    if (key <= threshold) {
      const unsigned position = atomicAdd(&counts[warp], 1U);
      if (position < kNeighbors) {
        local_ids[row * kNeighbors + position] = col0 + col;
        local_distances[row * kNeighbors + position] = value;
      }
    }
  }
}

__global__ void copy_local_kernel(const unsigned* local_ids, const float* local_distances,
                                  long row0, int rows, unsigned* ids, float* distances) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= rows * kNeighbors) return;
  const int row = index / kNeighbors;
  ids[(row0 + row) * kNeighbors + index % kNeighbors] = local_ids[index];
  distances[(row0 + row) * kNeighbors + index % kNeighbors] = local_distances[index];
}

__global__ void merge_select_kernel(unsigned* ids, float* distances,
                                    const unsigned* local_ids, const float* local_distances,
                                    long row0, int rows) {
  __shared__ unsigned candidate_ids[kWarps][kNeighbors * 2];
  __shared__ float candidate_distances[kWarps][kNeighbors * 2];
  __shared__ unsigned histogram[kWarps][256];
  __shared__ unsigned seen[kWarps][1024];
  __shared__ unsigned thresholds[kWarps];
  __shared__ int ranks[kWarps];
  __shared__ unsigned counts[kWarps];
  const int warp = threadIdx.x / 32;
  const int lane = threadIdx.x % 32;
  const int row = blockIdx.x * kWarps + warp;
  if (row >= rows) return;
  for (int index = lane; index < 1024; index += 32) seen[warp][index] = 0xffffffffU;
  for (int index = lane; index < kNeighbors * 2; index += 32) {
    if (index < kNeighbors) {
      candidate_ids[warp][index] = ids[(row0 + row) * kNeighbors + index];
      candidate_distances[warp][index] = distances[(row0 + row) * kNeighbors + index];
    } else {
      candidate_ids[warp][index] = local_ids[row * kNeighbors + index - kNeighbors];
      candidate_distances[warp][index] = local_distances[row * kNeighbors + index - kNeighbors];
    }
  }
  if (lane == 0) { ranks[warp] = kNeighbors - 1; thresholds[warp] = 0; counts[warp] = 0; }
  __syncthreads();
  // NN-Descent two-hop candidates contain duplicates. Keep one owner for
  // each ID and mark all later occurrences as unreachable before selection.
  for (int index = lane; index < kNeighbors * 2; index += 32) {
    const unsigned id = candidate_ids[warp][index];
    unsigned slot = (id * 2654435761U) & 1023U;
    unsigned owner = index;
    for (int probe = 0; probe < 1024; ++probe) {
      const unsigned previous = atomicCAS(&seen[warp][slot], 0xffffffffU, index);
      if (previous == 0xffffffffU) break;
      if (previous == index) break;
      if (candidate_ids[warp][previous] == id) { owner = previous; break; }
      slot = (slot + 1) & 1023U;
    }
    if (owner != index || id == row0 + row) candidate_distances[warp][index] = __int_as_float(0x7f800000);
  }
  __syncthreads();
  for (int pass = 0; pass < 4; ++pass) {
    for (int bin = lane; bin < 256; bin += 32) histogram[warp][bin] = 0;
    __syncthreads();
    const int shift = 24 - pass * 8;
    const unsigned prefix_mask = pass == 0 ? 0U : (0xffffffffU << (32 - pass * 8));
    const unsigned prefix = thresholds[warp] & prefix_mask;
    for (int index = lane; index < kNeighbors * 2; index += 32) {
      const unsigned key = ordered_float(candidate_distances[warp][index]);
      if ((key & prefix_mask) == prefix) {
        atomicAdd(&histogram[warp][(key >> shift) & 0xffU], 1U);
      }
    }
    __syncthreads();
    if (lane == 0) {
      int bin = 0;
      int lower = 0;
      const int rank = ranks[warp];
      while (bin < 255 && lower + static_cast<int>(histogram[warp][bin]) <= rank) {
        lower += histogram[warp][bin++];
      }
      thresholds[warp] = prefix | (static_cast<unsigned>(bin) << shift);
      ranks[warp] = rank - lower;
    }
    __syncthreads();
  }
  const unsigned threshold = thresholds[warp];
  for (int index = lane; index < kNeighbors * 2; index += 32) {
    const unsigned key = ordered_float(candidate_distances[warp][index]);
    if (key <= threshold) {
      const unsigned position = atomicAdd(&counts[warp], 1U);
      if (position < kNeighbors) {
        ids[(row0 + row) * kNeighbors + position] = candidate_ids[warp][index];
        distances[(row0 + row) * kNeighbors + position] = candidate_distances[warp][index];
      }
    }
  }
}

__device__ inline unsigned mix32(unsigned value) {
  value ^= value >> 16; value *= 0x7feb352dU;
  value ^= value >> 15; value *= 0x846ca68bU;
  return value ^ (value >> 16);
}

__device__ inline float vector_l2(const float* base, unsigned left, unsigned right, long dim) {
  float distance = 0;
  for (long col = 0; col < dim; ++col) {
    const float delta = base[left * dim + col] - base[right * dim + col];
    distance += delta * delta;
  }
  return distance;
}

__device__ inline unsigned device_gcd(unsigned left, unsigned right) {
  while (right != 0) { const unsigned next = left % right; left = right; right = next; }
  return left;
}

__global__ void init_random_kernel(const float* base, long n, long dim,
                                   unsigned* ids, float* distances) {
  const long row = blockIdx.x * static_cast<long>(blockDim.x) + threadIdx.x;
  if (row >= n) return;
  const long modulus = n - 1;
  unsigned stride = mix32(static_cast<unsigned>(row) + 17U) % modulus;
  while (device_gcd(stride, static_cast<unsigned>(modulus)) != 1) stride = (stride + 1) % modulus;
  const unsigned offset = mix32(static_cast<unsigned>(row) + 29U) % modulus;
  for (int slot = 0; slot < kNeighbors; ++slot) {
    unsigned id = (static_cast<uint64_t>(stride) * slot + offset) % modulus;
    if (id >= row) ++id;
    ids[row * kNeighbors + slot] = id;
    distances[row * kNeighbors + slot] = vector_l2(base, row, id, dim);
  }
}

__global__ void descent_candidates_kernel(const float* base, long n, long dim,
                                          const unsigned* ids, long row0, int rows, int iteration,
                                          unsigned* local_ids, float* local_distances) {
  const int row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= rows) return;
  const long global_row = row0 + row;
  for (int slot = 0; slot < kNeighbors; ++slot) {
    const unsigned neighbor = ids[global_row * kNeighbors + slot];
    unsigned id = global_row;
    float distance = __int_as_float(0x7f800000);
    if (neighbor < n) {
      const unsigned second_slot = mix32(static_cast<unsigned>(global_row) +
                                         slot * 977U + iteration * 7919U) & (kNeighbors - 1);
      const unsigned candidate = ids[neighbor * kNeighbors + second_slot];
      if (candidate < n && candidate != global_row) {
        id = candidate;
        distance = vector_l2(base, global_row, candidate, dim);
      }
    }
    local_ids[row * kNeighbors + slot] = id;
    local_distances[row * kNeighbors + slot] = distance;
  }
}

int check(hipError_t status, const char* operation) {
  if (status == hipSuccess) return 0;
  std::fprintf(stderr, "[full_graph] %s: %s\n", operation, hipGetErrorString(status));
  return -1;
}

int check(hipblasStatus_t status, const char* operation) {
  if (status == HIPBLAS_STATUS_SUCCESS) return 0;
  std::fprintf(stderr, "[full_graph] %s: hipBLAS status %d\n", operation, static_cast<int>(status));
  return -1;
}
}  // namespace

extern "C" int seekdb_full_graph_candidates(const float* device_base, long n, long dim,
                                             unsigned int* device_ids, float* device_distances,
                                             hipStream_t stream) {
  if (device_base == nullptr || device_ids == nullptr || device_distances == nullptr ||
      n <= kNeighbors || dim <= 0) return -1;
  float* norms = nullptr;
  float* dots = nullptr;
  float* local_distances = nullptr;
  unsigned* local_ids = nullptr;
  hipblasHandle_t blas = nullptr;
  int rc = 0;
  const size_t block_values = static_cast<size_t>(kRowBlock) * kColBlock;
  if (check(hipMalloc(&norms, n * sizeof(float)), "allocate norms") ||
      check(hipMalloc(&dots, block_values * sizeof(float)), "allocate distance block") ||
      check(hipMalloc(&local_distances, kRowBlock * kNeighbors * sizeof(float)),
            "allocate local distances") ||
      check(hipMalloc(&local_ids, kRowBlock * kNeighbors * sizeof(unsigned)), "allocate local IDs") ||
      check(hipblasCreate(&blas), "create hipBLAS") ||
      check(hipblasSetStream(blas, stream), "set hipBLAS stream")) {
    rc = -1;
    goto cleanup;
  }
  norms_kernel<<<(n + 255) / 256, 256, 0, stream>>>(device_base, n, dim, norms);
  if (check(hipGetLastError(), "launch norms")) { rc = -1; goto cleanup; }
  for (long row0 = 0; row0 < n && rc == 0; row0 += kRowBlock) {
    const int rows = static_cast<int>(std::min<long>(kRowBlock, n - row0));
    for (long col0 = 0; col0 < n; col0 += kColBlock) {
      const int cols = static_cast<int>(std::min<long>(kColBlock, n - col0));
      const float alpha = 1.0f, beta = 0.0f;
      // Row-major base is also a column-major dim-by-N matrix. X^T*Y
      // produces a cols-by-rows block whose rows are contiguous per base row.
      if (check(hipblasSgemm(blas, HIPBLAS_OP_T, HIPBLAS_OP_N, cols, rows, dim,
                             &alpha, device_base + col0 * dim, dim,
                             device_base + row0 * dim, dim, &beta, dots, cols), "SGEMM") ||
          check(hipGetLastError(), "SGEMM dispatch")) {
        rc = -1;
        break;
      }
      distances_kernel<<<(rows * cols + 255) / 256, 256, 0, stream>>>(
          dots, norms, row0, col0, rows, cols, n, dots);
      local_select_kernel<<<(rows + kWarps - 1) / kWarps, kWarps * 32, 0, stream>>>(
          dots, col0, rows, cols, local_ids, local_distances);
      if (col0 == 0) {
        copy_local_kernel<<<(rows * kNeighbors + 255) / 256, 256, 0, stream>>>(
            local_ids, local_distances, row0, rows, device_ids, device_distances);
      } else {
        merge_select_kernel<<<(rows + kWarps - 1) / kWarps, kWarps * 32, 0, stream>>>(
            device_ids, device_distances, local_ids, local_distances, row0, rows);
      }
      if (check(hipGetLastError(), "launch selection")) { rc = -1; break; }
    }
  }
  if (rc == 0 && check(hipStreamSynchronize(stream), "synchronize candidates")) rc = -1;

cleanup:
  if (blas != nullptr) hipblasDestroy(blas);
  hipFree(norms);
  hipFree(dots);
  hipFree(local_distances);
  hipFree(local_ids);
  return rc;
}

extern "C" int seekdb_nn_descent_candidates(const float* device_base, long n, long dim,
                                             unsigned int* device_ids, float* device_distances,
                                             int iterations, hipStream_t stream) {
  if (device_base == nullptr || device_ids == nullptr || device_distances == nullptr ||
      n <= kNeighbors || dim <= 0 || iterations <= 0) return -1;
  unsigned* local_ids = nullptr;
  float* local_distances = nullptr;
  int rc = 0;
  if (check(hipMalloc(&local_ids, kRowBlock * kNeighbors * sizeof(unsigned)),
            "allocate NN-Descent local IDs") ||
      check(hipMalloc(&local_distances, kRowBlock * kNeighbors * sizeof(float)),
            "allocate NN-Descent local distances")) {
    rc = -1;
    goto cleanup;
  }
  init_random_kernel<<<(n + 255) / 256, 256, 0, stream>>>(device_base, n, dim, device_ids, device_distances);
  if (check(hipGetLastError(), "launch random initialization")) { rc = -1; goto cleanup; }
  for (int iteration = 0; iteration < iterations && rc == 0; ++iteration) {
    for (long row0 = 0; row0 < n; row0 += kRowBlock) {
      const int rows = static_cast<int>(std::min<long>(kRowBlock, n - row0));
      descent_candidates_kernel<<<(rows + 255) / 256, 256, 0, stream>>>(
          device_base, n, dim, device_ids, row0, rows, iteration, local_ids, local_distances);
      merge_select_kernel<<<rows, kWarps * 32, 0, stream>>>(
          device_ids, device_distances, local_ids, local_distances, row0, rows);
      if (check(hipGetLastError(), "launch NN-Descent update")) { rc = -1; break; }
    }
  }
  if (rc == 0 && check(hipStreamSynchronize(stream), "synchronize NN-Descent")) rc = -1;
cleanup:
  hipFree(local_distances);
  hipFree(local_ids);
  return rc;
}
