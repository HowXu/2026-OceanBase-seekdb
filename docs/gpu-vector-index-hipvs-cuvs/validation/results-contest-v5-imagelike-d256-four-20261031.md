# Frozen V5 real-image d256 four-round baseline

- V5 revision: `634888d9b037153fa143d2c913ba4b25503e1d62`
- Dataset: `imagelike250k-d256`, two independently sampled 128-dimensional real-image SIFT-like blocks per vector; not official Hidden250K.
- Base SHA256: `af59aedd047eaba356e59d7fffe3c1b432fd353c80fa55712a1bb7d32385476f`
- Queries SHA256: `6c2a425a70ce2b470dc72c044b96636eee258ef51b2728a1f45de23c7af6b1e3`
- Truth SHA256: `1e17691eee8cf85992acaabaea27b559d369bab0ec7f3b09edce7a22cad40ccb`
- Timing: DDL + first Top-10 query; QPS from 4096 independent SQL queries per Top-K.

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | GPU evidence |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 58.4290 | 118.8250 | 108.0530 | 0.999951 | 0.999965 | PASS |
| 1 | 59.1900 | 116.7940 | 107.9780 | 0.999951 | 0.999951 | PASS |
| 2 | 59.3460 | 117.6100 | 107.9730 | 0.999976 | 0.999965 | PASS |
| 3 | 59.4170 | 117.4840 | 107.5990 | 0.999951 | 0.999944 | PASS |

## Average

| Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---:|---:|---:|---:|---:|
| 59.0959 | 117.6783 | 107.9007 | 0.999957 | 0.999951 |

Peak sampled resources across the four runs were 862392320 bytes of VRAM and 3422085120 bytes of RSS. Every round recorded 8193 GPU serves, one snapshot build and no CPU fallback.

## External reference

| Version | Ready (s) | QPS@10 | QPS@100 |
|---|---:|---:|---:|
| Supplied reference | 30.0000 | 87.0000 | 85.0000 |
| Reference strengthened by 10% | 27.2727 | 95.7000 | 93.5000 |

This is the fixed d256 real-image baseline for subsequent compliant CAGRA optimization. The high Recall is a property of this surrogate and must not be extrapolated to the hidden dataset. The companion d128 baseline is archived in `results-contest-v5-siftlike-four-20261031.md`.
