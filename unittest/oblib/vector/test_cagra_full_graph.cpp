#include "cagra_full_graph.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

static std::vector<float> load_npy(const char* path, long n, long dim) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { std::perror(path); std::exit(2); }
  char magic[6]; input.read(magic, 6);
  unsigned char version[2]; input.read(reinterpret_cast<char*>(version), 2);
  uint16_t header_size = 0;
  if (version[0] == 1) input.read(reinterpret_cast<char*>(&header_size), 2);
  else { uint32_t size; input.read(reinterpret_cast<char*>(&size), 4); header_size = static_cast<uint16_t>(size); }
  std::string header(header_size, ' '); input.read(header.data(), header_size);
  if (header.find("<f4") == std::string::npos) { std::fprintf(stderr, "not float32 NPY\n"); std::exit(2); }
  std::vector<float> base(static_cast<size_t>(n) * dim);
  input.read(reinterpret_cast<char*>(base.data()), base.size() * sizeof(float));
  if (!input) { std::fprintf(stderr, "truncated NPY\n"); std::exit(2); }
  return base;
}

static void check_small(bool nn_descent) {
  constexpr long n = 512, dim = 32;
  std::vector<float> base(n * dim);
  std::mt19937 rng(20261031);
  std::normal_distribution<float> normal;
  for (float& value : base) value = normal(rng);
  float* device_base = nullptr; float* device_distances = nullptr; unsigned* device_ids = nullptr;
  hipMalloc(&device_base, base.size() * sizeof(float));
  hipMalloc(&device_distances, n * 256 * sizeof(float));
  hipMalloc(&device_ids, n * 256 * sizeof(unsigned));
  hipMemcpy(device_base, base.data(), base.size() * sizeof(float), hipMemcpyHostToDevice);
  const int build_rc = nn_descent
      ? seekdb_nn_descent_candidates(device_base, n, dim, device_ids, device_distances, 12, 0)
      : seekdb_full_graph_candidates(device_base, n, dim, device_ids, device_distances, 0);
  if (build_rc != 0) {
    std::fprintf(stderr, "small candidate generation failed\n"); std::exit(1);
  }
  std::vector<unsigned> ids(n * 256); std::vector<float> distances(n * 256);
  hipMemcpy(ids.data(), device_ids, ids.size() * sizeof(unsigned), hipMemcpyDeviceToHost);
  hipMemcpy(distances.data(), device_distances, distances.size() * sizeof(float), hipMemcpyDeviceToHost);
  size_t matched = 0;
  for (long row = 0; row < n; ++row) {
    std::vector<std::pair<float, unsigned>> expected;
    for (long other = 0; other < n; ++other) {
      if (other == row) continue;
      float distance = 0;
      for (long col = 0; col < dim; ++col) {
        const float delta = base[row * dim + col] - base[other * dim + col];
        distance += delta * delta;
      }
      expected.emplace_back(distance, other);
    }
    std::sort(expected.begin(), expected.end());
    std::vector<unsigned> actual(ids.begin() + row * 256, ids.begin() + (row + 1) * 256);
    std::vector<unsigned> wanted;
    for (int i = 0; i < 256; ++i) wanted.push_back(expected[i].second);
    std::sort(actual.begin(), actual.end()); std::sort(wanted.begin(), wanted.end());
    if (std::adjacent_find(actual.begin(), actual.end()) != actual.end()) {
      std::fprintf(stderr, "duplicate ID at row %ld\n", row); std::exit(1);
    }
    matched += std::count_if(actual.begin(), actual.end(), [&](unsigned id) {
      return std::binary_search(wanted.begin(), wanted.end(), id);
    });
    if (!nn_descent && actual != wanted) {
      std::fprintf(stderr, "top-k mismatch at row %ld\n", row); std::exit(1);
    }
  }
  hipFree(device_base); hipFree(device_distances); hipFree(device_ids);
  const double recall = static_cast<double>(matched) / (n * 256);
  if (!nn_descent) std::printf("PASS full-graph small exact top-256 n=%ld dim=%ld\n", n, dim);
  else {
    std::printf("NN_DESCENT_SMALL recall=%.6f n=%ld dim=%ld\n", recall, n, dim);
    if (recall < 0.99) std::exit(1);
  }
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--check") { check_small(false); return 0; }
  if (argc == 2 && std::string(argv[1]) == "--nn-check") { check_small(true); return 0; }
  const bool nn_descent = argc > 1 && std::string(argv[1]) == "--nn";
  if ((nn_descent && argc != 5) || (!nn_descent && argc != 4)) {
    std::fprintf(stderr, "usage: %s --check | --nn-check | [--nn] base.npy n dim\n", argv[0]); return 2;
  }
  const char* path = nn_descent ? argv[2] : argv[1];
  const long n = std::strtol(nn_descent ? argv[3] : argv[2], nullptr, 10);
  const long dim = std::strtol(nn_descent ? argv[4] : argv[3], nullptr, 10);
  std::vector<float> base = load_npy(path, n, dim);
  float* device_base = nullptr; float* device_distances = nullptr; unsigned* device_ids = nullptr;
  const auto upload_start = std::chrono::steady_clock::now();
  if (hipMalloc(&device_base, base.size() * sizeof(float)) != hipSuccess ||
      hipMalloc(&device_distances, n * 256 * sizeof(float)) != hipSuccess ||
      hipMalloc(&device_ids, n * 256 * sizeof(unsigned)) != hipSuccess) return 1;
  hipMemcpy(device_base, base.data(), base.size() * sizeof(float), hipMemcpyHostToDevice);
  hipStreamSynchronize(0);
  const auto upload_end = std::chrono::steady_clock::now();
  hipEvent_t start, stop; hipEventCreate(&start); hipEventCreate(&stop);
  hipEventRecord(start, 0);
  const int rc = nn_descent
      ? seekdb_nn_descent_candidates(device_base, n, dim, device_ids, device_distances, 12, 0)
      : seekdb_full_graph_candidates(device_base, n, dim, device_ids, device_distances, 0);
  hipEventRecord(stop, 0); hipEventSynchronize(stop);
  float milliseconds = -1; hipEventElapsedTime(&milliseconds, start, stop);
  if (rc != 0) { std::fprintf(stderr, "full candidate generation failed\n"); return 1; }
  unsigned first[256]; hipMemcpy(first, device_ids, sizeof(first), hipMemcpyDeviceToHost);
  std::set<unsigned> unique(first, first + 256);
  if (unique.size() != 256 || unique.count(0) || *unique.rbegin() >= static_cast<unsigned>(n)) {
    std::fprintf(stderr, "invalid first-row IDs\n"); return 1;
  }
  size_t free_bytes = 0, total_bytes = 0; hipMemGetInfo(&free_bytes, &total_bytes);
  std::printf("%s n=%ld dim=%ld upload_s=%.6f candidates_s=%.6f free_vram=%zu\n",
              nn_descent ? "NN_DESCENT_CANDIDATES_PASS" : "FULL_GRAPH_CANDIDATES_PASS",
              n, dim, std::chrono::duration<double>(upload_end - upload_start).count(),
              milliseconds / 1000.0f, free_bytes);
  hipEventDestroy(start); hipEventDestroy(stop);
  hipFree(device_base); hipFree(device_distances); hipFree(device_ids);
}
