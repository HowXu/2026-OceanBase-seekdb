# Blocked full-neighbor candidate microbenchmark

Prototype: `9ab0acf30` (not connected to the SQL default path).

## Correctness gate

`512×32` random FP32 data passed row-by-row comparison with CPU exact top-256. Self IDs were excluded and all returned IDs were unique and valid.

## Full-scale distance and selection gate

| Dataset | Upload (s) | GEMM + local top-256 + global merge + sync (s) |
|---|---:|---:|
| SIFT-like 250K×128 | 0.110414 | 18.356173 |

Command output: `FULL_GRAPH_CANDIDATES_PASS n=250000 dim=128 upload_s=0.110414 candidates_s=18.356173`.

## Decision

Stop the all-pairs route. The 18.36-second result excludes diversified pruning, reverse edges, outer scan/transfer, snapshot serialization and the first query, so it cannot beat the frozen 22.87-second end-to-end candidate with useful margin. Per the approved stop condition, d256 and SQL integration were not run. The next prototype must use sparse full-GPU NN-Descent candidate updates instead of all-pairs GEMM.
