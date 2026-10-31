# IVF-PQ graph refinement investigation

## Motivation and baseline

BruteForce build is fast because it retains the dataset without constructing a graph. In the current CAGRA candidate, the measured build plus internal serialization time is 16.49s (d128) or 15.90s (d256); the outer flow accounts for roughly another 6.45s or 8.35s. Base upload itself is only about 0.11s, so dataset upload is not the main remaining opportunity.

The original default IVF_PQ graph builder remains fast: a repeated Gaussian d128 run completed in 12.3126s, but failed Recall gates as in the archived earlier experiment.

## Explicit parameter path

The fixed public defaults were inspected: n_probes=5, refinement_rate=1, four-bit PQ, ten kmeans iterations and FP16 IVF search types. Experimental bridge revisions supplied these values explicitly, added conservative allocation and tested higher n_probes/refinement rates.

All manually parameterized runs were dramatically slower. Even the nominal p5/x1 control exceeded 40–120 seconds. At one sampled point GPU utilization was 0% and observer used about 117% of one CPU. Device-base input did not correct the slow path. Timed-out runs have no valid Recall or performance result and were not included in any average.

## Decision

The entire explicit IVF parameter calibration series was reverted in `b732919a9`; production source is identical to the pre-experiment bridge. Do not use these timed-out experiments to judge n_probes or refinement_rate individually, and do not reintroduce the slow wrapper without first understanding the fixed library's internal default-parameter path.

The default CAGRA NN-Descent candidate and its archived four-round results were not changed by this investigation.
