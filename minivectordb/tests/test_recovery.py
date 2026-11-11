"""Deterministic, tiny process-crash tests; no public fixtures or timing races."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[2]


class Session:
    def __init__(self, data, checkpoint=None, testing=True):
        env = os.environ.copy()
        if testing:
            extension = Path(env.get('MINIVECTORDB_FAULT_BUILD', ROOT / 'build/task10-fault/python'))
            assert list(extension.glob('_minivectordb*.so')), 'explicit fault-enabled build required'
            env['PYTHONPATH'] = str(extension) + ':' + str(ROOT / 'minivectordb/python')
        if checkpoint:
            env['MINIVECTORDB_TEST_STOP'] = checkpoint
        else:
            env.pop('MINIVECTORDB_TEST_STOP', None)
        self.p = subprocess.Popen([sys.executable, '-m', 'minivectordb', '--data-dir', str(data)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, env=env)

    def read(self):
        assert select.select([self.p.stdout], [], [], 5)[0], 'response timeout'
        line = self.p.stdout.readline()
        assert line, 'unexpected EOF'
        return json.loads(line)

    def ready(self):
        assert self.read() == {'type': 'ready'}

    def send(self, sql):
        self.p.stdin.write(json.dumps({'id': 1, 'op': 'execute', 'sql': sql}) + '\n')
        self.p.stdin.flush()

    def sql(self, sql):
        self.send(sql)
        result = self.read()
        assert result['ok'] is True, result
        return result

    def stopped(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            pid, status = os.waitpid(self.p.pid, os.WNOHANG | os.WUNTRACED)
            if pid:
                assert os.WIFSTOPPED(status), status
                assert os.WSTOPSIG(status) == signal.SIGSTOP
                return
            time.sleep(.01)
        pytest.fail('checkpoint did not stop process')

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait(timeout=5)
        for stream in (self.p.stdin, self.p.stdout, self.p.stderr):
            stream.close()


@pytest.fixture
def sessions():
    active = []
    def start(data, checkpoint=None, testing=True):
        s = Session(data, checkpoint, testing)
        active.append(s)
        return s
    yield start
    for s in active:
        s.kill()


def seed(start, data):
    s = start(data)
    s.ready()
    for table in ('a', 'b'):
        s.sql(f'CREATE TABLE {table} (id BIGINT PRIMARY KEY, n BIGINT, v VECTOR(2))')
        s.sql(f"INSERT INTO {table} VALUES (1,10,'[1,2]')")
    s.kill()


def state(s):
    return tuple(s.sql(f'SELECT id,n FROM {table}')['rows'][0][1] for table in ('a', 'b'))


@pytest.mark.parametrize('commit', ['auto', 'explicit', 'uncommitted'])
def test_sigkill_acknowledged_and_uncommitted(tmp_path, sessions, commit):
    seed(sessions, tmp_path)
    s = sessions(tmp_path)
    s.ready()
    if commit != 'auto':
        s.sql('BEGIN')
    for table in ('a', 'b'):
        s.sql(f'UPDATE {table} SET n=20')
    if commit == 'explicit':
        s.sql('COMMIT')
    assert state(s) == (20, 20)
    s.kill()
    recovered = sessions(tmp_path)
    recovered.ready()
    assert state(recovered) == ((10, 10) if commit == 'uncommitted' else (20, 20))
    assert recovered.sql("SELECT id,L2_DISTANCE(v,'[1,2]') AS d FROM a ORDER BY d LIMIT 1")['rows'] == [[1, 0.0]]


@pytest.mark.parametrize('checkpoint', ['objects_synced', 'catalog_synced',
                                        'before_current_replace', 'current_replaced'])
def test_kill_commit_boundary_never_mixes_tables(tmp_path, sessions, checkpoint):
    seed(sessions, tmp_path)
    s = sessions(tmp_path, checkpoint)
    s.ready()
    s.sql('BEGIN')
    for table in ('a', 'b'):
        s.sql(f'UPDATE {table} SET n=20')
    s.send('COMMIT')
    s.stopped()
    assert not select.select([s.p.stdout], [], [], 0)[0], 'commit was acknowledged'
    s.kill()
    recovered = sessions(tmp_path)
    recovered.ready()
    assert state(recovered) in ((10, 10), (20, 20))
    assert state(recovered) == ((20, 20) if checkpoint == 'current_replaced' else (10, 10))


def test_actual_cleanup_interrupted_then_reentered(tmp_path, sessions):
    seed(sessions, tmp_path)
    orphans = [tmp_path / 'objects' / f'orphan{i}.tbl' for i in range(3)]
    for path in orphans:
        path.write_bytes(b'uncommitted')
    s = sessions(tmp_path, 'after_garbage_remove')
    s.stopped()  # Before ready, but AFTER one actual unlink.
    assert sum(p.exists() for p in orphans) == 2
    assert not select.select([s.p.stdout], [], [], 0)[0]
    s.kill()
    recovered = sessions(tmp_path)
    recovered.ready()
    assert state(recovered) == (10, 10)
    assert all(not p.exists() for p in orphans)
    assert len(list((tmp_path / 'objects').iterdir())) == 2


def test_real_rename_failure_is_fatal_and_never_serves_next(tmp_path, sessions):
    seed(sessions, tmp_path)
    s = sessions(tmp_path, 'before_current_replace')
    s.ready()
    s.send('UPDATE a SET n=20')
    s.stopped()
    # Force the actual rename syscall to fail (file cannot replace a directory).
    saved = tmp_path / 'saved-current'
    (tmp_path / 'CURRENT').rename(saved)
    (tmp_path / 'CURRENT').mkdir()
    s.send('SELECT * FROM a')
    os.kill(s.p.pid, signal.SIGCONT)
    assert s.p.wait(timeout=5) == 1
    assert s.p.stdout.read() == ''
    assert 'fatal_storage_error' in s.p.stderr.read()
    (tmp_path / 'CURRENT').rmdir()
    saved.rename(tmp_path / 'CURRENT')
    recovered = sessions(tmp_path)
    recovered.ready()
    assert state(recovered) == (10, 10)


@pytest.mark.parametrize('missing', ['catalog', 'table'])
def test_missing_committed_reference_never_ready(tmp_path, sessions, missing):
    seed(sessions, tmp_path)
    path = (tmp_path / (tmp_path / 'CURRENT').read_text().strip() if missing == 'catalog'
            else next((tmp_path / 'objects').iterdir()))
    path.unlink()
    s = sessions(tmp_path)
    assert s.p.wait(timeout=5) == 1
    assert s.p.stdout.read() == ''
    assert s.p.stderr.read()


def test_production_ignores_fault_environment(tmp_path, sessions):
    s = sessions(tmp_path, 'before_current_replace', testing=False)
    s.ready()
    s.sql('CREATE TABLE x (id BIGINT PRIMARY KEY)')
    s.sql('INSERT INTO x VALUES (7)')
    assert s.sql('SELECT * FROM x')['rows'] == [[7]]
