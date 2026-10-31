#!/usr/bin/env python3
"""Offline paired evaluation for frozen V5 and a candidate hipVS build."""

import argparse
import dataclasses
import gzip
import hashlib
import json
import math
import os
import re
import select
import socket
import statistics
import subprocess
import threading
import time
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
        DatasetSpec("siftlike250k-d128", 250_000, 128, 4096, "siftlike", 20261109),
        DatasetSpec("imagelike250k-d256", 250_000, 256, 4096, "siftlike", 20261110),
        DatasetSpec("gaussian250k-d128", 250_000, 128, 4096, "gaussian", 20261101),
        DatasetSpec("clustered250k-d128", 250_000, 128, 4096, "clustered", 20261102),
        DatasetSpec("gaussian250k-d256", 250_000, 256, 4096, "gaussian", 20261103),
        DatasetSpec("clustered250k-d256", 250_000, 256, 4096, "clustered", 20261104),
        DatasetSpec("gaussian250k-d384", 250_000, 384, 4096, "gaussian", 20261105),
        DatasetSpec("clustered1m-d128", 1_000_000, 128, 4096, "clustered", 20261107),
        DatasetSpec("clustered1m-d256", 1_000_000, 256, 4096, "clustered", 20261108),
    ]


def normalize(rows):
    rows /= np.linalg.norm(rows, axis=1, keepdims=True)
    return rows


def siftlike_descriptor(patch):
    """Build a single-scale 4x4x8 SIFT-like descriptor without OpenCV."""
    if patch.shape != (32, 32) or not np.isfinite(patch).all():
        return None
    patch = np.asarray(patch, dtype=np.float32)
    kernel = np.asarray([1, 4, 6, 4, 1], dtype=np.float32) / 16.0
    padded = np.pad(patch, 2, mode="reflect")
    smoothed = sum(kernel[i] * padded[i:i + 32, 2:34] for i in range(5))
    padded = np.pad(smoothed, ((2, 2), (0, 0)), mode="reflect")
    smoothed = sum(kernel[i] * padded[i:i + 32, :] for i in range(5))
    gy, gx = np.gradient(smoothed)
    magnitude = np.hypot(gx, gy)
    if float(np.sqrt(np.mean(magnitude * magnitude))) < 0.01:
        return None
    coordinates = np.arange(32, dtype=np.float32) - 15.5
    yy, xx = np.meshgrid(coordinates, coordinates, indexing="ij")
    weight = np.exp(-(xx * xx + yy * yy) / (2.0 * 8.0 * 8.0)).astype(np.float32)
    directions = np.floor((np.arctan2(gy, gx) % (2.0 * np.pi)) * (8.0 / (2.0 * np.pi))).astype(np.int32)
    cells = []
    for bin_index in range(8):
        weighted = magnitude * weight * (directions == bin_index)
        cells.append(weighted.reshape(4, 8, 4, 8).sum(axis=(1, 3)))
    descriptor = np.stack(cells, axis=-1).reshape(-1).astype(np.float32)
    norm = float(np.linalg.norm(descriptor))
    if norm <= 1e-12:
        return None
    descriptor /= norm
    descriptor = np.minimum(descriptor, 0.2)
    norm = float(np.linalg.norm(descriptor))
    if norm <= 1e-12:
        return None
    descriptor /= norm
    return descriptor.astype(np.float32)


