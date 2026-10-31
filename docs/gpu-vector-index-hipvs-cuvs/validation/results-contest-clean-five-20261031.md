# Clean rebuild five-round V5/candidate comparison

- Both binaries were built from clean `build_eval` directories with identical CMake options and `-j48`.
- Each dataset has five valid baseline and five valid candidate rounds. Candidate gates all pass; every round has 8193 GPU serves and zero CPU fallback.

| Dataset | V5 ready (s) | Candidate ready (s) | Build speedup | QPS ratio@10 | QPS ratio@100 | Gates |
|---|---:|---:|---:|---:|---:|---|
| siftlike250k-d128 | 52.7049 | 6.4492 | 8.1723x | 6.8302x | 4.0302x | 5/5 PASS |
| imagelike250k-d256 | 59.3448 | 8.3318 | 7.1227x | 6.0848x | 4.2381x | 5/5 PASS |

## Averages

| Dataset | V5 QPS@10/100 | Candidate QPS@10/100 | V5 Recall@10/100 | Candidate Recall@10/100 |
|---|---|---|---|
| siftlike250k-d128 | 139.3755/124.3471 | 951.9688/501.1492 | 0.992407/0.995695 | 0.992510/0.995697 |
| imagelike250k-d256 | 121.9681/111.6059 | 742.1571/473.0001 | 0.999941/0.999945 | 0.999937/0.999969 |

The initial V5 d128 run had one incomplete-evidence round and was replaced with a new five-round baseline and a new five-round candidate run. No failed round was included in the averages. Datasets are real-image-derived surrogates, not official SIFT250K/Hidden250K.
