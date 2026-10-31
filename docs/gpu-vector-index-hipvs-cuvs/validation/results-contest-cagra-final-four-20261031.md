# Zero-configuration CAGRA final four-round validation

- Candidate revision: `747dcdee06fe7c72f61442485a6fa258f1e34722`.
- Baseline revision: `634888d9b037153fa143d2c913ba4b25503e1d62`.
- No `SEEKDB_CUVS_ALGO`, OpenMP, CAGRA parameter or worker environment overrides were set.
- Each round used DDL + first Top-10 query timing and 4096 independent SQL queries per Top-K.
- Every one of the eight rounds passed all Recall, Recall-delta, QPS-regression, VRAM, RSS and GPU-evidence gates.

## siftlike250k-d128

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | Gates |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 22.9614 | 877.0392 | 455.9695 | 0.992529 | 0.995703 | PASS |
| 1 | 23.0626 | 886.5180 | 474.7623 | 0.992773 | 0.995674 | PASS |
| 2 | 22.5568 | 914.6083 | 487.8408 | 0.992236 | 0.995762 | PASS |
| 3 | 22.9148 | 882.7045 | 482.5650 | 0.992310 | 0.995686 | PASS |

### Average and comparison

| Version | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| V5 | 51.4196 | 138.2844 | 124.8723 | 0.992273 | 0.995742 |
| Candidate | 22.8739 | 890.2175 | 475.2844 | 0.992462 | 0.995706 |
| Candidate/V5 | 2.2480x | 6.4376x | 3.8062x | - | - |

Against the external reference strengthened by 10% (`20.9091s`, `161.7000/157.3000` QPS), the ratios are `0.9141x` for build, `5.5054x` for Top-10 and `3.0215x` for Top-100.

## imagelike250k-d256

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | Gates |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 24.2793 | 809.9852 | 493.6135 | 0.999951 | 0.999951 | PASS |
| 1 | 24.0382 | 748.4015 | 480.0280 | 0.999927 | 0.999951 | PASS |
| 2 | 24.1388 | 812.0364 | 475.9159 | 0.999976 | 0.999958 | PASS |
| 3 | 23.5602 | 815.4492 | 485.8590 | 0.999951 | 0.999958 | PASS |

### Average and comparison

| Version | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| V5 | 59.0959 | 117.6783 | 107.9007 | 0.999957 | 0.999951 |
| Candidate | 24.0041 | 796.4681 | 483.8541 | 0.999951 | 0.999955 |
| Candidate/V5 | 2.4619x | 6.7682x | 4.4843x | - | - |

Against the external reference strengthened by 10% (`27.2727s`, `95.7000/93.5000` QPS), the ratios are `1.1362x` for build, `8.3226x` for Top-10 and `5.1749x` for Top-100.

## Evidence and scope

- Each round recorded 8193 GPU serves, one snapshot build and zero CPU fallback. Peak sampled VRAM was 859439104 bytes; peak RSS was 3433406464 bytes.
- Default path: CAGRA NN-Descent degree 128/256 with niter=100, graph-only snapshot, persistent large-stack worker, MULTI_CTA and eight OpenMP threads.
- Separate Gaussian niter=100 screening passed both d128 and d256 gates. Shorter niter and IVF_PQ variants were rejected when they failed cross-distribution Recall or resource gates.
- SIFT-like and image-like datasets are real-image-derived surrogates, not official SIFT250K/Hidden250K. These results do not predict or guarantee official scores.
