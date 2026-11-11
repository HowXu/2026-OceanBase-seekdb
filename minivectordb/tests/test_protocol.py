"""Independent short JSONL sessions; no public case/answer imports."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def start(tmp_path):
    env = dict(os.environ)
    env.setdefault("PYTHONPATH", f"{ROOT}/build/python:{ROOT}/minivectordb/python")
    return subprocess.Popen([sys.executable, "-m", "minivectordb", "--data-dir", str(tmp_path)],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env)


def receive(p):
    with selectors.DefaultSelector() as selector:
        selector.register(p.stdout, selectors.EVENT_READ)
        assert selector.select(5), "response not flushed within 5 seconds"
    line = p.stdout.readline()
    assert line, f"process exited: {p.stderr.read()}"
    return json.loads(line)


def send(p, request):
    p.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
    p.stdin.flush()
    return receive(p)


def test_ready_shutdown_and_eof(tmp_path):
    for shutdown in (True, False):
        p = start(tmp_path)
        try:
            assert receive(p) == {"type": "ready"}
            if shutdown:
                assert send(p, {"id": "bye", "op": "shutdown"}) == {"id": "bye", "ok": True}
            p.stdin.close()
            assert p.wait(timeout=5) == 0
            assert p.stdout.read() == ""
            assert p.stderr.read() == ""
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()


def test_ids_flush_and_continuous_execute_errors(tmp_path):
    p = start(tmp_path)
    try:
        assert receive(p) == {"type": "ready"}
        for ident in (9007199254740993, "中文", None, True, [1, "x"], {"key": 2}):
            response = send(p, {"id": ident, "op": "execute", "sql": "SELECT id FROM docs;"})
            assert response["id"] == ident and type(response["id"]) is type(ident)
            assert response["ok"] is False
            assert response["error"]["code"] == "unknown_table"
        assert send(p, {"id": 2, "op": "shutdown"}) == {"id": 2, "ok": True}
        assert p.wait(timeout=5) == 0
        assert p.stdout.read() == ""
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_strict_json_and_request_errors_keep_session(tmp_path):
    p = start(tmp_path)
    try:
        assert receive(p) == {"type": "ready"}
        for raw in ('{"id":1,"id":2,"op":"shutdown"}', '{"id":NaN}', '{"id":1e999}', '[]', '{',
                    '{"id":1,"op":"execute","sql":3}',
                    '{"id":2,"op":"execute","sql":"BEGIN","search_mode":"bad"}'):
            p.stdin.write(raw + "\n")
            p.stdin.flush()
            response = receive(p)
            assert response["ok"] is False
            assert response["error"]["code"] == "invalid_request"
        assert send(p, {"id": 3, "op": "shutdown"})["ok"] is True
        assert p.wait(timeout=5) == 0
        assert p.stdout.read() == ""
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_escaped_surrogates_keep_session(tmp_path):
    p = start(tmp_path)
    try:
        assert receive(p) == {"type": "ready"}
        p.stdin.write('{"id":"\\ud800","op":"execute","sql":"SELECT 1 FROM docs"}\n')
        p.stdin.flush()
        response = receive(p)
        assert response["id"] == "\ud800"
        assert response["ok"] is False
        assert response["error"]["code"] == "unknown_table"
        # Duplicate-key diagnostics can contain the same accepted JSON character.
        p.stdin.write('{"id":2,"\\udfff":1,"\\udfff":2}\n')
        p.stdin.flush()
        response = receive(p)
        assert response["ok"] is False
        assert response["error"] == {"code": "invalid_request",
                                     "message": "duplicate JSON key: \udfff"}
        response = send(p, {"id": "next", "op": "execute", "sql": "SELECT 2 FROM docs"})
        assert response["id"] == "next"
        assert response["error"]["code"] == "unknown_table"
        assert send(p, {"id": 3, "op": "shutdown"}) == {"id": 3, "ok": True}
        assert p.wait(timeout=5) == 0
        assert p.stdout.read() == ""
        assert p.stderr.read() == ""
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_start_preserves_caller_pythonpath(tmp_path, monkeypatch):
    configured_path = f"{tmp_path}/alternate-build/python:{ROOT}/minivectordb/python"
    monkeypatch.setenv("PYTHONPATH", configured_path)
    sentinel = object()

    def capture_popen(*args, **kwargs):
        assert kwargs["env"]["PYTHONPATH"] == configured_path
        return sentinel

    monkeypatch.setattr(subprocess, "Popen", capture_popen)
    assert start(tmp_path) is sentinel


def test_sql_frontend_errors_keep_session(tmp_path):
    p = start(tmp_path)
    try:
        assert receive(p) == {"type": "ready"}
        for sql in ("ALTER TABLE local_notes ADD x BIGINT", "BEGIN; COMMIT",
                    "SELECT unknown_function(id) FROM local_notes",
                    "CREATE VECTOR INDEX i ON local_notes(v) trailing",
                    "SELECT '\ud800' FROM local_notes"):
            # JSON's escaped code unit must not reach native UTF-8 conversion.
            p.stdin.write(json.dumps({"id": "bad", "op": "execute", "sql": sql}) + "\n")
            p.stdin.flush()
            response = receive(p)
            assert response["id"] == "bad"
            assert response["ok"] is False
            assert response["error"]["code"] == "unsupported_sql"
        response = send(p, {"id": "next", "op": "execute", "sql": "BEGIN", "search_mode": "ann"})
        assert response["id"] == "next"
        assert response == {"id": "next", "ok": True}
        assert send(p, {"id": "bye", "op": "shutdown"}) == {"id": "bye", "ok": True}
        assert p.wait(timeout=5) == 0
        assert p.stdout.read() == ""
        assert p.stderr.read() == ""
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
