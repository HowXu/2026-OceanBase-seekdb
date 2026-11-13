"""Reproducible local MiniVectorDB benchmark; not an official score runner."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import statistics
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'protocol_check'))
from _transport import Session

CONFIG = json.loads((ROOT / 'protocol_check/config.json').read_text())

SEED = 20261112
ROWS = 10_000
DIMENSION = 128
BATCH_ROWS = 100
QUERY_COUNT = 1_000
K = 10


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def build_dataset(seed=SEED):
    data_random = random.Random(seed)
    records = []
    for ident in range(ROWS):
        payload = (f'row-{ident}-' + 'x' * 64)[:64]
        vector = [f32(data_random.uniform(-1, 1)) for _ in range(DIMENSION)]
        records.append((ident, ident % 10, payload, vector))
    query_random = random.Random(seed + 1)
    queries = []
    for ident in range(QUERY_COUNT):
        group = ident % 10 if ident % 2 == 0 else None
        vector = [f32(query_random.uniform(-1, 1)) for _ in range(DIMENSION)]
        queries.append((ident, group, vector))
    return records, queries


def write_oracle_input(path, records, queries, k=K):
    dimension = len(records[0][3]) if records else (len(queries[0][2]) if queries else 0)
    if dimension <= 0:
        raise ValueError('positive vector dimension required')
    with Path(path).open('wb') as output:
        output.write(struct.pack('<9sQQQQ', b'MVDBPERF1', len(records), len(queries), dimension, k))
        for ident, group, payload, vector in records:
            if len(vector) != dimension:
                raise ValueError('record dimension mismatch')
            encoded = payload.encode('utf-8')
            output.write(struct.pack('<qqQ', ident, group, len(encoded)))
            output.write(encoded)
            output.write(struct.pack(f'<{dimension}f', *vector))
        for ident, group, vector in queries:
            if len(vector) != dimension:
                raise ValueError('query dimension mismatch')
            output.write(struct.pack('<qq', ident, -1 if group is None else group))
            output.write(struct.pack(f'<{dimension}f', *vector))


def run_oracle(executable, input_path):
    result = subprocess.run([str(executable), str(input_path)], check=True,
                            capture_output=True, text=True)
    payload = json.loads(result.stdout)
    return [entry['rows'] for entry in payload]


def distribution(values):
    ordered = sorted(float(value) for value in values)
    if not ordered:
        return {'count': 0, 'sum': 0.0, 'min': None,
                'p50': None, 'p95': None, 'p99': None, 'max': None}
    def percentile(fraction):
        index = math.ceil(fraction * len(ordered)) - 1
        return ordered[index]
    return {'count': len(ordered), 'sum': math.fsum(ordered), 'min': ordered[0],
            'p50': percentile(.50), 'p95': percentile(.95),
            'p99': percentile(.99), 'max': ordered[-1]}


def recall_at_k(actual_ids, expected_ids, k=K):
    expected = expected_ids[:k]
    if not expected:
        return None
    return len(set(actual_ids[:k]) & set(expected)) / len(expected)


def exact_topk_matches(actual, expected):
    if len(actual) != len(expected):
        return False
    for actual_row, expected_row in zip(actual, expected):
        if actual_row[0] != expected_row[0]:
            return False
        if not isinstance(actual_row[1], (int, float)) or not math.isfinite(actual_row[1]):
            return False
        if not math.isclose(actual_row[1], expected_row[1], rel_tol=1e-5, abs_tol=1e-5):
            return False
    return True


def io_bytes(before, after):
    result = {}
    for device, values in after.items():
        old = before.get(device, {})
        read = values.get('rbytes', 0) - old.get('rbytes', 0)
        written = values.get('wbytes', 0) - old.get('wbytes', 0)
        if read < 0 or written < 0:
            raise ValueError('cgroup IO counters moved backwards')
        result[device] = {'rbytes': read, 'wbytes': written, 'total': read + written}
    return result


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def cgroup_metrics(session):
    info = json.loads(subprocess.check_output(['docker', 'inspect', session.name], text=True))[0]
    pid = info['State']['Pid']
    group = Path(f'/proc/{pid}/cgroup').read_text().split('0::', 1)[1].strip()
    root = Path('/sys/fs/cgroup') / group.lstrip('/')
    result = {'cgroup': str(root)}
    for name in ['memory.max', 'memory.swap.max', 'memory.peak', 'cpu.max', 'pids.max']:
        result[name] = (root / name).read_text().strip()
    for name in ['memory.max', 'memory.swap.max', 'memory.peak']:
        result[name] = int(result[name])
    result['memory.events'] = dict(line.split() for line in (root / 'memory.events').read_text().splitlines())
    io = {}
    for line in (root / 'io.stat').read_text().splitlines():
        device, *items = line.split()
        io[device] = {key: int(value) for key, value in (item.split('=') for item in items)}
    result['io.stat'] = io
    return result


def synchronize_filesystem(session):
    subprocess.run(['docker', 'exec', session.name, '/opt/venv/bin/python', '-c',
                    'import os; os.sync()'], check=True, capture_output=True, timeout=60)


def create_schema(session):
    response = session.request(
        'CREATE TABLE items (id BIGINT PRIMARY KEY,grp BIGINT,payload VARCHAR(64),v VECTOR(128))')
    require(response['ok'] is True, response)


def vector_text(vector):
    return '[' + ','.join(repr(value) for value in vector) + ']'


def insert_batch(session, batch):
    values = ','.join(
        f"({ident},{grp},'{payload}','{vector_text(vector)}')"
        for ident, grp, payload, vector in batch)
    response = session.request('INSERT INTO items VALUES ' + values)
    require(response.get('ok') is True and response.get('affected_rows') == len(batch), response)


def query_sql(group, vector):
    predicate = '' if group is None else f' WHERE grp={group}'
    return ("SELECT id,L2_DISTANCE(v,'" + vector_text(vector) +
            "') AS distance FROM items" + predicate + ' ORDER BY distance LIMIT 10')


def response_rows(response):
    require(response.get('ok') is True and response.get('columns') == ['id', 'distance'], response)
    rows = response.get('rows')
    require(isinstance(rows, list), response)
    return rows


def start_measured_container(session):
    started = time.perf_counter_ns()
    session.start(ready=False)
    deadline = time.monotonic() + CONFIG['startup_timeout']
    while True:
        inspected = subprocess.run(['docker', 'inspect', session.name],
                                   capture_output=True, text=True)
        if inspected.returncode == 0 and json.loads(inspected.stdout)[0]['State']['Pid'] > 0:
            break
        require(session.proc.poll() is None, 'container exited before cgroup observation')
        require(time.monotonic() < deadline, 'container/cgroup observation timeout')
        time.sleep(.005)
    initial = cgroup_metrics(session)
    ready = session.read(CONFIG['startup_timeout'])
    require(ready == {'type': 'ready'}, ready)
    return (time.perf_counter_ns() - started) / 1e9, initial


def finish_metrics(session, initial):
    started = time.perf_counter_ns()
    synchronize_filesystem(session)
    elapsed = (time.perf_counter_ns() - started) / 1e9
    final = cgroup_metrics(session)
    events = final['memory.events']
    require(events.get('oom', '0') == '0' and events.get('oom_kill', '0') == '0', events)
    return elapsed, final


def validate_written_records(session, records):
    session.crash()
    session.start()
    checked = 0
    for first in range(0, len(records), 1000):
        expected = records[first:first + 1000]
        last = expected[-1][0]
        response = session.request(
            f'SELECT id,grp,payload,v FROM items WHERE id>={first} AND id<={last} ORDER BY id')
        require(response.get('ok') is True and response.get('columns') == ['id', 'grp', 'payload', 'v'], response)
        actual = response['rows']
        require(len(actual) == len(expected), (first, len(actual), len(expected)))
        for row, record in zip(actual, expected):
            ident, group, payload, vector = record
            require(row[:3] == [ident, group, payload], (row[:3], record[:3]))
            require(len(row[3]) == len(vector), ident)
            for actual_value, expected_value in zip(row[3], vector):
                require(struct.pack('<f', actual_value) == struct.pack('<f', expected_value), ident)
        checked += len(actual)
    require(checked == len(records), checked)
    return {'status': 'PASS', 'rows': checked, 'vector_components': checked * DIMENSION,
            'restart_recovery': True}


def run_write(round_index, image, records, out):
    out.mkdir(parents=True)
    result = {'project': 'write', 'round': round_index, 'image': image,
              'seed': SEED, 'rows': len(records), 'batch_rows': BATCH_ROWS,
              'status': 'FAIL', 'cleaned': False}
    session = Session(CONFIG, out / 'stderr.log', image=image)
    try:
        startup, initial = start_measured_container(session)
        started = time.perf_counter_ns()
        create_schema(session)
        schema_seconds = (time.perf_counter_ns() - started) / 1e9
        batches = []
        for first in range(0, len(records), BATCH_ROWS):
            started = time.perf_counter_ns()
            insert_batch(session, records[first:first + BATCH_ROWS])
            batches.append({'batch': first // BATCH_ROWS,
                            'elapsed_ns': time.perf_counter_ns() - started})
        require(len(batches) == 100, len(batches))
        sync_seconds, final = finish_metrics(session, initial)
        io = io_bytes(initial['io.stat'], final['io.stat'])
        result.update(
            stages={'startup_seconds': startup, 'schema_seconds': schema_seconds,
                    'sync_seconds': sync_seconds},
            batches=batches,
            batch_seconds=distribution([row['elapsed_ns'] / 1e9 for row in batches]),
            T_seconds=math.fsum(row['elapsed_ns'] for row in batches) / 1e9,
            B_bytes=max((row['total'] for row in io.values()), default=0),
            M_bytes=final['memory.peak'], io_by_device=io,
            cgroup_initial=initial, cgroup_final=final)
        result['correctness'] = validate_written_records(session, records)
        session.shutdown()
        result['status'] = 'PASS'
    except Exception as error:
        result['error'] = repr(error)
    finally:
        session.close()
        result['cleaned'] = True
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    return result


def validate_query_responses(records, queries, truth, responses):
    groups = {record[0]: record[1] for record in records}
    recalls = {'filtered': [], 'unfiltered': []}
    checked = 0
    for query, expected, response in zip(queries, truth, responses):
        _, group, _ = query
        rows = response_rows(response)
        require(exact_topk_matches(rows, expected), (query[0], rows, expected))
        ids = [row[0] for row in rows]
        require(len(ids) == len(set(ids)), (query[0], 'duplicate IDs'))
        require(all(group is None or groups[ident] == group for ident in ids), (query[0], 'filter'))
        recall = recall_at_k(ids, [row[0] for row in expected])
        if recall is not None:
            recalls['filtered' if group is not None else 'unfiltered'].append(recall)
        checked += 1
    require(checked == len(queries) == 1000, checked)
    means = {name: math.fsum(values) / len(values) for name, values in recalls.items()}
    require(all(value >= .95 for value in means.values()), means)
    return {'status': 'PASS', 'queries': checked, 'recall_at_10': means,
            'exact_id_and_distance_matches': True}


def run_query(round_index, image, records, queries, truth, out):
    out.mkdir(parents=True)
    result = {'project': 'query', 'round': round_index, 'image': image,
              'seed': SEED, 'rows': len(records), 'queries': len(queries),
              'status': 'FAIL', 'cleaned': False}
    session = Session(CONFIG, out / 'stderr.log', image=image)
    responses_path = out / 'responses.json'
    try:
        startup, initial = start_measured_container(session)
        started = time.perf_counter_ns()
        create_schema(session)
        schema_seconds = (time.perf_counter_ns() - started) / 1e9
        load = []
        for first in range(0, len(records), BATCH_ROWS):
            started = time.perf_counter_ns()
            insert_batch(session, records[first:first + BATCH_ROWS])
            load.append(time.perf_counter_ns() - started)
        started = time.perf_counter_ns()
        response = session.request('CREATE VECTOR INDEX vi ON items(v)')
        require(response['ok'] is True, response)
        index_seconds = (time.perf_counter_ns() - started) / 1e9
        warmup_random = random.Random(SEED + 2)
        warmup = []
        for index in range(20):
            vector = [f32(warmup_random.uniform(-1, 1)) for _ in range(DIMENSION)]
            group = index % 10 if index % 2 == 0 else None
            started = time.perf_counter_ns()
            rows = response_rows(session.request(query_sql(group, vector)))
            require(len(rows) == K, len(rows))
            warmup.append(time.perf_counter_ns() - started)
        measurements = []
        responses = []
        for ident, group, vector in queries:
            started = time.perf_counter_ns()
            response = session.request(query_sql(group, vector))
            elapsed = time.perf_counter_ns() - started
            measurements.append({'query': ident, 'filtered': group is not None,
                                 'elapsed_ns': elapsed})
            responses.append(response)
        require(len(measurements) == 1000, len(measurements))
        responses_path.write_text(json.dumps(responses, ensure_ascii=False) + '\n')
        sync_seconds, final = finish_metrics(session, initial)
        io = io_bytes(initial['io.stat'], final['io.stat'])
        correctness = validate_query_responses(records, queries, truth, responses)
        filtered = [row['elapsed_ns'] / 1e9 for row in measurements if row['filtered']]
        unfiltered = [row['elapsed_ns'] / 1e9 for row in measurements if not row['filtered']]
        result.update(
            stages={'startup_seconds': startup, 'schema_seconds': schema_seconds,
                    'load_seconds': math.fsum(load) / 1e9,
                    'index_prepare_seconds': index_seconds,
                    'warmup_seconds': math.fsum(warmup) / 1e9,
                    'sync_seconds': sync_seconds},
            load_batch_seconds=distribution([value / 1e9 for value in load]),
            warmup_query_seconds=distribution([value / 1e9 for value in warmup]),
            measurements=measurements,
            query_seconds=distribution([row['elapsed_ns'] / 1e9 for row in measurements]),
            filtered_seconds=distribution(filtered), unfiltered_seconds=distribution(unfiltered),
            T_seconds=math.fsum(row['elapsed_ns'] for row in measurements) / 1e9,
            B_bytes=max((row['total'] for row in io.values()), default=0),
            M_bytes=final['memory.peak'], io_by_device=io,
            cgroup_initial=initial, cgroup_final=final,
            responses_file=str(responses_path), correctness=correctness)
        session.shutdown()
        result['status'] = 'PASS'
    except Exception as error:
        result['error'] = repr(error)
    finally:
        session.close()
        result['cleaned'] = True
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    return result


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def summarize(rounds, image, output):
    summary = {'scope': 'local generated workload, not official score', 'seed': SEED,
               'rows': ROWS, 'dimension': DIMENSION, 'batch_rows': BATCH_ROWS,
               'queries': QUERY_COUNT, 'k': K, 'image': image,
               'image_id': json.loads(subprocess.check_output(
                   ['docker', 'image', 'inspect', image], text=True))[0]['Id'],
               'tool_sha256': sha256(__file__),
               'oracle_sha256': sha256(ROOT / 'tools/benchmark/minivectordb_oracle.cpp'),
               'rounds': rounds, 'projects': {}}
    for project in ['write', 'query']:
        values = [entry[project] for entry in rounds]
        project_summary = {'status': 'PASS' if all(row['status'] == 'PASS' for row in values) else 'FAIL'}
        for metric in ['T_seconds', 'B_bytes', 'M_bytes']:
            numbers = [row[metric] for row in values]
            project_summary[metric] = {'rounds': numbers, 'median': statistics.median(numbers)}
        stage_names = values[0]['stages'].keys()
        project_summary['stage_median_seconds'] = {
            name: statistics.median(row['stages'][name] for row in values) for name in stage_names}
        if project == 'query':
            project_summary['recall_at_10'] = [row['correctness']['recall_at_10'] for row in values]
            project_summary['query_seconds_median_distribution'] = {
                key: statistics.median(row['query_seconds'][key] for row in values)
                for key in ['p50', 'p95', 'p99', 'max']}
        summary['projects'][project] = project_summary
    summary['passed'] = all(project['status'] == 'PASS' for project in summary['projects'].values())
    summary['all_containers_and_volumes_cleaned'] = all(
        project['cleaned'] for entry in rounds for project in entry.values())
    (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='minivectordb:five-fixes')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.rounds <= 0:
        parser.error('--rounds must be positive')
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    oracle = output / 'oracle'
    subprocess.run(['c++', '-std=c++20', '-O2',
                    str(ROOT / 'tools/benchmark/minivectordb_oracle.cpp'),
                    '-o', str(oracle)], check=True)
    records, queries = build_dataset()
    oracle_input = output / 'oracle-input.bin'
    write_oracle_input(oracle_input, records, queries)
    truth = run_oracle(oracle, oracle_input)
    (output / 'truth.json').write_text(json.dumps(
        [{'query': query[0], 'rows': rows} for query, rows in zip(queries, truth)]) + '\n')
    rounds = []
    for index in range(args.rounds):
        write = run_write(index, args.image, records, output / f'round-{index}-write')
        print(f'write round={index} status={write["status"]} T={write.get("T_seconds")} '
              f'B={write.get("B_bytes")} M={write.get("M_bytes")}', flush=True)
        query = run_query(index, args.image, records, queries, truth, output / f'round-{index}-query')
        print(f'query round={index} status={query["status"]} T={query.get("T_seconds")} '
              f'B={query.get("B_bytes")} M={query.get("M_bytes")}', flush=True)
        rounds.append({'write': write, 'query': query})
        if write['status'] != 'PASS' or query['status'] != 'PASS':
            (output / 'failure-summary.json').write_text(json.dumps(rounds, indent=2) + '\n')
            raise SystemExit(1)
    summary = summarize(rounds, args.image, output)
    print(json.dumps(summary['projects'], ensure_ascii=False, indent=2), flush=True)
    if not summary['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