def siftlike_arrays(spec, images_root, metadata=None):
    """Sample base and held-out query descriptors from real image patches."""
    from PIL import Image

    rng = np.random.default_rng(spec.seed)
    images = []
    source_hash = hashlib.sha256()
    for path in sorted(Path(images_root).rglob("*")):
        if not path.is_file():
            continue
        try:
            with Image.open(path) as source:
                if source.width < 128 or source.height < 128:
                    continue
                gray = np.asarray(source.convert("L"), dtype=np.float32) / 255.0
                images.append(gray)
                source_hash.update(file_sha256(path).encode("ascii"))
        except (OSError, ValueError):
            continue
    if not images:
        raise RuntimeError(f"no eligible SIFT-like source images: {images_root}")
    if metadata is not None:
        metadata.update({"generator": "real-image-siftlike-v1", "source_images": len(images),
                         "source_images_sha256": source_hash.hexdigest()})

    def random_patch(image_index=None):
        image_index = int(rng.integers(len(images))) if image_index is None else image_index
        image = images[image_index]
        size = int(rng.choice(np.asarray([32, 48, 64, 96])))
        x = int(rng.integers(image.shape[1] - size + 1))
        y = int(rng.integers(image.shape[0] - size + 1))
        return image_index, x, y, size

    def extract(metadata):
        image_index, x, y, size = metadata
        image = images[image_index]
        patch = Image.fromarray(image[y:y + size, x:x + size]).resize(
            (32, 32), Image.Resampling.BILINEAR)
        return siftlike_descriptor(np.asarray(patch, dtype=np.float32))

    if spec.dim <= 0 or spec.dim % 128 != 0:
        raise ValueError(f"SIFT-like dimension must be a multiple of 128: {spec.dim}")
    blocks = spec.dim // 128
    base = np.empty((spec.rows, spec.dim), dtype=np.float32)
    metadata = []
    while len(metadata) < spec.rows:
        row_metadata = []
        for block in range(blocks):
            while True:
                patch_metadata = random_patch()
                descriptor = extract(patch_metadata)
                if descriptor is not None:
                    base[len(metadata), block * 128:(block + 1) * 128] = descriptor
                    row_metadata.append(patch_metadata)
                    break
        metadata.append(tuple(row_metadata))

    queries = np.empty((spec.nq, spec.dim), dtype=np.float32)
    related = spec.nq * 7 // 10
    for row in range(spec.nq):
        for block in range(blocks):
            while True:
                if row < related:
                    image_index, x, y, size = metadata[int(rng.integers(len(metadata)))][block]
                    image = images[image_index]
                    center_x, center_y = x + size / 2.0, y + size / 2.0
                    size = int(np.clip(round(size * rng.uniform(0.9, 1.1)), 32, 96))
                    x = int(np.clip(round(center_x - size / 2.0 + rng.integers(-3, 4)), 0, image.shape[1] - size))
                    y = int(np.clip(round(center_y - size / 2.0 + rng.integers(-3, 4)), 0, image.shape[0] - size))
                    patch_metadata = (image_index, x, y, size)
                else:
                    patch_metadata = random_patch()
                descriptor = extract(patch_metadata)
                if descriptor is not None:
                    queries[row, block * 128:(block + 1) * 128] = descriptor
                    break
    return base, queries


def generate_arrays(spec, metadata=None):
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
    elif spec.distribution == "siftlike":
        base, queries = siftlike_arrays(spec, Path.home() / "Pictures", metadata)
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
    generation_metadata = {}
    base, queries = generate_arrays(spec, generation_metadata)
    truth = exact_topk(base, queries, 100)
    manifest = {"version": 1, "spec": dataclasses.asdict(spec), **generation_metadata}
    arrays = {"base": base, "queries": queries, "truth": truth}
    for name, array in arrays.items():
        np.save(paths[name], array, allow_pickle=False)
        manifest[f"{name}_sha256"] = array_sha256(array)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return Dataset(manifest=manifest, **arrays)


@dataclasses.dataclass(frozen=True)
class ServerConfig:
    role: str
    observer: Path
    bridge: Path
    port: int
    base_dir: Path


def valid_table(table):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", table):
        raise ValueError(f"unsafe table name: {table}")
    return table


def create_index_sql(table, backend):
    valid_table(table)
    if backend not in ("cuvs", "vsag"):
        raise ValueError("unknown vector backend")
    return (f"CREATE VECTOR INDEX idx_items ON {table}(v) WITH "
            f"(distance=l2,type=hnsw,lib={backend},m=16,ef_construction=200,ef_search=64)")


def query_sql(table, k):
    valid_table(table)
    if k not in (10, 100):
        raise ValueError("only Top-10 and Top-100 are supported")
    return (f"SELECT id FROM {table} ORDER BY l2_distance(v,%s) "
            f"APPROXIMATE LIMIT {k}")


