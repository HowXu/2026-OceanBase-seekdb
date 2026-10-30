#!/usr/bin/env python3
"""Measure preloaded-table HNSW DDL and first-ready time. Requires a trace build.

The cuvs SQL route uses CAGRA snapshots; it does not export VSAG HNSW graphs.
No results are accepted without GPU routing evidence and a recall check.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time
import uuid


def make_dataset(n, dim, nq, k, seed):
    import numpy as np
    rng = np.random.default_rng(seed)
    base = rng.normal(size=(n, dim)).astype(np.float32)
    base /= np.linalg.norm(base, axis=1, keepdims=True)
    queries = rng.normal(size=(nq, dim)).astype(np.float32)
    queries /= np.linalg.norm(queries, axis=1, keepdims=True)
    truth = []
    for query in queries:
        distances = np.sum((base - query) ** 2, axis=1)
        # Stable ordering makes the generated ground truth reproducible.
        truth.append(np.argsort(distances, kind="stable")[:k].tolist())
    return base, queries, truth


def vector(values):
    # Nine significant decimal digits round-trip every float32 value.
    return "[" + ",".join(format(float(x), ".9g") for x in values) + "]"


def recall(results, truth):
    if len(results) != len(truth) or not truth:
        raise ValueError("query count mismatch")
    scores = []
    for actual, expected in zip(results, truth):
        if len(actual) != len(expected) or len(set(actual)) != len(actual):
            raise ValueError("incomplete or duplicate ANN results")
        scores.append(len(set(actual).intersection(expected)) / len(expected))
    return statistics.mean(scores)


def evidence_offset(path):
    stat = path.stat()
    return stat.st_ino, stat.st_size


def evidence_since(path, offset):
    stat = path.stat()
    if stat.st_ino != offset[0] or stat.st_size < offset[1]:
        raise RuntimeError(f"evidence file rotated or truncated: {path}")
    with path.open("rb") as stream:
        stream.seek(offset[1])
        return stream.read().decode("utf-8", errors="replace")


def summarize(trials, minimum_recall):
    grouped = {name: [t for t in trials if t["backend"] == name] for name in ("vsag", "cuvs")}
    if not grouped["vsag"] or len(grouped["vsag"]) != len(grouped["cuvs"]):
        raise ValueError("both backends need the same number of trials")
    for trial in trials:
        if trial["recall_at_k"] < minimum_recall:
            raise ValueError("recall threshold not met")
        if trial["backend"] == "cuvs" and not (
                trial["gpu_serves"] > 0 and trial["gpu_snapshot_builds"] > 0):
            raise ValueError("GPU build/serve evidence missing; fallback is not an acceleration result")
        if trial["backend"] == "vsag" and trial["gpu_serves"]:
            raise ValueError("CPU baseline unexpectedly used GPU")
    medians = {}
    for name, rows in grouped.items():
        medians[name] = {key: statistics.median(row[key] for row in rows)
                         for key in ("create_seconds", "first_query_seconds", "ready_seconds", "recall_at_k")}
    create_ratio = medians["vsag"]["create_seconds"] / medians["cuvs"]["create_seconds"]
    ready_ratio = medians["vsag"]["ready_seconds"] / medians["cuvs"]["ready_seconds"]
    return {"status": "VALID_ACCELERATION" if ready_ratio > 1 else "VALID_NO_SPEEDUP",
            "medians": medians, "ddl_speedup": create_ratio, "ready_speedup": ready_ratio,
            "speedup_definition": "median VSAG seconds / median cuvs seconds"}


def git_revision():
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=Path(__file__).resolve().parents[3],
                                       stderr=subprocess.DEVNULL, text=True, timeout=5).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def run(args, result, base, queries, truth):
    import pymysql
    db = "hipvs_bench_" + uuid.uuid4().hex[:12]
    connection = pymysql.connect(host=args.host, port=args.port, user=args.user,
                                 password=os.environ.get(args.password_env, ""), autocommit=True,
                                 connect_timeout=10, read_timeout=args.timeout + 30,
                                 write_timeout=args.timeout + 30)
    result["database"] = db
    created = False
    try:
        with connection.cursor() as cursor:
            cursor.execute("SELECT version()")
            result["server_version"] = cursor.fetchone()[0]
            cursor.execute(f"SET ob_query_timeout={args.timeout * 1000000}")
            # DDL ef_search does not set the query session variable. Record the
            # value actually used by SQL ANN so recall comparisons are valid.
            cursor.execute(f"SET SESSION ob_hnsw_ef_search={args.ef_search}")
            cursor.execute("SELECT @@session.ob_hnsw_ef_search")
            result["effective_hnsw_ef_search"] = int(cursor.fetchone()[0])
            if result["effective_hnsw_ef_search"] != args.ef_search:
                raise RuntimeError("session HNSW search effort did not take effect")
            cursor.execute(f"CREATE DATABASE `{db}`")
            created = True
            cursor.execute(f"USE `{db}`")
            for repeat in range(args.repeats):
                # Alternate order to reduce a fixed warm-cache advantage.
                for backend in (("vsag", "cuvs") if repeat % 2 == 0 else ("cuvs", "vsag")):
                    table = f"items_{backend}_{repeat}"
                    cursor.execute(f"CREATE TABLE `{table}` (id BIGINT PRIMARY KEY, v VECTOR({args.dim}))")
                    for start in range(0, len(base), args.batch_size):
                        rows = [(i, vector(base[i])) for i in range(start, min(start + args.batch_size, len(base)))]
                        cursor.executemany(f"INSERT INTO `{table}` VALUES (%s,%s)", rows)
                    cursor.execute(f"SELECT COUNT(*) FROM `{table}`")
                    if cursor.fetchone()[0] != args.rows:
                        raise RuntimeError("pre-DDL row count mismatch")
                    trace_start = evidence_offset(args.trace_file)
                    log_start = evidence_offset(args.server_log)
                    started = time.perf_counter()
                    cursor.execute(f"CREATE VECTOR INDEX idx ON `{table}`(v) WITH "
                                   f"(distance=l2,type=hnsw,lib={backend},m={args.m},ef_construction={args.ef_construction},ef_search={args.ef_search})")
                    create_seconds = time.perf_counter() - started
                    answers = []
                    first_seconds = None
                    for query in queries:
                        started = time.perf_counter()
                        cursor.execute(f"SELECT id FROM `{table}` ORDER BY l2_distance(v,%s) APPROXIMATE LIMIT {args.k}",
                                       (vector(query),))
                        answers.append([int(row[0]) for row in cursor.fetchall()])
                        if first_seconds is None:
                            first_seconds = time.perf_counter() - started
                    trace = evidence_since(args.trace_file, trace_start)
                    log = evidence_since(args.server_log, log_start)
                    stem = args.output.parent / f"{args.output.stem}.{backend}.{repeat}"
                    stem.with_suffix(stem.suffix + ".trace").write_text(trace, encoding="utf-8")
                    stem.with_suffix(stem.suffix + ".log").write_text(log, encoding="utf-8")
                    trial = {"backend": backend, "repeat": repeat, "create_seconds": create_seconds,
                             "first_query_seconds": first_seconds, "ready_seconds": create_seconds + first_seconds,
                             "recall_at_k": recall(answers, truth),
                             "gpu_serves": len(re.findall(r"^cuvs_serve\s", trace, re.MULTILINE)),
                             "gpu_snapshot_builds": len(re.findall(
                                 r"serialized GPU CAGRA snapshot[^\n]*built=true", log)),
                             "answers": answers}
                    result["trials"].append(trial)
                    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
                    print(f"{backend} trial={repeat} DDL={create_seconds:.3f}s "
                          f"ready={trial['ready_seconds']:.3f}s recall={trial['recall_at_k']:.4f}", flush=True)
                    if trial["recall_at_k"] < args.minimum_recall:
                        raise ValueError(f"{backend} recall {trial['recall_at_k']:.4f} below "
                                         f"{args.minimum_recall:.4f}; calibrate graph and search parameters before timing")
                    cursor.execute(f"DROP TABLE `{table}`")
            result.update(summarize(result["trials"], args.minimum_recall))
    finally:
        try:
            if created and not args.keep_database:
                with connection.cursor() as cursor:
                    cursor.execute(f"DROP DATABASE `{db}`")
        finally:
            connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--password-env", default="SEEKDB_PASSWORD")
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261030)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--batch-size", type=int, default=250)
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--minimum-recall", type=float, default=0.95)
    parser.add_argument("--ef-search", type=int, default=200,
                        help="SQL session HNSW search effort; increase until the CPU baseline meets the same recall target")
    parser.add_argument("--m", type=int, default=16, help="CPU HNSW graph degree (recorded explicitly; cuvs uses its own CAGRA graph degree)")
    parser.add_argument("--ef-construction", type=int, default=200)
    parser.add_argument("--trace-file", type=Path)
    parser.add_argument("--server-log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare-only", action="store_true", help="generate dataset evidence without connecting to seekdb")
    parser.add_argument("--keep-database", action="store_true")
    args = parser.parse_args()
    if args.rows < 256 or args.dim < 1 or not 1 <= args.k <= args.rows or min(
            args.queries, args.repeats, args.batch_size, args.timeout, args.ef_construction) < 1 or not 0 <= args.minimum_recall <= 1 or not 1 <= args.ef_search <= 160000 or not 5 <= args.m <= 128:
        parser.error("invalid dataset, repetition, timeout or recall parameters")
    if not args.prepare_only and (not args.trace_file or not args.server_log):
        parser.error("--trace-file and --server-log are required to prove GPU construction and serving")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = {"status": "RUNNING", "trials": [], "source_revision": git_revision(),
              "platform": platform.platform(), "parameters": {key: str(value) if isinstance(value, Path) else value
                                                             for key, value in vars(args).items()},
              "algorithm_note": "VSAG HNSW vs hipVS CAGRA in the seekdb HNSW SQL route; graph parameters differ",
              "cpu_configuration": {"m": args.m, "ef_construction": args.ef_construction, "ef_search": args.ef_search},
              "gpu_configuration": {"graph_degree": 128, "intermediate_graph_degree": 256,
                                    "nn_descent_niter": 100, "itopk_minimum": 1024 if args.dim > 256 else 512,
                                    "search_mode": "MULTI_CTA" if args.dim > 256 or args.k * 4 > 512 else "library_default"}}
    try:
        base, queries, truth = make_dataset(args.rows, args.dim, args.queries, args.k, args.seed)
        result["base_sha256"] = hashlib.sha256(base.tobytes()).hexdigest()
        result["queries_sha256"] = hashlib.sha256(queries.tobytes()).hexdigest()
        result["ground_truth"] = truth
        if args.prepare_only:
            result["status"] = "DATASET_ONLY_NO_GPU_MEASUREMENT"
        else:
            run(args, result, base, queries, truth)
    except Exception as exc:
        result["status"] = "FAILED"
        result["error"] = f"{type(exc).__name__}: {exc}"
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(result["status"], flush=True)
    return 1 if result["status"] == "FAILED" else 0


if __name__ == "__main__":
    raise SystemExit(main())
