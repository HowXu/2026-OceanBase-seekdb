# Intermediate-144 zero-configuration CAGRA validation

- Candidate revision: `d81a4a302`.
- Baseline revision: `634888d9b037153fa143d2c913ba4b25503e1d62`.
- No algorithm, OpenMP, CAGRA parameter or worker environment overrides were set.
- All eight rounds passed every Recall, QPS, resource and GPU-evidence gate.

## siftlike250k-d128

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | Gates |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 16.5205 | 869.4893 | 476.8672 | 0.992285 | 0.995649 | PASS |
| 1 | 17.0203 | 903.6169 | 484.7492 | 0.992407 | 0.995659 | PASS |
| 2 | 16.8397 | 860.9764 | 485.9459 | 0.992700 | 0.995718 | PASS |
| 3 | 16.8284 | 813.5845 | 483.8933 | 0.992163 | 0.995652 | PASS |

| Version | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| V5 | 51.4196 | 138.2844 | 124.8723 | 0.992273 | 0.995742 |
| Candidate | 16.8022 | 861.9168 | 482.8639 | 0.992389 | 0.995670 |
| Candidate/V5 | 3.0603x | 6.2329x | 3.8669x | - | - |

Against the external reference strengthened by 10%, build ratio is `1.2444x`; Top-10/100 QPS ratios are `5.3303x / 3.0697x`.

## imagelike250k-d256

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | Gates |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 20.3166 | 812.4490 | 478.2040 | 0.999951 | 0.999890 | PASS |
| 1 | 20.4427 | 816.3804 | 483.3150 | 0.999951 | 0.999890 | PASS |
| 2 | 20.5460 | 806.0243 | 481.7219 | 0.999902 | 0.999895 | PASS |
| 3 | 20.8492 | 759.1814 | 478.9770 | 0.999951 | 0.999893 | PASS |

| Version | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| V5 | 59.0959 | 117.6783 | 107.9007 | 0.999957 | 0.999951 |
| Candidate | 20.5386 | 798.5088 | 480.5545 | 0.999939 | 0.999892 |
| Candidate/V5 | 2.8773x | 6.7855x | 4.4537x | - | - |

Against the external reference strengthened by 10%, build ratio is `1.3279x`; Top-10/100 QPS ratios are `8.3439x / 5.1396x`.

## Evidence and scope

- Every round recorded 8193 GPU serves, one snapshot build and zero CPU fallback. Peak sampled VRAM was 859136000 bytes; peak RSS was 2853859328 bytes.
- Default path: CAGRA NN-Descent niter=100, final degree 128, intermediate degree 144, graph-only snapshot, persistent worker, MULTI_CTA and eight OpenMP threads.
- Separate Gaussian d128/d256 screening also passed all gates. Real-image-derived datasets are surrogates, not official SIFT250K/Hidden250K, and no official score is claimed.