def vector_literal(values):
    return "[" + ",".join(format(float(value), ".9g") for value in values) + "]"


def latency_summary(latencies):
    return {"qps": len(latencies) / sum(latencies),
            "p50_ms": percentile(latencies, 50) * 1000,
            "p95_ms": percentile(latencies, 95) * 1000,
            "p99_ms": percentile(latencies, 99) * 1000}


def gpu_evidence(trace, log):
    serves = len(re.findall(r"^cuvs_serve\s", trace, re.MULTILINE))
    builds = len(re.findall(r"serialized GPU CAGRA snapshot[^\n]*built=true", log))
    fallbacks = len(re.findall(r"lazily built VSAG fallback|cpu_fallback_observed", log))
    return {"passed": serves > 0 and builds > 0 and fallbacks == 0,
            "gpu_serves": serves, "gpu_snapshot_builds": builds, "cpu_fallbacks": fallbacks}


def dataset_kind(name):
    return {"gaussian250k-d128": "sift", "siftlike250k-d128": "sift",
            "gaussian250k-d256": "hidden", "imagelike250k-d256": "hidden"}.get(name, "local")


def observer_command(config):
    base = config.base_dir
    command = [str(config.observer), "--nodaemon", "--port", str(config.port),
               "--data-dir", str(base / "store"), "--base-dir", str(base),
               "--parameter", "__min_full_resource_pool_memory=2147483648",
               "--parameter", "memory_budget=2G", "--parameter", "datafile_size=20G",
               "--parameter", "cpu_count=24", "--parameter", "local_ip=127.0.0.1"]
    env = dict(os.environ)
    env.pop("OB_VSAG_USE_CUVS", None)
    env.update({"LD_LIBRARY_PATH": f"{config.bridge.parent}:/opt/hipvs/lib:/opt/rocm/lib",
                "OB_VSAG_TRACE": "1", "OB_VSAG_TRACE_FILE": str(base / "hipvs.trace")})
    return command, env


def owned_process_cmdline(cmdline, observer, base_dir):
    return str(observer) in cmdline and f"--base-dir {base_dir}" in cmdline


class MysqlCursor:
    def __init__(self, client):
        self.client = client
        self.rows = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def execute(self, sql, params=None, expected_rows=None):
        self.rows = self.client.execute(sql, params, expected_rows)

    def fetchone(self):
        return self.rows[0] if self.rows else None

    def fetchall(self):
        return self.rows


class MysqlCli:
    DONE = "__SEEKDB_CONTEST_DONE__"

    def __init__(self, command, env=None, timeout=7200):
        self.timeout = timeout
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, env=env, bufsize=0)

    def cursor(self):
        return MysqlCursor(self)

    def execute(self, sql, params=None, expected_rows=None):
        if "\n" in sql or "\r" in sql:
            raise ValueError("SQL statement must be single-line")
        if params:
            if len(params) != 1 or sql.count("%s") != 1:
                raise ValueError("only one scalar SQL parameter is supported")
            value = str(params[0]).replace("\\", "\\\\").replace("'", "''")
            sql = sql.replace("%s", f"'{value}'")
        statement = sql.rstrip().rstrip(";") + ";"
        self.process.stdin.write(statement.encode("utf-8") + b"\n")
        if expected_rows is None:
            self.process.stdin.write(f"SELECT '{self.DONE}';\n".encode("ascii"))
        self.process.stdin.flush()
        rows = []
        while True:
            ready, _, _ = select.select([self.process.stdout], [], [], self.timeout)
            if not ready:
                raise TimeoutError(f"mysql query exceeded {self.timeout}s")
            line = self.process.stdout.readline()
            if not line:
                self.process.wait(5)
                error = self.process.stderr.read().decode("utf-8", errors="replace")
                raise RuntimeError(f"mysql client exited with {self.process.returncode}: {error}")
            text = line.decode("utf-8", errors="strict").rstrip("\r\n")
            if text == self.DONE:
                return rows
            rows.append(tuple(text.split("\t")))
            if expected_rows is not None and len(rows) == expected_rows:
                return rows

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(5)
        self.process.stdout.close()
        self.process.stderr.close()


