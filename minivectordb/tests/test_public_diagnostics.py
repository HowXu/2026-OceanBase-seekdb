"""Independent miniature packages; never consume public SQL or answer fixtures."""
import json
from pathlib import Path
import subprocess
import sys

import pytest


RUNNER = Path(__file__).resolve().parents[1] / "tools" / "run_public.py"


@pytest.fixture
def miniature(tmp_path):
    package = tmp_path / "package"
    package.mkdir()
    cases = []
    for name in ("alpha", "beta"):
        (package / f"{name}.test").write_text(
            "# generated miniature\nSELECT 11;\n--restart\n--mode ann\n"
            "SELECT 22;\n--mode exact\nSELECT 33;\n"
        )
        (package / f"{name}.result").write_text(json.dumps([{}, {}, {}]))
        cases.append(dict(id=name, test=f"{name}.test", result=f"{name}.result"))
    (package / "manifest.json").write_text(json.dumps(dict(cases=cases)))
    config = tmp_path / "config.json"
    config.write_text(json.dumps(dict(startup_timeout=2, request_timeout=0.2,
        case_timeout=5, max_response_bytes=65536, abs_tol=1e-5, rel_tol=1e-5)))
    server = tmp_path / "server.py"
    server.write_text('''import json, pathlib, sys, time
kind, directory = sys.argv[1:]
root = pathlib.Path(directory)
marker = root / "count"
existing = marker.exists()
count = int(marker.read_text()) if existing else 0
if kind == "startup":
    print('{"wrong":"ready"}', flush=True)
else:
    print('{"type":"ready"}', flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if request["op"] == "shutdown":
        ok = kind != "shutdown" and not (kind == "final_shutdown" and count == 3)
        print(json.dumps(dict(id=request["id"], ok=ok)), flush=True)
        break
    if kind == "timeout":
        time.sleep(10)
    if kind == "crash":
        sys.exit(3)
    if kind == "json":
        print('{"id":1,"id":1,"ok":true}', flush=True)
        continue
    count += 1
    marker.write_text(str(count))
    response = dict(id=request["id"], ok=kind != "fail", affected_rows=count,
        data_dir=directory, existing=existing, mode=request["search_mode"], sql=request["sql"])
    if kind == "fail":
        response["error"] = dict(code="not_implemented", message="miniature scaffold")
    if kind == "id":
        response["id"] = -1
    print(json.dumps(response), flush=True)
''')
    return package, config, server


def run(miniature, tmp_path, kind="fail", *options):
    package, config, server = miniature
    output = tmp_path / ("report-" + str(len(list(tmp_path.glob("report-*")))))
    command = [sys.executable, str(RUNNER), "--command",
        json.dumps([sys.executable, str(server), kind, "{data_dir}"]),
        "--package", str(package), "--config", str(config), "--output", str(output), *options]
    result = subprocess.run(command, text=True, capture_output=True, timeout=15)
    assert (output / "result.json").exists(), result.stderr
    return result, json.loads((output / "result.json").read_text())


def records(report):
    return [statement for case in report["cases"] for statement in case["statements"]]


def test_keep_going_counts_real_six_attempts_and_expected_errors(miniature, tmp_path):
    result, report = run(miniature, tmp_path, "fail", "--keep-going")
    assert result.returncode == 1
    assert report["passed"] is False
    assert report["statistics"] == dict(attempted=6, transport_ok=6,
        sql_passed=0, sql_failed=6, not_run=0)
    assert [r["category"] for r in records(report)] == ["assertion"] * 6
    assert all(r["response"]["error"]["code"] == "not_implemented" for r in records(report))
    assert all(r["expected"] == {} and r["message"] for r in records(report))

    # Preserve the original permissive unmapped-error assertion, not our own comparator.
    package = miniature[0]
    (package / "alpha.result").write_text(json.dumps([{}, {"error": "some_error"}, {}]))
    result, report = run(miniature, tmp_path, "fail", "--keep-going")
    assert result.returncode == 1
    assert report["statistics"] == dict(attempted=6, transport_ok=6,
        sql_passed=1, sql_failed=5, not_run=0)
    assert records(report)[1]["status"] == "PASS"

    result, report = run(miniature, tmp_path, "fail")
    assert result.returncode == 1
    assert report["statistics"] == dict(attempted=2, transport_ok=2,
        sql_passed=0, sql_failed=2, not_run=4)
    result, report = run(miniature, tmp_path, "fail", "--keep-going", "--case", "beta")
    assert result.returncode == 1
    assert [c["id"] for c in report["cases"]] == ["beta"]
    assert report["statistics"]["attempted"] == 3


def test_restart_modes_alignment_and_isolated_directories(miniature, tmp_path):
    package = miniature[0]
    for name in ("alpha", "beta"):
        (package / f"{name}.result").write_text(json.dumps(
            [{"affected_rows": n} for n in (1, 2, 3)]))
    result, report = run(miniature, tmp_path, "success", "--keep-going")
    assert result.returncode == 0
    assert report["passed"] is True
    assert report["statistics"] == dict(attempted=6, transport_ok=6,
        sql_passed=6, sql_failed=0, not_run=0)
    directories = []
    for case in report["cases"]:
        rows = case["statements"]
        assert [r["line"] for r in rows] == [2, 5, 7]
        assert [r["index"] for r in rows] == [0, 1, 2]
        assert [r["mode"] for r in rows] == ["exact", "ann", "exact"]
        assert [r["response"]["mode"] for r in rows] == ["exact", "ann", "exact"]
        assert [r["response"]["sql"] for r in rows] == [r["sql"] for r in rows]
        assert [r["response"]["existing"] for r in rows] == [False, True, True]
        paths = {r["response"]["data_dir"] for r in rows}
        assert len(paths) == 1
        directories.extend(paths)
    assert directories[0] != directories[1]
    assert all(not Path(path).exists() for path in directories)


@pytest.mark.parametrize("kind", ["timeout", "id", "crash", "json", "startup", "shutdown", "final_shutdown"])
def test_transport_failures_and_not_run_are_honest(miniature, tmp_path, kind):
    result, report = run(miniature, tmp_path, kind, "--keep-going")
    assert result.returncode == 1
    assert report["passed"] is False
    stats = report["statistics"]
    rows = records(report)
    if kind == "startup":
        assert stats == dict(attempted=0, transport_ok=0, sql_passed=0, sql_failed=0, not_run=6)
    elif kind == "final_shutdown":
        assert stats == dict(attempted=6, transport_ok=6, sql_passed=6, sql_failed=0, not_run=0)
        assert all(case["status"] == "FAIL" and case["errors"][0]["phase"] == "shutdown"
            for case in report["cases"])
    elif kind == "shutdown":  # restart shutdown fails after the first successful SQL
        assert stats == dict(attempted=2, transport_ok=2, sql_passed=2, sql_failed=0, not_run=4)
    else:
        attempts = 1 if kind == "timeout" else 2
        assert stats == dict(attempted=attempts, transport_ok=0, sql_passed=0,
            sql_failed=attempts, not_run=6-attempts)
        failed = [r for r in rows if r["status"] == "FAIL"]
        assert all(r["response"] is None and r["category"] == "transport" for r in failed)
    assert all(r["response"] is None and not r["attempted"] and r["message"]
        for r in rows if r["status"] == "NOT_RUN")
    assert any(case["errors"] for case in report["cases"])
    if kind == "timeout":
        assert report["cases"][1]["status"] == "NOT_RUN"
        assert all("timeout" in r["message"] for r in report["cases"][1]["statements"])
