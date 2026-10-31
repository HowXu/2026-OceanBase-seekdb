# Default BruteForce four-round experimental report

> **NOT ELIGIBLE FOR SUBMISSION:** this candidate calls `cuvsBruteForceSearch()`.
> Contest rules require the fixed hipVS/cuVS CAGRA Search implementation. All results below are experimental only.

- Branch: `hipvs-exact-ivf-contest`
- Candidate revision: `190e254ce76732b9c2bee0311577da4906b8ec41`
- Algorithm environment: `SEEKDB_CUVS_ALGO` explicitly unset; zero-configuration default BruteForce.
- Data: normalized synthetic gaussian FP32 + L2, not official SIFT250K/Hidden250K.
- Remote report: `/work/evals/head-default-four-20261031-070633`.

## Per-round candidate results

| Dataset | Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 | GPU evidence |
|---|---:|---:|---:|---:|---:|---:|---|
| gaussian250k-d128 | 0 | 6.4032 | 223.2258 | 186.4100 | 1.000000 | 1.000000 | PASS |
| gaussian250k-d128 | 1 | 6.3938 | 223.7058 | 185.6160 | 1.000000 | 1.000000 | PASS |
| gaussian250k-d128 | 2 | 6.4859 | 221.9638 | 185.7604 | 1.000000 | 1.000000 | PASS |
| gaussian250k-d128 | 3 | 6.3962 | 221.1843 | 185.8993 | 1.000000 | 1.000000 | PASS |
| gaussian250k-d256 | 0 | 7.9202 | 201.6673 | 160.9100 | 1.000000 | 0.999998 | PASS |
| gaussian250k-d256 | 1 | 7.9215 | 202.5393 | 161.2245 | 1.000000 | 0.999998 | PASS |
| gaussian250k-d256 | 2 | 7.9179 | 200.9542 | 158.9296 | 1.000000 | 0.999998 | PASS |
| gaussian250k-d256 | 3 | 7.9096 | 210.9513 | 171.0509 | 1.000000 | 0.999998 | PASS |

## Four-round averages

| Dataset | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| gaussian250k-d128 | 6.4198 | 222.5199 | 185.9214 | 1.000000 | 1.000000 |
| gaussian250k-d256 | 7.9173 | 204.0280 | 163.0288 | 1.000000 | 0.999998 |

## Speedup over archived remote V5

| Dataset | Build speedup | QPS ratio@10 | QPS ratio@100 |
|---|---:|---:|---:|
| gaussian250k-d128 | 8.0165x | 1.7080x | 1.5749x |
| gaussian250k-d256 | 7.3688x | 1.8992x | 1.6441x |

## Speedup over supplied V5 reference strengthened by 10%

Reference QPS is multiplied by 1.10 and reference build time is divided by 1.10.

| Dataset | Build speedup | QPS ratio@10 | QPS ratio@100 |
|---|---:|---:|---:|
| gaussian250k-d128 | 3.2570x | 1.3761x | 1.1820x |
| gaussian250k-d256 | 3.4447x | 2.1320x | 1.7436x |

## Interpretation

- The zero-configuration binary did exercise the GPU BruteForce path with complete per-run GPU evidence.
- These measurements cannot be used as a compliant contest submission or official-score estimate.
- Subsequent compliant work must restore CAGRA Search and compare against the archived V5 baseline again.