def connect_mysql(config, timeout=7200):
    command = ["stdbuf", "-oL", "-eL", os.environ.get("MYSQL_BIN", "mysql"),
               "--protocol=TCP", "-h127.0.0.1",
               f"-P{config.port}", "-uroot", "-ANB", "--raw", "--local-infile=1",
               "--connect-timeout=10"]
    env = dict(os.environ)
    if os.environ.get("SEEKDB_PASSWORD"):
        env["MYSQL_PWD"] = os.environ["SEEKDB_PASSWORD"]
    client = MysqlCli(command, env=env, timeout=timeout)
    client.execute(f"SET ob_query_timeout={timeout * 1000000}")
    return client


def evidence_offset(path):
    try:
        stat = path.stat()
        return stat.st_ino, stat.st_size
    except FileNotFoundError:
        return None, 0


def evidence_since(path, offset):
    try:
        stat = path.stat()
    except FileNotFoundError:
        return ""
    if stat.st_ino != offset[0] and offset[0] is not None:
        raise RuntimeError(f"evidence file rotated: {path}")
    if stat.st_size < offset[1]:
        raise RuntimeError(f"evidence file truncated: {path}")
    with path.open("rb") as stream:
        stream.seek(offset[1])
        return stream.read().decode("utf-8", errors="replace")


class ResourceSampler:
    def __init__(self, pid):
        self.pid = pid
        self.samples = []
        self.errors = []
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def sample(self):
        rss = 0
        with open(f"/proc/{self.pid}/status", encoding="utf-8") as status:
            match = re.search(r"^VmRSS:\s+(\d+) kB", status.read(), re.MULTILINE)
            if match:
                rss = int(match.group(1)) * 1024
        output = subprocess.check_output(["rocm-smi", "--showmeminfo", "vram", "--showuse"],
                                         text=True, stderr=subprocess.STDOUT, timeout=10)
        vram_match = re.search(r"VRAM Total Used Memory \(B\):\s*(\d+)", output)
        use_match = re.search(r"GPU use \(%\):\s*(\d+)", output)
        if not vram_match or not use_match:
            raise RuntimeError("cannot parse rocm-smi resource sample")
        self.samples.append({"time": time.time(), "rss_bytes": rss,
                             "vram_bytes": int(vram_match.group(1)),
                             "gpu_percent": int(use_match.group(1))})

    def run(self):
        while not self.stop_event.is_set():
            try:
                self.sample()
            except Exception as exc:
                self.errors.append(f"{type(exc).__name__}: {exc}")
            self.stop_event.wait(1)

    def start(self):
        self.thread.start()

    def finish(self):
        self.stop_event.set()
        self.thread.join(15)
        return {"samples": len(self.samples), "errors": self.errors,
                "peak_rss_bytes": max((s["rss_bytes"] for s in self.samples), default=0),
                "peak_vram_bytes": max((s["vram_bytes"] for s in self.samples), default=0),
                "peak_gpu_percent": max((s["gpu_percent"] for s in self.samples), default=0)}


