# Frozen V5 SIFT-like four-round baseline

- V5 revision: `634888d9b037153fa143d2c913ba4b25503e1d62`
- Dataset: `siftlike250k-d128`, real-image single-scale SIFT-like descriptors; not official SIFT250K.
- Base SHA256: `3a69ebe104a4f4772979930b4121625444a547eb232eb46f089d6be24834caac`
- Timing: DDL + first Top-10 query; QPS from 4096 independent SQL queries per Top-K.

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | GPU evidence |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 52.8734 | 136.4171 | 122.4868 | 0.992114 | 0.995684 | PASS |
| 1 | 50.4025 | 144.2839 | 129.4752 | 0.991895 | 0.995732 | PASS |
| 2 | 51.1767 | 134.9425 | 122.6299 | 0.992676 | 0.995774 | PASS |
| 3 | 51.2258 | 137.4940 | 124.8973 | 0.992407 | 0.995776 | PASS |

## Average

| Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---:|---:|---:|---:|---:|
| 51.4196 | 138.2844 | 124.8723 | 0.992273 | 0.995742 |

## External reference

| Version | Ready (s) | QPS@10 | QPS@100 |
|---|---:|---:|---:|
| Supplied reference | 23.0000 | 147.0000 | 143.0000 |
| Reference strengthened by 10% | 20.9091 | 161.7000 | 157.3000 |

This is the fixed SIFT-like baseline for subsequent compliant CAGRA optimization.
