#!/usr/bin/env python3
"""Offline paired evaluation for frozen V5 and a candidate hipVS build."""

import json
import math
import statistics
from pathlib import Path

GIB = 1024 ** 3


def recall_at_k(actual, truth):
    if len(actual) != len(truth) or not truth:
        raise ValueError("query count mismatch")
    scores = []
    for got, expected in zip(actual, truth):
        if len(got) != len(expected) or len(set(got)) != len(got):
            raise ValueError("incomplete or duplicate ANN results")
        scores.append(len(set(got).intersection(expected)) / len(expected))
    return statistics.fmean(scores)


def validate_ids(actual, rows, k):
    for answer in actual:
        if len(answer) != k:
            raise ValueError("incorrect result count")
        if len(set(answer)) != k:
            raise ValueError("duplicate result ID")
        if any(type(value) is not int or not 0 <= value < rows for value in answer):
            raise ValueError("result ID outside valid range")


def percentile(values, q):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty latency sample")
    position = (len(ordered) - 1) * q / 100
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def paired_metrics(baseline, candidate):
    recall_delta = {k: baseline["recall"][k] - candidate["recall"][k] for k in (10, 100)}
    qps_ratio = {k: candidate["qps"][k] / baseline["qps"][k] for k in (10, 100)}
    return {
        "build_speedup": baseline["ready_seconds"] / candidate["ready_seconds"],
        "recall": dict(candidate["recall"]),
        "baseline_recall": dict(baseline["recall"]),
        "recall_delta": recall_delta,
        "query_qps_ratio": qps_ratio,
        "query_speedup": math.sqrt(qps_ratio[10] * qps_ratio[100]),
    }


def evaluate_gates(comparison, resources, dataset_kind):
    gates = []

    def add(name, passed, actual, limit):
        gates.append({"name": name, "passed": bool(passed), "actual": actual, "limit": limit})

    minimums = {"sift": (0.95, 0.95), "hidden": (0.90, 0.85)}.get(dataset_kind)
    if minimums:
        for k, minimum in zip((10, 100), minimums):
            add(f"recall@{k}_minimum", comparison["recall"][k] >= minimum,
                comparison["recall"][k], minimum)
    for k in (10, 100):
        add(f"recall@{k}_baseline_delta", comparison["recall_delta"][k] <= 0.005 + 1e-12,
            comparison["recall_delta"][k], 0.005)
        add(f"query_regression@{k}", comparison["query_qps_ratio"][k] >= 0.90,
            comparison["query_qps_ratio"][k], 0.90)
    add("relative_vram_limit",
        resources["candidate_vram_bytes"] <= 1.25 * resources["baseline_vram_bytes"],
        resources["candidate_vram_bytes"], 1.25 * resources["baseline_vram_bytes"])
    add("physical_vram_limit", resources["candidate_vram_bytes"] <= 45 * GIB,
        resources["candidate_vram_bytes"], 45 * GIB)
    add("host_memory_limit", resources["candidate_rss_bytes"] <= 96 * GIB,
        resources["candidate_rss_bytes"], 96 * GIB)
    return gates


def write_reports(result, output_dir):
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if result.get("errors") or not result.get("rounds"):
        result["status"] = "FAILED"
    elif result.get("status") == "RUNNING":
        result["status"] = "COMPLETED"
    (output_dir / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# Paired hipVS contest evaluation", "", f"Status: **{result['status']}**", ""]
    if result.get("errors"):
        lines.extend(["## Errors", ""] + [f"- {error}" for error in result["errors"]])
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