class ObserverServer:
    def __init__(self, config):
        self.config = config
        self.process = None
        self.console = None
        self.sampler = None

    def start(self):
        config = self.config
        if config.base_dir.exists() and any(config.base_dir.iterdir()):
            raise RuntimeError(f"refusing nonempty server directory: {config.base_dir}")
        config.base_dir.mkdir(parents=True, exist_ok=True)
        command, env = observer_command(config)
        self.console = (config.base_dir / "console.log").open("wb")
        self.process = subprocess.Popen(command, cwd=config.base_dir, env=env,
                                        stdout=self.console, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        self.sampler = ResourceSampler(self.process.pid)
        self.sampler.start()
        try:
            deadline = time.monotonic() + 600
            last_error = None
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError(f"observer exited with {self.process.returncode}: {config.base_dir}")
                try:
                    connection = connect_mysql(config, timeout=30)
                    with connection.cursor() as cursor:
                        cursor.execute("SELECT 1", expected_rows=1)
                    connection.close()
                    return self
                except Exception as exc:
                    last_error = f"{type(exc).__name__}: {exc}"
                    time.sleep(1)
            raise RuntimeError(f"observer readiness timeout: {config.base_dir}; last_error={last_error}")
        except Exception:
            self.stop()
            raise

    def stop(self):
        if self.process and self.process.poll() is None:
            try:
                cmdline = Path(f"/proc/{self.process.pid}/cmdline").read_bytes().replace(b"\0", b" ").decode()
            except FileNotFoundError:
                cmdline = ""
            if not owned_process_cmdline(cmdline, self.config.observer, self.config.base_dir):
                raise RuntimeError(f"refusing to stop unowned pid={self.process.pid}: {cmdline}")
            self.process.terminate()
            try:
                self.process.wait(30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(10)
        if self.console:
            self.console.close()
        if self.sampler:
            return self.sampler.finish()
        return {"samples": 0, "errors": ["sampler did not start"], "peak_rss_bytes": 0,
                "peak_vram_bytes": 0, "peak_gpu_percent": 0}

    def __enter__(self):
        return self.start()

    def __exit__(self, exc_type, exc, tb):
        self.resources = self.stop()


def write_base_tsv(spec, data, cache_dir):
    path = Path(cache_dir) / spec.name / "base.tsv"
    if path.exists():
        return path
    temporary = path.with_suffix(".tsv.tmp")
    with temporary.open("w", encoding="ascii", buffering=1024 * 1024) as stream:
        for start in range(0, spec.rows, 500):
            stop = min(start + 500, spec.rows)
            stream.writelines(f"{row}\t{vector_literal(data.base[row])}\n"
                              for row in range(start, stop))
    os.replace(temporary, path)
    return path


def load_data(connection, spec, data, cache_dir):
    tsv = write_base_tsv(spec, data, cache_dir)
    with connection.cursor() as cursor:
        cursor.execute("CREATE DATABASE contest_eval")
        cursor.execute("USE contest_eval")
        cursor.execute(f"CREATE TABLE items (id BIGINT PRIMARY KEY, v VECTOR({spec.dim}))")
        escaped = str(tsv).replace("'", "''")
        cursor.execute(f"LOAD DATA LOCAL INFILE '{escaped}' INTO TABLE items "
                       "FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (id,v)")
        cursor.execute("SELECT COUNT(*) FROM items", expected_rows=1)
        if int(cursor.fetchone()[0]) != spec.rows:
            raise RuntimeError("loaded row count mismatch")


def run_queries(connection, spec, data, k, raw_dir):
    answers = []
    latencies = []
    sql = query_sql("items", k)
    with connection.cursor() as cursor:
        for query in data.queries:
            started = time.perf_counter()
            cursor.execute(sql, (vector_literal(query),), expected_rows=k)
            answer = [int(row[0]) for row in cursor.fetchall()]
            latencies.append(time.perf_counter() - started)
            answers.append(answer)
    validate_ids(answers, spec.rows, k)
    recall = recall_at_k(answers, data.truth[:, :k].tolist())
    with gzip.open(raw_dir / f"top{k}.json.gz", "wt", encoding="utf-8") as stream:
        json.dump({"answers": answers, "latencies": latencies}, stream)
    return {"recall": recall, "latencies": latencies, **latency_summary(latencies)}


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_single(spec, data, config, raw_dir, cache_dir):
    raw_dir.mkdir(parents=True, exist_ok=True)
    result = {"role": config.role, "observer": str(config.observer),
              "observer_sha256": file_sha256(config.observer),
              "bridge_sha256": file_sha256(config.bridge), "port": config.port}
    with ObserverServer(config) as server:
        connection = connect_mysql(config)
        try:
            load_data(connection, spec, data, cache_dir)
            trace_path = config.base_dir / "hipvs.trace"
            log_path = config.base_dir / "log/seekdb.log"
            trace_start = evidence_offset(trace_path)
            log_start = evidence_offset(log_path)
            with connection.cursor() as cursor:
                started = time.perf_counter()
                cursor.execute(create_index_sql("items", "cuvs"))
                result["create_seconds"] = time.perf_counter() - started
                started = time.perf_counter()
                cursor.execute(query_sql("items", 10), (vector_literal(data.queries[0]),), expected_rows=10)
                cursor.fetchall()
                result["first_query_seconds"] = time.perf_counter() - started
            result["ready_seconds"] = result["create_seconds"] + result["first_query_seconds"]
            result["qps"] = {}
            result["recall"] = {}
            result["latency"] = {}
            for k in (10, 100):
                measured = run_queries(connection, spec, data, k, raw_dir)
                result["qps"][k] = measured["qps"]
                result["recall"][k] = measured["recall"]
                result["latency"][k] = {name: measured[name] for name in ("p50_ms", "p95_ms", "p99_ms")}
            trace = evidence_since(trace_path, trace_start)
            log = evidence_since(log_path, log_start)
            (raw_dir / "hipvs.trace").write_text(trace, encoding="utf-8")
            (raw_dir / "seekdb.log").write_text(log, encoding="utf-8")
            result["evidence"] = gpu_evidence(trace, log)
            if not result["evidence"]["passed"]:
                raise RuntimeError(f"GPU evidence missing or CPU fallback observed: {config.role}")
        finally:
            connection.close()
    result["resources"] = server.resources
    if not result["resources"]["samples"] or result["resources"]["errors"]:
        raise RuntimeError(f"resource sampling failed: {result['resources']['errors']}")
    return result


def run_evaluation(args):
    output_dir = Path(args.output_dir)
    cache_dir = Path(args.data_cache)
    if output_dir.exists() and any(output_dir.iterdir()):
        raise RuntimeError(f"refusing nonempty output directory: {output_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)
    specs = dataset_specs(args.mode)
    if args.dataset:
        selected = set(args.dataset)
        specs = [spec for spec in specs if spec.name in selected]
        missing = selected - {spec.name for spec in specs}
        if missing:
            raise ValueError(f"unknown or out-of-mode datasets: {sorted(missing)}")
    result = {"status": "RUNNING", "mode": args.mode, "round_count": args.rounds,
              "config": {"baseline_revision": args.baseline_revision,
                         "candidate_revision": args.candidate_revision,
                         "baseline_observer": str(args.baseline_observer),
                         "candidate_observer": str(args.candidate_observer),
                         "baseline_bridge": str(args.baseline_bridge),
                         "candidate_bridge": str(args.candidate_bridge)},
              "datasets": [dataclasses.asdict(spec) for spec in specs],
              "rounds": [], "errors": []}
    try:
        for spec in specs:
            data = load_or_create_dataset(spec, cache_dir)
            for round_index in range(args.rounds):
                roles = ("baseline", "candidate") if round_index % 2 == 0 else ("candidate", "baseline")
                measured = {}
                for role in roles:
                    observer = args.baseline_observer if role == "baseline" else args.candidate_observer
                    bridge = args.baseline_bridge if role == "baseline" else args.candidate_bridge
                    stem = f"{spec.name}/round-{round_index}/{role}"
                    config = ServerConfig(role, Path(observer), Path(bridge), args.port,
                                          output_dir / "runs" / stem)
                    print(f"START dataset={spec.name} round={round_index} role={role}", flush=True)
                    measured[role] = run_single(spec, data, config, output_dir / "raw" / stem, cache_dir)
                    print(f"DONE dataset={spec.name} role={role} "
                          f"ready={measured[role]['ready_seconds']:.3f}s "
                          f"recall10={measured[role]['recall'][10]:.6f} "
                          f"recall100={measured[role]['recall'][100]:.6f}", flush=True)
                comparison = paired_metrics(measured["baseline"], measured["candidate"])
                resources = {
                    "baseline_vram_bytes": measured["baseline"]["resources"]["peak_vram_bytes"],
                    "candidate_vram_bytes": measured["candidate"]["resources"]["peak_vram_bytes"],
                    "candidate_rss_bytes": measured["candidate"]["resources"]["peak_rss_bytes"]}
                gates = evaluate_gates(comparison, resources, dataset_kind(spec.name))
                passed = all(gate["passed"] for gate in gates)
                result["rounds"].append({"dataset": spec.name, "round": round_index,
                    "order": roles, "baseline": measured["baseline"], "candidate": measured["candidate"],
                    "comparison": comparison, "resources": resources, "gates": gates, "passed": passed})
                write_reports(result, output_dir)
    except Exception as exc:
        result["errors"].append(f"{type(exc).__name__}: {exc}")
        result["status"] = "FAILED"
    if not result["errors"]:
        complete = len(result["rounds"]) == len(specs) * args.rounds
        all_passed = complete and all(item["passed"] for item in result["rounds"])
        if args.mode == "quick":
            result["status"] = "QUICK_PASS" if all_passed else "FAILED"
        else:
            official = [item for item in result["rounds"] if dataset_kind(item["dataset"]) != "local"]
            official_passed = (len(official) == 2 * args.rounds and
                               all(item["passed"] for item in official))
            result["official_like_passed"] = official_passed
            result["local_strict_passed"] = all_passed
            result["status"] = ("LOCAL_STRICT_PASS" if all_passed and len(specs) == 8 else
                                "OFFICIAL_LIKE_PASS" if official_passed else
                                "SUBSET_PASS" if all_passed else "FAILED")
    write_reports(result, output_dir)
    return result


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("quick", "full"), default="quick")
    parser.add_argument("--dataset", action="append", help="dataset name; repeat to select several")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--port", type=int, default=2982)
    parser.add_argument("--baseline-observer", type=Path, required=True)
    parser.add_argument("--candidate-observer", type=Path, required=True)
    parser.add_argument("--baseline-bridge", type=Path, required=True)
    parser.add_argument("--candidate-bridge", type=Path, required=True)
    parser.add_argument("--baseline-revision", default="634888d9b037153fa143d2c913ba4b25503e1d62")
    parser.add_argument("--candidate-revision", default="unknown")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--data-cache", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    for name in ("baseline_observer", "candidate_observer", "baseline_bridge", "candidate_bridge"):
        if not getattr(args, name).is_file():
            parser.error(f"{name.replace('_', '-')} does not exist: {getattr(args, name)}")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        try:
            sock.bind(("127.0.0.1", args.port))
        except OSError as exc:
            parser.error(f"port {args.port} is unavailable: {exc}")
    return args


def main(argv=None):
    args = parse_args(argv)
    result = run_evaluation(args)
    print(f"{result['status']} reports={args.output_dir}", flush=True)
    return 0 if result["status"].endswith("PASS") else 1


def write_reports(result, output_dir):
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if result.get("errors") or not result.get("rounds"):
        result["status"] = "FAILED"
    (output_dir / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# Paired hipVS contest evaluation", "", f"Status: **{result['status']}**", ""]
    if result.get("rounds"):
        lines.extend(["| Dataset | Round | Build speedup | Recall@10 delta | Recall@100 delta | Query speedup | Gates |",
                      "| --- | ---: | ---: | ---: | ---: | ---: | --- |"])
        for item in result["rounds"]:
            comparison = item["comparison"]
            lines.append(f"| {item['dataset']} | {item['round']} | "
                         f"{comparison['build_speedup']:.4f} | {comparison['recall_delta'][10]:.6f} | "
                         f"{comparison['recall_delta'][100]:.6f} | {comparison['query_speedup']:.4f} | "
                         f"{'PASS' if item['passed'] else 'FAIL'} |")
        lines.append("")
    if result.get("errors"):
        lines.extend(["## Errors", ""] + [f"- {error}" for error in result["errors"]])
    lines.extend(["", "Synthetic local evaluation does not certify Hidden250K or an official score.", ""])
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
