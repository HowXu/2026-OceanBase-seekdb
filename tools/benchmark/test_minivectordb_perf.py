import json
import math
from pathlib import Path
import struct
import subprocess

from minivectordb_perf import (
    build_dataset, distribution, exact_topk_matches, io_bytes,
    recall_at_k, run_oracle, write_oracle_input,
)


def test_default_shape_and_determinism():
    records, queries = build_dataset()
    assert len(records) == 10_000 and len(queries) == 1_000
    assert len(records[0][3]) == 128
    assert sum(q[1] is not None for q in queries) == 500
    assert all(q[1] == i % 10 for i, q in enumerate(queries) if i % 2 == 0)
    assert records[0] == build_dataset()[0][0]


def test_distribution_is_recomputable():
    assert distribution([1, 2, 3, 4]) == {
        'count': 4, 'sum': 10.0, 'min': 1.0,
        'p50': 2.0, 'p95': 4.0, 'p99': 4.0, 'max': 4.0}
    assert distribution([]) == {
        'count': 0, 'sum': 0.0, 'min': None,
        'p50': None, 'p95': None, 'p99': None, 'max': None}


def test_recall_and_exact_ids():
    actual = [[2, 0.2], [1, 0.2]]
    expected = [[1, 0.2], [2, 0.2]]
    assert recall_at_k([x[0] for x in actual], [x[0] for x in expected], 10) == 1.0
    assert exact_topk_matches(expected, expected)
    assert not exact_topk_matches(actual, expected)
    assert exact_topk_matches(actual, list(reversed(expected)))
    assert recall_at_k([], [], 10) is None


def test_metric_delta_uses_device_total_without_double_counting():
    before = {'259:0': {'rbytes': 10, 'wbytes': 20}}
    after = {'259:0': {'rbytes': 30, 'wbytes': 50}}
    assert io_bytes(before, after) == {'259:0': {'rbytes': 20, 'wbytes': 30, 'total': 50}}


def test_independent_oracle_filters_then_applies_distance_and_pk_ties(tmp_path):
    records = [
        (2, 0, 'b', [1.0, 0.0]),
        (1, 1, 'a', [0.0, 0.0]),
        (3, 1, 'c', [-1.0, 0.0]),
        (4, 1, 'd', [0.0, 2.0]),
    ]
    queries = [(10, None, [0.0, 0.0]), (11, 1, [0.0, 0.0]), (12, 9, [0.0, 0.0])]
    source = tmp_path / 'input.bin'
    write_oracle_input(source, records, queries, k=2)
    executable = tmp_path / 'oracle'
    subprocess.run([
        'c++', '-std=c++20', '-O2',
        str(Path(__file__).with_name('minivectordb_oracle.cpp')),
        '-o', str(executable)], check=True)
    actual = run_oracle(executable, source)
    assert len(actual) == 3
    expected = []
    for _, grp, query in queries:
        distances = []
        for ident, row_grp, _, vector in records:
            if grp is not None and row_grp != grp:
                continue
            distance = math.sqrt(math.fsum((a - b) ** 2 for a, b in zip(vector, query)))
            distances.append([ident, distance])
        distances.sort(key=lambda item: (item[1], item[0]))
        expected.append(distances[:2])
    for result, wanted in zip(actual, expected):
        assert [row[0] for row in result] == [row[0] for row in wanted]
        for row, target in zip(result, wanted):
            assert math.isclose(row[1], target[1], rel_tol=1e-12, abs_tol=1e-12)
    assert actual[2] == []
