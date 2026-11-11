"""Tiny independent durable-root/reopen tests, not public fixtures."""
import json
import os
import struct
import subprocess
import sys

import pytest
from _minivectordb import Engine
from minivectordb.errors import DatabaseError
from minivectordb.frontend import parse_statement


def sql(engine, text):
    return engine.execute(parse_statement(text))


def test_multitable_reopen_encoding_and_gc(tmp_path):
    engine = Engine(str(tmp_path))
    sql(engine, 'CREATE TABLE a (s VARCHAR(8), id BIGINT PRIMARY KEY, v VECTOR(2))')
    sql(engine, 'CREATE TABLE b (id BIGINT PRIMARY KEY, n BIGINT)')
    sql(engine, "INSERT INTO a VALUES ('中文',9007199254740993,'[0.1,-2]')")
    sql(engine, 'INSERT INTO b VALUES (1,2),(3,4)')
    sql(engine, 'UPDATE b SET n=9 WHERE id=1')
    sql(engine, 'DELETE FROM b WHERE id=3')
    for n in range(3):
        sql(engine, f'UPDATE b SET n={n} WHERE id=1')
    engine.close()
    reopened = Engine(str(tmp_path))
    assert sql(reopened, 'SELECT * FROM a') == {
        'columns': ['s', 'id', 'v'],
        'rows': [['中文', 9007199254740993, [struct.unpack('f', struct.pack('f', .1))[0], -2.0]]]}
    assert sql(reopened, 'SELECT * FROM b')['rows'] == [[1, 2]]
    assert len(list((tmp_path/'objects').iterdir())) == 2
    assert len(list((tmp_path/'catalogs').iterdir())) == 1
    assert (tmp_path/'CURRENT').read_text().startswith('catalogs/')
    reopened.close()


def test_ack_is_durable_without_close(tmp_path):
    process = subprocess.Popen([sys.executable, '-m', 'minivectordb', '--data-dir', str(tmp_path)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=os.environ.copy())
    try:
        assert json.loads(process.stdout.readline()) == {'type': 'ready'}
        for ident, text in enumerate(['CREATE TABLE x (id BIGINT PRIMARY KEY)', 'INSERT INTO x VALUES (7)']):
            process.stdin.write(json.dumps({'id': ident, 'op': 'execute', 'sql': text})+'\n')
            process.stdin.flush()
            assert json.loads(process.stdout.readline())['ok'] is True
        process.kill()  # No EOF, shutdown, close or destructor can provide durability.
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdin.close()
        process.stdout.close()
        process.stderr.close()
    engine = Engine(str(tmp_path))
    assert sql(engine, 'SELECT * FROM x')['rows'] == [[7]]
    engine.close()


def test_recovery_current_only_missing_reference_and_cleanup(tmp_path):
    engine = Engine(str(tmp_path))
    sql(engine, 'CREATE TABLE x (id BIGINT PRIMARY KEY)')
    sql(engine, 'INSERT INTO x VALUES (1)')
    engine.close()
    orphan = tmp_path/'objects'/'orphan.tbl'
    orphan.write_bytes(b'unpublished')
    (tmp_path/'catalogs'/'zzzz.bin').write_bytes(b'not a root')
    (tmp_path/'staging'/'abandoned.tmp').write_bytes(b'incomplete')
    engine = Engine(str(tmp_path))
    assert sql(engine, 'SELECT * FROM x')['rows'] == [[1]]
    assert not orphan.exists()
    assert not (tmp_path/'catalogs'/'zzzz.bin').exists()
    assert not list((tmp_path/'staging').iterdir())
    engine.close()
    next((tmp_path/'objects').iterdir()).unlink()
    with pytest.raises(DatabaseError):
        Engine(str(tmp_path))
    # Failed startup must not emit ready or clean away evidence.
    result = subprocess.run([sys.executable, '-m', 'minivectordb', '--data-dir', str(tmp_path)],
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 1
    assert result.stdout == ''
    assert result.stderr
    assert (tmp_path/'CURRENT').exists()


def test_uncertain_publish_transport_exits_without_serving_next_request(monkeypatch, capsys):
    """Transport must not turn a fatal publication error into a recoverable SQL error."""
    import io
    import minivectordb.__main__ as transport

    calls = []

    class FailingEngine:
        def __init__(self, data_dir):
            pass

        def execute(self, statement, mode):
            calls.append(statement['op'])
            raise DatabaseError('fatal_storage_error', 'CURRENT directory fsync failed')

        def close(self):
            calls.append('close')

    monkeypatch.setattr(transport, 'Engine', FailingEngine)
    monkeypatch.setattr(sys, 'argv', ['minivectordb'])
    monkeypatch.setattr(sys, 'stdin', io.StringIO(
        '{"id":1,"op":"execute","sql":"CREATE TABLE x (id BIGINT PRIMARY KEY)"}\n'
        '{"id":2,"op":"execute","sql":"SELECT * FROM x"}\n'))
    assert transport.main() == 1
    captured = capsys.readouterr()
    assert captured.out == '{"type":"ready"}\n'
    assert 'fatal_storage_error' in captured.err
    assert calls == ['create_table', 'close']
