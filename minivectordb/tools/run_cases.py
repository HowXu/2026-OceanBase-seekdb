#!/usr/bin/env python3
"""Local runner for the public mysqltest-style cases.

Each case starts from an empty data directory. SQL lines are sent one at a
time on stdin and one JSON line is read back per statement. `--restart`
closes stdin, waits for a clean exit and restarts on the same directory.
`--mode ann` is forwarded to the program as a directive line.

This is not the official driver; it only mirrors the documented semantics.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile

TOL = 1e-5


def start(binary, data_dir, extra):
    p = subprocess.Popen([binary, "--data-dir", data_dir] + extra,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, encoding="utf-8")
    line = p.stdout.readline().strip()
    if line != "READY":
        raise RuntimeError("expected READY, got %r" % line)
    return p


def stop(p):
    p.stdin.close()
    rc = p.wait(timeout=60)
    err = p.stderr.read()
    if rc != 0:
        raise RuntimeError("exit code %d: %s" % (rc, err))


def same_value(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, int) and isinstance(b, int):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        if isinstance(b, int) and isinstance(a, int):
            return a == b
        d = abs(float(a) - float(b))
        return d <= TOL or d <= TOL * max(abs(float(a)), abs(float(b)))
    return a == b


def same_row(a, b):
    return len(a) == len(b) and all(same_value(x, y) for x, y in zip(a, b))


def check(expect, got):
    if "error" in expect:
        return "error" in got and (not expect["error"] or got["error"] == expect["error"])
    if "error" in got:
        return False
    if "affected_rows" in expect and got.get("affected_rows") != expect["affected_rows"]:
        return False
    if "columns" in expect and got.get("columns") != expect["columns"]:
        return False
    if "rows" in expect:
        rows = got.get("rows")
        if rows is None or len(rows) != len(expect["rows"]):
            return False
        if expect.get("unordered"):
            left = list(rows)
            for r in expect["rows"]:
                for i, g in enumerate(left):
                    if same_row(r, g):
                        del left[i]
                        break
                else:
                    return False
        elif not all(same_row(r, g) for r, g in zip(expect["rows"], rows)):
            return False
    return True


def run_case(binary, root, case, extra, verbose):
    lines = [l.rstrip("\n") for l in open(os.path.join(root, case["test"]), encoding="utf-8")]
    lines = [l for l in lines if l.strip()]
    expects = json.load(open(os.path.join(root, case["result"]), encoding="utf-8"))
    data = tempfile.mkdtemp(prefix="mvdb-" + case["id"] + "-",
                            dir=os.environ.get("MVDB_TMP"))
    p = start(binary, data, extra)
    k = 0
    ok = True
    try:
        for line in lines:
            s = line.strip()
            if s == "--restart":
                stop(p)
                p = start(binary, data, extra)
                continue
            if s.startswith("--"):
                p.stdin.write(s + "\n")
                p.stdin.flush()
                continue
            p.stdin.write(s + "\n")
            p.stdin.flush()
            out = p.stdout.readline()
            if not out:
                raise RuntimeError("process exited: " + p.stderr.read())
            got = json.loads(out)
            exp = expects[k]
            k += 1
            if not check(exp, got):
                ok = False
                print("  FAIL %s\n    expect %s\n    got    %s" % (s, json.dumps(exp, ensure_ascii=False),
                                                               json.dumps(got, ensure_ascii=False)))
            elif verbose:
                print("  ok   %s" % s)
        stop(p)
    except Exception as e:  # noqa: BLE001
        ok = False
        print("  ERROR %s" % e)
        p.kill()
    if k != len(expects):
        ok = False
        print("  statement count mismatch: ran %d, expected %d" % (k, len(expects)))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--cases", required=True, help="mysqltest directory containing manifest.json")
    ap.add_argument("--case", action="append")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("extra", nargs="*")
    a = ap.parse_args()
    manifest = json.load(open(os.path.join(a.cases, "manifest.json"), encoding="utf-8"))
    passed = failed = 0
    for c in manifest["cases"]:
        if a.case and c["id"] not in a.case:
            continue
        print(c["id"])
        if run_case(a.bin, a.cases, c, a.extra, a.v):
            passed += 1
        else:
            failed += 1
    print("passed %d, failed %d" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
