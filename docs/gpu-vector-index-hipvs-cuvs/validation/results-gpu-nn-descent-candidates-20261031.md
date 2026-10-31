# Sparse full-GPU NN-Descent candidate microbenchmark

Prototype: `587cc7077` (not connected to the SQL default path).

## Correctness and quality screen

- Dataset: 512×32 random FP32.
- Twelve sparse two-hop update rounds.
- Top-256 Recall against CPU exact neighbors: `0.996628`.
- No duplicate, self or out-of-range IDs.

The blocked all-pairs exact test still passed after sharing the merge kernel with the NN-Descent prototype.

## Full-scale throughput gate

| Dataset | Upload (s) | 12-round candidate generation and merge (s) |
|---|---:|---:|
| SIFT-like 250K×128 | 0.110075 | 61.448109 |

Command output: `NN_DESCENT_CANDIDATES_PASS n=250000 dim=128 upload_s=0.110075 candidates_s=61.448109`.

## Decision

Stop this sparse implementation before pruning and SQL integration. It is slower than both the blocked all-pairs prototype (18.36s) and the frozen end-to-end candidate (22.87s), while its full-scale graph quality is still unmeasured. Reducing iterations to hide the throughput failure would invalidate the quality premise of the replacement-builder route.
