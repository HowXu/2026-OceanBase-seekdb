#!/usr/bin/env python3
"""Offline paired evaluation for frozen V5 and a candidate hipVS build."""

import dataclasses
import hashlib
import json
import math
import statistics
from pathlib import Path

import numpy as np

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


@dataclasses.dataclass(frozen=True)
class DatasetSpec:
    name: str
    rows: int
    dim: int
    nq: int
    distribution: str
    seed: int


@dataclasses.dataclass
class Dataset:
    base: np.ndarray
    queries: np.ndarray
    truth: np.ndarray
    manifest: dict


def dataset_specs(mode):
    if mode == "quick":
        return [DatasetSpec("quick10k-d128", 10_000, 128, 4096, "gaussian", 20261031)]
    if mode != "full":
        raise ValueError(f"unknown dataset mode: {mode}")
    return [
        DatasetSpec("gaussian250k-d128", 250_000, 128, 4096, "gaussian", 20261101),
        DatasetSpec("clustered250k-d128", 250_000, 128, 4096, "clustered", 20261102),
        DatasetSpec("gaussian250k-d256", 250_000, 256, 4096, "gaussian", 20261103),
        DatasetSpec("clustered250k-d256", 250_000, 256, 4096, "clustered", 20261104),
        DatasetSpec("gaussian250k-d384", 250_000, 384, 4096, "gaussian", 20261105),
        DatasetSpec("gaussian250k-d768", 250_000, 768, 4096, "gaussian", 20261106),
        DatasetSpec("clustered1m-d128", 1_000_000, 128, 4096, "clustered", 20261107),
        DatasetSpec("clustered1m-d256", 1_000_000, 256, 4096, "clustered", 20261108),
    ]


def normalize(rows):
    rows /= np.linalg.norm(rows, axis=1, keepdims=True)
    return rows


def generate_arrays(spec):
    rng = np.random.default_rng(spec.seed)
    if spec.distribution == "gaussian":
        base = normalize(rng.standard_normal((spec.rows, spec.dim), dtype=np.float32))
        queries = rng.standard_normal((spec.nq, spec.dim), dtype=np.float32)
        near_count = spec.nq // 5
        queries[:near_count] = base[rng.integers(0, spec.rows, near_count)]
        queries[:near_count] += rng.normal(0, 0.01, (near_count, spec.dim))
        queries[near_count:near_count * 2] += 0.25
    elif spec.distribution == "clustered":
        centers = normalize(rng.standard_normal((64, spec.dim), dtype=np.float32))
        labels = rng.integers(0, len(centers), spec.rows)
        scales = np.geomspace(0.25, 2.0, spec.dim, dtype=np.float32)
        base = centers[labels] + rng.normal(0, 0.04, (spec.rows, spec.dim)) * scales
        normalize(base)
        queries = np.empty((spec.nq, spec.dim), dtype=np.float32)
        ordinary = spec.nq * 7 // 10
        boundary = spec.nq * 9 // 10
        queries[:ordinary] = centers[rng.integers(0, len(centers), ordinary)]
        queries[:ordinary] += rng.normal(0, 0.04, (ordinary, spec.dim)) * scales
        left = centers[rng.integers(0, len(centers), boundary - ordinary)]
        right = centers[rng.integers(0, len(centers), boundary - ordinary)]
        queries[ordinary:boundary] = (left + right) * 0.5
        queries[ordinary:boundary] += rng.normal(0, 0.02, (boundary - ordinary, spec.dim))
        queries[boundary:] = rng.normal(0, 1, (spec.nq - boundary, spec.dim)) + 0.35
    else:
        raise ValueError(f"unknown distribution: {spec.distribution}")
    return base.astype(np.float32), normalize(queries).astype(np.float32)


def exact_topk(base, queries, k, chunk=8):
    if not 1 <= k <= len(base):
        raise ValueError("invalid ground-truth k")
    base_squared = np.einsum("ij,ij->i", base, base)
    output = []
    for start in range(0, len(queries), chunk):
        block = queries[start:start + chunk]
        query_squared = np.einsum("ij,ij->i", block, block)
        distances = query_squared[:, None] + base_squared[None, :] - 2 * (block @ base.T)
        nearest = np.argpartition(distances, k - 1, axis=1)[:, :k]
        for row, ids in enumerate(nearest):
            ordered = sorted(ids.tolist(), key=lambda i: (float(distances[row, i]), i))
            output.append(ordered)
    return np.asarray(output, dtype=np.int32)


def array_sha256(array):
    contiguous = np.ascontiguousarray(array)
    digest = hashlib.sha256()
    digest.update(memoryview(contiguous))
    return digest.hexdigest()


def load_or_create_dataset(spec, cache_dir):
    directory = Path(cache_dir) / spec.name
    paths = {name: directory / f"{name}.npy" for name in ("base", "queries", "truth")}
    manifest_path = directory / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest.get("spec") != dataclasses.asdict(spec):
            raise RuntimeError(f"dataset manifest mismatch: {spec.name}")
        arrays = {name: np.load(path, allow_pickle=False, mmap_mode="r")
                  for name, path in paths.items()}
        for name, array in arrays.items():
            if array_sha256(array) != manifest.get(f"{name}_sha256"):
                raise RuntimeError(f"dataset hash mismatch: {spec.name}/{name}")
        return Dataset(manifest=manifest, **arrays)
    if directory.exists() and any(directory.iterdir()):
        raise RuntimeError(f"incomplete dataset cache: {directory}")
    directory.mkdir(parents=True, exist_ok=True)
    base, queries = generate_arrays(spec)
    truth = exact_topk(base, queries, 100)
    manifest = {"version": 1, "spec": dataclasses.asdict(spec)}
    arrays = {"base": base, "queries": queries, "truth": truth}
    for name, array in arrays.items():
        np.save(paths[name], array, allow_pickle=False)
        manifest[f"{name}_sha256"] = array_sha256(array)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return Dataset(manifest=manifest, **arrays)


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
