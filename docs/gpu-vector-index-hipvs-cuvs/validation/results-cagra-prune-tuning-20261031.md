# CAGRA pruning and intermediate-degree investigation

## ROCm profile

Standalone 250K×128 FP32 CAGRA builds used the same fixed hipVS library and eight OpenMP threads. GPU timings came from `rocprof --stats`.

| Configuration | local_join | kern_prune | Total build wall time |
|---|---:|---:|---:|
| niter=100, intermediate=256 | 7.122s / 100 calls | 3.336s | 15.524s |
| niter=25, intermediate=256 | 1.691s / 25 calls | 3.594s | 8.769s |
| niter=100, intermediate=192 | 7.081s / 100 calls | 1.391s | 12.701s |

Reducing the intermediate degree attacks pruning without reducing NN-Descent iterations or final degree 128. The niter=25 configuration is not a candidate because its Gaussian d256 Recall failed in the earlier SQL gate.

## Gaussian SQL screening

Every row below passed all Recall, Recall-delta, QPS-regression, VRAM, RSS and GPU-evidence gates.

| Intermediate | d128 ready (s) | d128 Recall@10/@100 | d256 ready (s) | d256 Recall@10/@100 |
|---:|---:|---:|---:|---:|
| 192 | 19.1467 | 0.992993 / 0.970618 | 22.4823 | 0.930664 / 0.861731 |
| 160 | 18.4545 | 0.993457 / 0.970818 | 22.1545 | 0.930151 / 0.863286 |
| 144 | 17.3906 | 0.993042 / 0.970728 | 21.6614 | 0.929736 / 0.864023 |
| 136 | 17.4338 | gates PASS | 21.4568 | 0.930542 / 0.863965 |

The 136 versus 144 difference is within run-to-run noise. Revision `d81a4a302` selects 144 to retain a larger candidate margin. It remains a zero-configuration default: final degree 128, niter=100, MULTI_CTA, persistent worker and eight OpenMP threads.

These are surrogate datasets, not official SIFT250K/Hidden250K results. Four-round real-image-derived regression is archived separately after completion.
