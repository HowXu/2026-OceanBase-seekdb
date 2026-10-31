# Paired hipVS contest evaluation

Status: **FAILED**

| Dataset | Round | Build speedup | Recall@10 delta | Recall@100 delta | Query speedup | Gates |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| gaussian250k-d128 | 0 | 4.7115 | 0.017480 | 0.037651 | 1.7093 | FAIL |
| gaussian250k-d128 | 1 | 4.7942 | 0.017456 | 0.037781 | 1.7417 | FAIL |
| clustered250k-d128 | 0 | 4.8520 | -0.015112 | -0.018613 | 1.5441 | PASS |
| clustered250k-d128 | 1 | 4.8057 | -0.015137 | -0.018625 | 1.5283 | PASS |
| gaussian250k-d256 | 0 | 4.6977 | 0.083862 | 0.089712 | 2.0311 | FAIL |
| gaussian250k-d256 | 1 | 4.6029 | 0.082129 | 0.089570 | 2.0927 | FAIL |
| clustered250k-d256 | 0 | 4.5701 | -0.018530 | -0.024006 | 1.6713 | PASS |
| clustered250k-d256 | 1 | 4.0303 | -0.018555 | -0.024106 | 1.6514 | PASS |
| gaussian250k-d384 | 0 | 3.6043 | -0.063647 | -0.093132 | 2.7921 | PASS |
| gaussian250k-d384 | 1 | 2.5391 | -0.064941 | -0.092993 | 2.8226 | PASS |

## Errors

- RuntimeError: evidence file rotated: /work/evals/full-20261031-005041/runs/gaussian250k-d768/round-0/baseline/log/seekdb.log

Synthetic local evaluation does not certify Hidden250K or an official score.

