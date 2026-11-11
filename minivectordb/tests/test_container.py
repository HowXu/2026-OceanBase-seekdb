"""Container acceptance: Docker by default; explicitly select Podman when needed."""
import json
import os
import select
import shutil
import subprocess
import uuid

import pytest


@pytest.mark.parametrize('read_only', [False, True])
def test_container_jsonl_crud_shutdown(tmp_path, read_only):
    engine = os.environ.get('VECTORDB_CONTAINER_ENGINE', 'docker')
    assert shutil.which(engine), f'BLOCKED: actual {engine} executable is unavailable'
    image = os.environ.get('VECTORDB_IMAGE')
    assert image, 'BLOCKED: VECTORDB_IMAGE must name the built container image'
    assert subprocess.run([engine, 'info'], capture_output=True, timeout=30).returncode == 0, f'BLOCKED: {engine} unavailable'
    data = tmp_path / 'data'
    data.mkdir()
    name = 'minivectordb-task10-' + uuid.uuid4().hex
    command = [engine, 'run', '--rm', '--name', name, '-i', '--network=none',
               '--mount', f'type=bind,source={data},target=/data',
               '-e', 'MINIVECTORDB_TEST_STOP=before_current_replace']
    if read_only:
        command.append('--read-only')
        if engine == 'podman':
            command.append('--read-only-tmpfs=false')
    command.append(image)
    p = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    def read():
        assert select.select([p.stdout], [], [], 20)[0], 'container response timeout'
        return json.loads(p.stdout.readline())
    def sql(text):
        p.stdin.write(json.dumps({'id': 1, 'op': 'execute', 'sql': text}) + '\n')
        p.stdin.flush()
        result = read()
        assert result['id'] == 1 and result['ok'] is True, result
        return result
    try:
        assert read() == {'type': 'ready'}
        sql('CREATE TABLE x (id BIGINT PRIMARY KEY, n BIGINT)')
        assert sql('INSERT INTO x VALUES (1,8),(2,9)')['affected_rows'] == 2
        assert sql('UPDATE x SET n=10 WHERE id=1')['affected_rows'] == 1
        assert sql('SELECT * FROM x ORDER BY id')['rows'] == [[1, 10], [2, 9]]
        assert sql('DELETE FROM x WHERE id=2')['affected_rows'] == 1
        p.stdin.write('{"id":2,"op":"shutdown"}\n')
        p.stdin.flush()
        assert read() == {'id': 2, 'ok': True}
        assert p.wait(timeout=10) == 0
        assert p.stdout.read() == ''
        assert p.stderr.read() == ''
        assert (data / 'CURRENT').is_file()
        assert sorted(path.name for path in data.iterdir()) == ['CURRENT', 'catalogs', 'objects', 'staging']
    finally:
        subprocess.run([engine, 'rm', '-f', name], capture_output=True, timeout=15)
        if p.poll() is None:
            p.kill()
            p.wait(timeout=10)
        for stream in (p.stdin, p.stdout, p.stderr):
            stream.close()
