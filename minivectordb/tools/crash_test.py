#!/usr/bin/env python3
"""kill -9 recovery test against the real binary.

Each round sends multi-row autocommit INSERTs and explicit transactions
(some committed, some left open), kills the process with SIGKILL at a random
moment, restarts on the same directory and checks:
  * every acknowledged commit is present;
  * a statement or COMMIT without an acknowledgement is all-or-nothing;
  * an open transaction never leaks;
  * vector TopK agrees with a brute-force check of the visible rows.
Some rounds also kill the process again while it is recovering.
"""
import argparse
import json
import math
import os
import random
import signal
import subprocess
import tempfile
import threading
import time


def start(binary, d):
    p = subprocess.Popen([binary, "--data-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
    if json.loads(p.stdout.readline() or "null") != {"type": "ready"}:
        raise RuntimeError("no ready handshake")
    return p


_seq = [0]


def send(p, sql):
    _seq[0] += 1
    p.stdin.write(json.dumps({"id": _seq[0], "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n")
    p.stdin.flush()
    line = p.stdout.readline()
    if not line:
        return None
    r = json.loads(line)
    assert r.get("id") == _seq[0] and r.get("ok") is True, "unexpected response %r" % r
    return r


def vec(i):
    return [round(math.sin(i * 0.37) * 10, 3), round(math.cos(i * 0.11) * 10, 3), float(i % 7)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rounds", type=int, default=30)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    d = tempfile.mkdtemp(prefix="mvdb-crash-", dir=os.environ.get("MVDB_TMP"))
    p = start(a.bin, d)
    send(p, "CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), e VECTOR(3))")
    send(p, "CREATE VECTOR INDEX vi ON docs(e)")
    p.kill()
    p.wait()
    committed = {}       # id -> tag, acknowledged
    maybe = []           # list of dicts for unacknowledged statements/commits
    next_id = 1
    for rnd in range(a.rounds):
        p = start(a.bin, d)
        # Verify the previous round.
        got = send(p, "SELECT id, tag FROM docs")
        rows = {r[0]: r[1] for r in got["rows"]}
        base = dict(committed)
        for i, v in base.items():
            assert rows.get(i) == v, "round %d: lost committed id %d" % (rnd, i)
        extra = {i: v for i, v in rows.items() if i not in base}
        if maybe:
            m = maybe[0]
            if extra:
                assert extra == m, "round %d: partial unacked change %s vs %s" % (rnd, extra, m)
                committed.update(extra)
        else:
            assert not extra, "round %d: unexpected rows %s" % (rnd, extra)
        maybe = []
        q = [1.5, -2.0, 3.0]
        topk = send(p, "SELECT id, L2_DISTANCE(e, '%s') AS d FROM docs ORDER BY d LIMIT 5" % json.dumps(q))
        brute = sorted((math.dist(vec(i), q), i) for i in committed)[:5]
        assert len(topk["rows"]) == len(brute), "round %d: topk size" % rnd
        for r, b in zip(topk["rows"], brute):
            # Vectors are stored as float32, so distances match to ~1e-5.
            assert abs(r[1] - b[0]) < 1e-3, "round %d: topk mismatch %s vs %s" % (rnd, topk["rows"], brute)
            assert r[0] in committed

        # Run a workload and kill it at a random time.
        killer = threading.Timer(rng.uniform(0.005, 0.08), lambda: os.kill(p.pid, signal.SIGKILL))
        killer.start()
        try:
            while True:
                n = rng.randint(1, 20)
                ids = list(range(next_id, next_id + n))
                next_id += n
                tag = "r%d" % rnd
                change = {i: tag for i in ids}
                values = ",".join("(%d,'%s','%s')" % (i, tag, json.dumps(vec(i))) for i in ids)
                kind = rng.random()
                if kind < 0.5:
                    maybe = [change]
                    r = send(p, "INSERT INTO docs VALUES " + values)
                    if r is None:
                        break
                    committed.update(change)
                    maybe = []
                else:
                    if send(p, "BEGIN") is None:
                        break
                    if send(p, "INSERT INTO docs VALUES " + values) is None:
                        break
                    if kind < 0.8:
                        maybe = [change]
                        if send(p, "COMMIT") is None:
                            break
                        committed.update(change)
                        maybe = []
                    else:
                        if send(p, "ROLLBACK") is None:
                            break
        except (BrokenPipeError, ValueError):
            pass
        killer.cancel()
        p.kill()
        p.wait()
        if rnd % 3 == 0:
            # Kill during recovery as well.
            p2 = subprocess.Popen([a.bin, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(rng.uniform(0, 0.01))
            p2.kill()
            p2.wait()
    print("ok: %d rounds, %d committed rows" % (a.rounds, len(committed)))


if __name__ == "__main__":
    main()
