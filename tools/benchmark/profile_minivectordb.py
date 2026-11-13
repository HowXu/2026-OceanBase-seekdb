"""Profile only the measured write/query phase of the same local workload."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from minivectordb_perf import (
    BATCH_ROWS, DIMENSION, K, ROOT, SEED, Session, CONFIG, build_dataset,
    create_schema, insert_batch, query_sql, response_rows, run_oracle,
    validate_query_responses, validate_written_records, write_oracle_input,
)


def profile(kind, perf, build, output):
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    records, queries = build_dataset()
    oracle = output / 'oracle'
    subprocess.run(['c++', '-std=c++20', '-O2',
                    str(ROOT / 'tools/benchmark/minivectordb_oracle.cpp'),
                    '-o', str(oracle)], check=True)
    truth = None
    if kind == 'query':
        source = output / 'oracle-input.bin'
        write_oracle_input(source, records, queries)
        truth = run_oracle(oracle, source)
    env_pythonpath = str((ROOT / build / 'python').resolve()) + ':' + str(ROOT / 'minivectordb/python')
    os.environ['PYTHONPATH'] = env_pythonpath
    command = [sys.executable, '-m', 'minivectordb', '--data-dir', '{data_dir}']
    session = Session(CONFIG, output / 'database-stderr.log', command=command)
    result = {'kind': kind, 'seed': SEED, 'build': str(build), 'perf': str(perf),
              'status': 'FAIL', 'note': 'host RelWithDebInfo profile; not baseline T/B/M'}
    recorder = None
    profile_log = (output / 'perf-record.log').open('wb')
    try:
        session.start()
        create_schema(session)
        if kind == 'query':
            for first in range(0, len(records), BATCH_ROWS):
                insert_batch(session, records[first:first + BATCH_ROWS])
            response = session.request('CREATE VECTOR INDEX vi ON items(v)')
            if response['ok'] is not True:
                raise AssertionError(response)
            import random
            from minivectordb_perf import f32
            warmup_random = random.Random(SEED + 2)
            for index in range(20):
                vector = [f32(warmup_random.uniform(-1, 1)) for _ in range(DIMENSION)]
                group = index % 10 if index % 2 == 0 else None
                if len(response_rows(session.request(query_sql(group, vector)))) != K:
                    raise AssertionError('warmup row count')
        data = output / f'perf-{kind}.data'
        recorder = subprocess.Popen([
            str(perf), 'record', '-F', '199', '-g', '--call-graph', 'dwarf',
            '-o', str(data), '-p', str(session.proc.pid)],
            stdout=subprocess.DEVNULL, stderr=profile_log)
        # Perf has no readiness response in attach mode. This fixed setup delay is
        # outside every benchmark measurement and only avoids losing initial samples.
        time.sleep(.1)
        started = time.perf_counter_ns()
        responses = []
        if kind == 'write':
            for first in range(0, len(records), BATCH_ROWS):
                insert_batch(session, records[first:first + BATCH_ROWS])
        else:
            for _, group, vector in queries:
                responses.append(session.request(query_sql(group, vector)))
        elapsed = (time.perf_counter_ns() - started) / 1e9
        recorder.send_signal(signal.SIGINT)
        recorder.wait(timeout=60)
        if recorder.returncode not in (0, -signal.SIGINT) or not data.exists():
            raise RuntimeError(f'perf record failed: {recorder.returncode}')
        result['perf_returncode'] = recorder.returncode
        recorder = None
        result['profiled_phase_seconds'] = elapsed
        result['perf_data_bytes'] = data.stat().st_size
        if kind == 'write':
            data_dir = session.temp.name
            session.shutdown()
            validation = Session(CONFIG, output / 'validation-stderr.log',
                                 command=[sys.executable, '-m', 'minivectordb',
                                          '--data-dir', data_dir])
            try:
                result['correctness'] = validate_written_records(validation, records)
                validation.shutdown()
            finally:
                validation.close()
        else:
            result['correctness'] = validate_query_responses(records, queries, truth, responses)
            session.shutdown()
        for name, extra in [('report.txt', ['--no-children']), ('callgraph.txt', [])]:
            with (output / name).open('w') as report:
                subprocess.run([str(perf), 'report', '--stdio', '--percent-limit=0.1',
                                '-i', str(data), *extra],
                               stdout=report, stderr=subprocess.STDOUT, check=True, timeout=120)
        result['status'] = 'PASS'
    except Exception as error:
        result['error'] = repr(error)
    finally:
        if recorder is not None and recorder.poll() is None:
            recorder.send_signal(signal.SIGINT)
            recorder.wait(timeout=60)
        profile_log.close()
        session.close()
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if result['status'] != 'PASS':
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=['write', 'query'])
    parser.add_argument('--perf', required=True)
    parser.add_argument('--build', default='build/perf-release')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    profile(args.kind, Path(args.perf), Path(args.build), Path(args.output))


if __name__ == '__main__':
    main()
