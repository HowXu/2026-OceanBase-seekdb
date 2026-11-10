#!/usr/bin/env python3
"""Local approximation of the performance workload in the task statement.

Default: 10,000 rows, 128-dim vectors, 100 rows per committed batch, then
1,000 TopK queries with K=10, half of them with an equality filter.
Reports write time, query time, peak RSS of the child (local stand-in for the
cgroup memory.peak), data directory size, and Recall@10 for both groups
against a brute-force float32 ground truth.

Local numbers are reference only: the official run uses Linux cgroup v2.
"""
import argparse
import json
import os
import random
import resource
import struct
import subprocess
import tempfile
import time


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--cats", type=int, default=10)
    ap.add_argument("--mode", default="exact")
    ap.add_argument("--index", action="store_true", help="CREATE VECTOR INDEX before queries")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--clustered", action="store_true", help="draw vectors around a few centers")
    a = ap.parse_args()
    rng = random.Random(a.seed)
    d = tempfile.mkdtemp(prefix="mvdb-bench-", dir=os.environ.get("MVDB_TMP"))
    centers = [[rng.gauss(0, 1) for _ in range(a.dim)] for _ in range(32)]

    def gen():
        if a.clustered:
            c = rng.choice(centers)
            return [f32(x + rng.gauss(0, 0.3)) for x in c]
        return [f32(rng.random()) for _ in range(a.dim)]

    vecs = [gen() for _ in range(a.rows)]
    cats = ["c%d" % rng.randrange(a.cats) for _ in range(a.rows)]
    p = subprocess.Popen([a.bin, "--data-dir", d, "--mode", a.mode], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1 << 20)
    assert json.loads(p.stdout.readline()) == {"type": "ready"}
    seq = [0]

    def send(sql):
        seq[0] += 1
        p.stdin.write(json.dumps({"id": seq[0], "op": "execute", "sql": sql, "search_mode": a.mode}) + "\n")
        p.stdin.flush()
        r = json.loads(p.stdout.readline())
        if r.get("ok") is not True or r.get("id") != seq[0]:
            raise RuntimeError("%s: %s" % (sql[:80], r))
        return r

    send("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(%d))" % a.dim)
    batches = []
    for s in range(0, a.rows, 100):
        vals = ",".join("(%d,'%s','[%s]')" % (i + 1, cats[i], ",".join(repr(x) for x in vecs[i]))
                        for i in range(s, min(s + 100, a.rows)))
        batches.append("INSERT INTO docs VALUES " + vals)
    t0 = time.perf_counter()
    for b in batches:
        send(b)
    t_write = time.perf_counter() - t0
    if a.index:
        send("CREATE VECTOR INDEX docs_vec ON docs(embedding)")
    qs = []
    for j in range(a.queries):
        q = gen()
        cat = "c%d" % rng.randrange(a.cats) if j % 2 else None
        qs.append((q, cat))
    sqls = []
    for q, cat in qs:
        where = " WHERE category = '%s'" % cat if cat else ""
        sqls.append("SELECT id, L2_DISTANCE(embedding, '[%s]') AS distance FROM docs%s ORDER BY distance LIMIT %d"
                    % (",".join(repr(x) for x in q), where, a.k))
    send(sqls[0])  # warm-up, outside the timed window
    t0 = time.perf_counter()
    results = [send(s) for s in sqls]
    t_query = time.perf_counter() - t0
    seq[0] += 1
    p.stdin.write(json.dumps({"id": seq[0], "op": "shutdown"}) + "\n")
    p.stdin.flush()
    assert json.loads(p.stdout.readline()) == {"id": seq[0], "ok": True}
    p.stdin.close()
    assert p.wait() == 0
    peak_kb = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    if os.uname().sysname == "Darwin":
        peak_kb //= 1024
    size = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))

    recall = {"filtered": [], "unfiltered": []}
    for (q, cat), r in zip(qs, results):
        cand = [(sum((x - y) ** 2 for x, y in zip(vecs[i], q)), i + 1)
                for i in range(a.rows) if cat is None or cats[i] == cat]
        cand.sort()
        truth = {i for _, i in cand[:a.k]}
        got = [row[0] for row in r["rows"]]
        assert len(got) == len(set(got)), "duplicate ids"
        if truth:
            recall["filtered" if cat else "unfiltered"].append(len(truth & set(got)) / min(a.k, len(cand)))
    print(json.dumps({
        "write_s": round(t_write, 3),
        "query_s": round(t_query, 3),
        "peak_rss_mb": round(peak_kb / 1024, 1),
        "data_mb": round(size / 2**20, 2),
        "recall_unfiltered": round(sum(recall["unfiltered"]) / max(1, len(recall["unfiltered"])), 4),
        "recall_filtered": round(sum(recall["filtered"]) / max(1, len(recall["filtered"])), 4),
    }))


if __name__ == "__main__":
    main()
