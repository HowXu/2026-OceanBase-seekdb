"""Independent tiny transaction/root-visibility and statement-atomicity sequences."""
import json
import os
import subprocess
import sys

import pytest
from _minivectordb import Engine
from minivectordb.errors import DatabaseError
from minivectordb.frontend import parse_statement


def sql(engine, text, mode='exact'):
    return engine.execute(parse_statement(text), mode)


def setup(engine):
    sql(engine, 'CREATE TABLE docs (id BIGINT PRIMARY KEY, n BIGINT, v VECTOR(2))')
    sql(engine, "INSERT INTO docs VALUES (1,10,'[3,4]')")


def visible(engine, n=10, distance=5.0, mode='exact'):
    assert sql(engine, 'SELECT id,n FROM docs')['rows'] == [[1, n]]
    assert sql(engine, "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM docs "
               "ORDER BY d LIMIT 2", mode)['rows'] == [[1, distance]]


def test_read_own_writes_multitable_commit_and_reopen(tmp_path):
    engine = Engine(str(tmp_path))
    setup(engine)
    before = (tmp_path/'CURRENT').read_bytes()
    assert sql(engine, 'BEGIN') == {}
    sql(engine, 'CREATE TABLE other (id BIGINT PRIMARY KEY)')
    sql(engine, 'INSERT INTO other VALUES (7)')
    sql(engine, "UPDATE docs SET n=20,v='[0,2]' WHERE id=1")
    visible(engine, 20, 2.0)
    visible(engine, 20, 2.0, 'ann')
    assert sql(engine, 'SELECT * FROM other')['rows'] == [[7]]
    assert (tmp_path/'CURRENT').read_bytes() == before
    assert sql(engine, 'COMMIT') == {}
    assert (tmp_path/'CURRENT').read_bytes() != before
    engine.close()
    reopened = Engine(str(tmp_path))
    visible(reopened, 20, 2.0)
    assert sql(reopened, 'SELECT * FROM other')['rows'] == [[7]]
    reopened.close()


def test_rollback_scalar_vector_and_transaction_boundaries(tmp_path):
    engine = Engine(str(tmp_path))
    setup(engine)
    before = (tmp_path/'CURRENT').read_bytes()
    for op in ('COMMIT', 'ROLLBACK'):
        assert sql(engine, op) == {}
    sql(engine, 'BEGIN')
    sql(engine, "UPDATE docs SET n=99,v='[0,1]'")
    with pytest.raises(DatabaseError) as caught:
        sql(engine, 'BEGIN')
    assert caught.value.code == 'transaction_active'
    visible(engine, 99, 1.0)
    sql(engine, 'CREATE TABLE transient (id BIGINT PRIMARY KEY)')
    assert sql(engine, 'ROLLBACK') == {}
    visible(engine)
    visible(engine, mode='ann')
    with pytest.raises(DatabaseError) as caught:
        sql(engine, 'SELECT * FROM transient')
    assert caught.value.code == 'unknown_table'
    assert (tmp_path/'CURRENT').read_bytes() == before
    assert len(list((tmp_path/'objects').iterdir())) == 1
    sql(engine, 'BEGIN')
    sql(engine, 'COMMIT')
    engine.close()


def test_failed_statements_discard_only_candidate(tmp_path):
    engine = Engine(str(tmp_path))
    setup(engine)
    sql(engine, 'BEGIN')
    sql(engine, 'UPDATE docs SET n=11')
    objects = set((tmp_path/'objects').iterdir())
    with pytest.raises(DatabaseError) as caught:
        sql(engine, "INSERT INTO docs VALUES (2,20,'[0,0]'),(1,30,'[1,1]')")
    assert caught.value.code == 'duplicate_key'
    assert set((tmp_path/'objects').iterdir()) == objects
    visible(engine, 11)
    sql(engine, 'COMMIT')
    with pytest.raises(DatabaseError) as caught:
        sql(engine, "INSERT INTO docs VALUES (2,20,'[0,0]'),(3,30,'[1]')")
    assert caught.value.code == 'type_error'
    visible(engine, 11)
    engine.close()
    reopened = Engine(str(tmp_path))
    visible(reopened, 11)
    assert len(list((tmp_path/'objects').iterdir())) == 1
    reopened.close()


@pytest.mark.parametrize('ending', ['close', 'shutdown', 'eof'])
def test_close_shutdown_eof_rollback(tmp_path, ending):
    if ending == 'close':
        engine = Engine(str(tmp_path))
        setup(engine)
        sql(engine, 'BEGIN')
        sql(engine, "UPDATE docs SET n=88,v='[0,1]'")
        engine.close()
        engine.close()
        with pytest.raises(DatabaseError) as caught:
            sql(engine, 'COMMIT')
        assert caught.value.code == 'engine_closed'
    else:
        statements = ['CREATE TABLE docs (id BIGINT PRIMARY KEY, n BIGINT, v VECTOR(2))',
                      "INSERT INTO docs VALUES (1,10,'[3,4]')", 'BEGIN',
                      "UPDATE docs SET n=88,v='[0,1]'", 'SELECT n FROM docs']
        requests = [{'id': i, 'op': 'execute', 'sql': text} for i, text in enumerate(statements)]
        if ending == 'shutdown':
            requests.append({'id': 5, 'op': 'shutdown'})
        result = subprocess.run([sys.executable, '-m', 'minivectordb', '--data-dir', str(tmp_path)],
                                input=''.join(json.dumps(r)+'\n' for r in requests),
                                capture_output=True, text=True, timeout=5, env=os.environ.copy())
        assert result.returncode == 0 and result.stderr == ''
        responses = [json.loads(line) for line in result.stdout.splitlines()]
        assert responses[0] == {'type': 'ready'}
        assert all(r['ok'] is True for r in responses[1:])
        assert responses[5]['rows'] == [[88]]
        assert len(responses) == len(requests)+1
        if ending == 'shutdown':
            assert responses[-1] == {'id': 5, 'ok': True}
    # Cleanup must already have occurred, not merely on the next recovery.
    assert len(list((tmp_path/'objects').iterdir())) == 1
    assert len(list((tmp_path/'catalogs').iterdir())) == 1
    assert not list((tmp_path/'staging').iterdir())
    reopened = Engine(str(tmp_path))
    visible(reopened)
    reopened.close()


def test_commit_ack_survives_kill_without_close(tmp_path):
    process = subprocess.Popen([sys.executable, '-m', 'minivectordb', '--data-dir', str(tmp_path)],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=os.environ.copy())
    try:
        assert json.loads(process.stdout.readline()) == {'type': 'ready'}
        statements = ['BEGIN', 'CREATE TABLE docs (id BIGINT PRIMARY KEY, n BIGINT, v VECTOR(2))',
                      "INSERT INTO docs VALUES (1,10,'[3,4]')", 'COMMIT']
        for ident, text in enumerate(statements):
            process.stdin.write(json.dumps({'id': ident, 'op': 'execute', 'sql': text})+'\n')
            process.stdin.flush()
            assert json.loads(process.stdout.readline())['ok'] is True
        process.kill()
        process.wait(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdin.close()
        process.stdout.close()
        process.stderr.close()
    engine = Engine(str(tmp_path))
    visible(engine)
    engine.close()
