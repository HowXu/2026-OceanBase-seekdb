"""Tiny independent exact-search oracle; no public fixtures or engine self-oracle."""
import math
import struct

import pytest
from _minivectordb import Engine
from minivectordb.errors import DatabaseError
from minivectordb.frontend import parse_statement


@pytest.fixture
def db(tmp_path):
    engine = Engine(str(tmp_path))
    def execute(sql, mode='exact'):
        return engine.execute(parse_statement(sql), mode)
    yield execute
    engine.close()


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def oracle(records, query):
    query = list(map(f32, query))
    return sorted([[pk, math.sqrt(sum((f32(x)-q)**2 for x, q in zip(v, query)))]
                   for pk, v in records], key=lambda row: (row[1], row[0]))


def test_euclidean_distance_and_independent_oracle(db):
    db('CREATE TABLE points (id BIGINT PRIMARY KEY, v VECTOR(2))')
    db("INSERT INTO points VALUES (3,'[3,4]'),(1,'[-2,1]'),(2,'[0,0]')")
    assert db("SELECT L2_DISTANCE(v,'[0,0]') AS d FROM points WHERE id=3") == {
        'columns': ['d'], 'rows': [[5.0]]}
    records = [(3,[3,4]), (1,[-2,1]), (2,[0,0])]
    for query in ([0,0], [1.25,-0.5]):
        sql = f"SELECT id,L2_DISTANCE(v,'{query}') AS d FROM points ORDER BY d LIMIT 3"
        expected = oracle(records, query)
        for mode in ('exact', 'ann'):
            result = db(sql, mode)
            assert result['columns'] == ['id', 'd']
            assert result['rows'] == expected
    # Direct distance ordering and unbounded external-sort path share semantics.
    assert db("SELECT id FROM points ORDER BY L2_DISTANCE(v,'[0,0]')")['rows'] == [[2],[1],[3]]


def test_filter_before_topk_ties_empty_and_visible_mutations(db):
    db('CREATE TABLE points (id BIGINT PRIMARY KEY, keep BIGINT, v VECTOR(2))')
    db("INSERT INTO points VALUES (4,1,'[0,-5]'),(2,0,'[0,0]'),(3,1,'[0,5]'),(1,0,'[1,0]')")
    base = "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM points WHERE keep=1 ORDER BY d"
    for k in (0,1,2,9):
        assert db(f'{base} LIMIT {k}') == {'columns':['id','d'], 'rows':[[3,5.0],[4,5.0]][:k]}
    assert db(base.replace('keep=1','keep=9') + ' LIMIT 2') == {'columns':['id','d'], 'rows':[]}
    assert db(base + ' DESC LIMIT 2')['rows'] == [[3,5.0],[4,5.0]]
    db("UPDATE points SET v='[0,2]' WHERE id=4")
    assert db(base+' LIMIT 1')['rows'] == [[4,2.0]]
    db('DELETE FROM points WHERE id=4')
    assert db(base+' LIMIT 2')['rows'] == [[3,5.0]]


def test_float32_storage_query_and_double_arithmetic(db):
    db('CREATE TABLE points (id BIGINT PRIMARY KEY, v VECTOR(2))')
    db("INSERT INTO points VALUES (1,'[16777217,0]'),(2,'[3e38,0]')")
    assert db('SELECT v FROM points WHERE id=1')['rows'] == [[[16777216.0,0.0]]]
    for query in ('[16777216,0]', '[16777217,0]'):
        assert db(f"SELECT L2_DISTANCE(v,'{query}') AS d FROM points WHERE id=1")['rows'] == [[0.0]]
    # Subtraction and squaring in float would overflow, but double must not.
    distance = db("SELECT L2_DISTANCE(v,'[-3e38,0]') AS d FROM points WHERE id=2")['rows'][0][0]
    assert math.isfinite(distance)
    assert distance == 2 * f32(3e38)


def test_invalid_vectors_and_atomic_mutations(db, tmp_path):
    db('CREATE TABLE points (id BIGINT PRIMARY KEY, v VECTOR(2))')
    db("INSERT INTO points VALUES (1,'[3,4]')")
    baseline = db('SELECT * FROM points')
    objects = set((tmp_path/'objects').iterdir())
    for bad in ('[NaN,0]', '[Infinity,0]', '[-Infinity,0]', '[1e999,0]', '[3.5e38,0]',
                '[1]', '[1,2,3]', '[1,]', '[1,2]junk', 'not a vector'):
        for sql in (
            f"INSERT INTO points VALUES (2,'[0,0]'),(3,'{bad}')",
            f"UPDATE points SET v='{bad}' WHERE id=1",
            f"SELECT L2_DISTANCE(v,'{bad}') AS d FROM points LIMIT 0",
            f"SELECT id FROM points ORDER BY L2_DISTANCE(v,'{bad}') LIMIT 0",
        ):
            with pytest.raises(DatabaseError) as caught:
                db(sql)
            assert caught.value.code == 'type_error'
            assert db('SELECT * FROM points') == baseline
            assert set((tmp_path/'objects').iterdir()) == objects
    with pytest.raises(DatabaseError) as caught:
        db("SELECT L2_DISTANCE(id,'[0,0]') AS d FROM points")
    assert caught.value.code == 'type_error'
    with pytest.raises(DatabaseError) as caught:
        db("SELECT L2_DISTANCE(missing,'[0,0]') AS d FROM points")
    assert caught.value.code == 'unknown_column'
