# Frozen V5 four-round contest baseline

- Revision: `634888d9b037153fa143d2c913ba4b25503e1d62`
- Scope: FP32 + L2, synthetic gaussian 250K at dimensions 128 and 256; not official SIFT/Hidden data.
- Timing: DDL plus first Top-10 query. QPS: 4096 independent SQL queries for each Top-10/100.
- The evidence-incomplete fourth d256 attempt was discarded and replaced with a fresh valid run.

## Per-round results

| Dataset | Round | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| gaussian250k-d128 | 0 | 51.2879 | 130.4169 | 116.3744 | 0.991748 | 0.971165 |
| gaussian250k-d128 | 1 | 50.7478 | 129.0889 | 116.4034 | 0.991602 | 0.971042 |
| gaussian250k-d128 | 2 | 50.7481 | 130.4813 | 119.5410 | 0.991870 | 0.971116 |
| gaussian250k-d128 | 3 | 53.0740 | 131.1281 | 119.8905 | 0.991846 | 0.971113 |
| gaussian250k-d256 | 0 | 61.1675 | 109.1823 | 100.5225 | 0.923120 | 0.860371 |
| gaussian250k-d256 | 1 | 56.6307 | 106.6588 | 98.4612 | 0.922559 | 0.860586 |
| gaussian250k-d256 | 2 | 60.7402 | 107.2439 | 99.6923 | 0.921606 | 0.860334 |
| gaussian250k-d256 | 3 | 54.8251 | 106.6268 | 97.9529 | 0.923438 | 0.860466 |

## Four-round averages

| Dataset | Ready (s) | QPS@10 | QPS@100 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|
| gaussian250k-d128 | 51.4645 | 130.2788 | 118.0523 | 0.991766 | 0.971109 |
| gaussian250k-d256 | 58.3409 | 107.4280 | 99.1572 | 0.922681 | 0.860439 |

## External reference with additional 10% performance

The supplied V5 reference is strengthened by multiplying QPS by 1.10 and dividing build time by 1.10.

| Dataset | Ready (s) | QPS@10 | QPS@100 |
|---|---:|---:|---:|
| gaussian250k-d128 | 20.9091 | 161.7000 | 157.3000 |
| gaussian250k-d256 | 27.2727 | 95.7000 | 93.5000 |

Source reports: `/work/evals/v5-four-20261031-063937` and `/work/evals/v5-d256-replacement-20261031-070228`.

This archive is the fixed comparison baseline for subsequent optimization rounds.
