# Final zero-configuration CAGRA build/query validation

- Candidate revision: `b4718ba2a`.
- Baseline revision: `634888d9b037153fa143d2c913ba4b25503e1d62`.
- No algorithm, OpenMP, CAGRA parameter or worker environment overrides were set.
- Every round has exactly 8193 GPU serves, one snapshot build and zero CPU fallback; all eight rounds passed every gate.

## siftlike250k-d128

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | Gates |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 6.4765 | 997.7918 | 509.2676 | 0.992627 | 0.995708 | PASS |
| 1 | 6.4825 | 959.7411 | 498.2036 | 0.992505 | 0.995728 | PASS |
| 2 | 6.5815 | 984.0130 | 504.7127 | 0.992334 | 0.995662 | PASS |
| 3 | 6.3847 | 962.9357 | 502.0133 | 0.992334 | 0.995752 | PASS |

| Version | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| V5 | 51.4196 | 138.2844 | 124.8723 | 0.992273 | 0.995742 |
| Candidate | 6.4813 | 976.1204 | 503.5493 | 0.992450 | 0.995712 |
| Candidate/V5 | 7.9335x | 7.0588x | 4.0325x | - | - |

Against the external reference strengthened by 10%, build ratio is `3.2261x`; Top-10/100 QPS ratios are `6.0366x / 3.2012x`. Both build ratios exceed the 3x cap.

## imagelike250k-d256

| Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | Gates |
|---:|---:|---:|---:|---:|---:|---|
| 0 | 8.3810 | 747.3063 | 466.1592 | 0.999951 | 0.999966 | PASS |
| 1 | 8.0921 | 746.8367 | 474.4020 | 0.999951 | 0.999968 | PASS |
| 2 | 8.3847 | 763.0818 | 476.5421 | 0.999951 | 0.999968 | PASS |
| 3 | 8.2500 | 753.1757 | 479.8203 | 0.999951 | 0.999971 | PASS |

| Version | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| V5 | 59.0959 | 117.6783 | 107.9007 | 0.999957 | 0.999951 |
| Candidate | 8.2769 | 752.6002 | 474.2309 | 0.999951 | 0.999968 |
| Candidate/V5 | 7.1398x | 6.3954x | 4.3951x | - | - |

Against the external reference strengthened by 10%, build ratio is `3.2950x`; Top-10/100 QPS ratios are `7.8642x / 5.0720x`. Both build ratios exceed the 3x cap.

## Evidence and scope

- Peak sampled VRAM was 859152384 bytes; peak RSS was 2752630784 bytes.
- Default path: CAGRA NN-Descent niter=3, final degree 128, intermediate degree 144, itopk=2048, graph-only snapshot, persistent worker, MULTI_CTA and eight OpenMP threads.
- Trace output uses a reused stream and flushes each completed serve, preserving all 8193 records per round without per-row open/close overhead.
- Gaussian d128/d256 screening passed. Real-image-derived datasets are surrogates, not official SIFT250K/Hidden250K; these results meet the local reference caps but do not guarantee an official full score.
