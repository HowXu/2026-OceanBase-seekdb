"""Independent scalar behavior; at most four source records per test."""
import pytest
from _minivectordb import Engine
from minivectordb.frontend import parse_statement
from minivectordb.errors import DatabaseError


@pytest.fixture
def db(tmp_path):
    engine = Engine(str(tmp_path))
    def execute(sql):
        return engine.execute(parse_statement(sql))
    yield execute
    engine.close()


def fails(db, sql, code):
    with pytest.raises(DatabaseError) as caught:
        db(sql)
    assert caught.value.code == code


def test_crud_projection_and_affected_rows(db):
    assert db('CREATE TABLE notes (id BIGINT PRIMARY KEY, n BIGINT, s VARCHAR(8))') == {}
    assert db("INSERT INTO notes (s,id,n) VALUES ('a',3,7),('b',1,8),('c',2,9)") == {'affected_rows': 3}
    result = db('SELECT * FROM notes')
    assert result['columns'] == ['id', 'n', 's']
    assert sorted(result['rows']) == [[1,8,'b'],[2,9,'c'],[3,7,'a']]
    assert db("UPDATE notes SET n=11,s='new' WHERE id=2") == {'affected_rows': 1}
    assert db('SELECT s AS label,id,42 AS fixed FROM notes WHERE id=2') == {
        'columns': ['label','id','fixed'], 'rows': [['new',2,42]]}
    assert db('UPDATE notes SET n=11 WHERE id=2') == {'affected_rows': 1}
    assert db('DELETE FROM notes WHERE n>=8') == {'affected_rows': 2}
    assert db('DELETE FROM notes WHERE id<0') == {'affected_rows': 0}
    assert db('SELECT s FROM notes WHERE id=99') == {'columns': ['s'], 'rows': []}


def test_comparisons_and_stable_sort_limit(db, tmp_path):
    db('CREATE TABLE ranks (id BIGINT PRIMARY KEY, n BIGINT, s VARCHAR(8))')
    db("INSERT INTO ranks VALUES (4,2,'z'),(2,1,'é'),(3,2,'a'),(1,1,'中')")
    for op, expected in [('=',[3,4]),('<',[1,2]),('<=',[1,2,3,4]),('>',[]),('>=',[3,4])]:
        assert db(f'SELECT id FROM ranks WHERE n{op}2 ORDER BY id')['rows'] == [[i] for i in expected]
    assert db('SELECT id FROM ranks WHERE n>=1 AND id>1 AND id<=3 ORDER BY n DESC')['rows'] == [[3],[2]]
    expected = [[3],[4],[1],[2]]
    assert db('SELECT id FROM ranks ORDER BY n DESC')['rows'] == expected
    assert db('SELECT id FROM ranks ORDER BY n DESC LIMIT 3')['rows'] == expected[:3]
    assert db('SELECT id FROM ranks ORDER BY n DESC LIMIT 0') == {'columns':['id'], 'rows':[]}
    assert db('SELECT id,n AS score FROM ranks ORDER BY score DESC,id DESC LIMIT 3')['rows'] == [[4,2],[3,2],[2,1]]
    assert db('SELECT id FROM ranks ORDER BY s')['rows'] == [[3],[4],[2],[1]]
    # Exercise run boundaries/merge without a large or pressure-test dataset.
    db('CREATE TABLE runs (id BIGINT PRIMARY KEY, s VARCHAR(70000))')
    for i, text in [(4,'d'),(1,'a'),(3,'c'),(2,'b')]:
        db(f"INSERT INTO runs VALUES ({i},'{text * 66000}')")
    objects = set((tmp_path/'objects').iterdir())
    assert db('SELECT id FROM runs ORDER BY s DESC')['rows'] == [[4],[3],[2],[1]]
    assert db('SELECT id FROM runs WHERE id>=2 ORDER BY s DESC')['rows'] == [[4],[3],[2]]
    assert db('SELECT id FROM runs WHERE id<0 ORDER BY s DESC')['rows'] == []
    assert db('SELECT id FROM runs ORDER BY s DESC LIMIT 2')['rows'] == [[4],[3]]
    assert set((tmp_path/'objects').iterdir()) == objects


@pytest.mark.parametrize('projection,columns,order,ascending,descending', [
    ('id,id', ['id','id'], 'id', [[1,1],[2,2],[3,3]], [[3,3],[2,2],[1,1]]),
    ('*,id', ['id','n','s','v','id'], 'id',
     [[1,2,'z',[3.,4.],1],[2,1,'a',[0.,0.],2],[3,1,'a',[0.,0.],3]],
     [[3,1,'a',[0.,0.],3],[2,1,'a',[0.,0.],2],[1,2,'z',[3.,4.],1]]),
    ('s,s,id', ['s','s','id'], 's',
     [['a','a',2],['a','a',3],['z','z',1]], [['z','z',1],['a','a',2],['a','a',3]]),
    ('n AS k,n AS k,id', ['k','k','id'], 'k',
     [[1,1,2],[1,1,3],[2,2,1]], [[2,2,1],[1,1,2],[1,1,3]]),
    ("L2_DISTANCE(v,'[0,0]') AS d,L2_DISTANCE(v,'[0,0]') AS d,id", ['d','d','id'], 'd',
     [[0.,0.,2],[0.,0.,3],[5.,5.,1]], [[5.,5.,1],[0.,0.,2],[0.,0.,3]]),
    ('id,1 AS k,1 AS k', ['id','k','k'], 'k',
     [[1,1,1],[2,1,1],[3,1,1]], [[1,1,1],[2,1,1],[3,1,1]]),
])
@pytest.mark.parametrize('limit', [None, 2, 0])
@pytest.mark.parametrize('desc', [False, True])
def test_equivalent_projections_can_share_order_name(db, projection, columns, order, ascending, descending, limit, desc):
    db('CREATE TABLE dup (id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(4),v VECTOR(2))')
    db("INSERT INTO dup VALUES (3,1,'a','[0,0]'),(1,2,'z','[3,4]'),(2,1,'a','[0,0]')")
    suffix = (' DESC' if desc else '') + (f' LIMIT {limit}' if limit is not None else '')
    expected = descending if desc else ascending
    if limit is not None:
        expected = expected[:limit]
    assert db(f'SELECT {projection} FROM dup ORDER BY {order}{suffix}') == {
        'columns': columns, 'rows': expected}


@pytest.mark.parametrize('projection', [
    'id AS k,n AS k', '1 AS k,2 AS k',
    "L2_DISTANCE(v,'[0,0]') AS k,L2_DISTANCE(v,'[1,0]') AS k",
])
@pytest.mark.parametrize('state', ['empty', 'rows', 'limit0'])
def test_distinct_projections_with_same_order_name_remain_ambiguous(db, projection, state):
    db('CREATE TABLE dup (id BIGINT PRIMARY KEY,n BIGINT,v VECTOR(2))')
    if state != 'empty':
        # Equal current values do not make different source expressions identical.
        db("INSERT INTO dup VALUES (1,1,'[0,0]')")
    suffix = ' LIMIT 0' if state == 'limit0' else ''
    fails(db, f'SELECT {projection} FROM dup ORDER BY k{suffix}', 'ambiguous_column')


def test_failed_candidates_are_invisible(db, tmp_path):
    db('CREATE TABLE safe (id BIGINT PRIMARY KEY, n BIGINT, s VARCHAR(2))')
    db("INSERT INTO safe VALUES (1,1,'x'),(4,4,'y')")
    baseline = db('SELECT * FROM safe')
    objects = set((tmp_path/'objects').iterdir())
    for sql, code in [
        ("INSERT INTO safe VALUES (2,2,'a'),(4,4,'b')", 'duplicate_key'),
        ("INSERT INTO safe VALUES (2,2,'a'),(2,3,'b')", 'duplicate_key'),
        ("INSERT INTO safe VALUES (2,2,'a'),(3,3,'long')", 'type_error'),
        ("UPDATE safe SET n=9,s='long'", 'type_error'),
        ('UPDATE safe SET id=2', 'primary_key_update'),
        ('UPDATE safe SET n=2,n=3', 'duplicate_column'),
        ('SELECT missing FROM safe LIMIT 0','unknown_column'),
        ('SELECT id FROM absent','unknown_table'),
        ('SELECT id FROM safe WHERE n=\'1\'', 'type_error'),
        ('SELECT id FROM safe ORDER BY missing LIMIT 0','unknown_column'),
        ('DELETE FROM safe WHERE absent=1','unknown_column'),
        ('INSERT INTO safe (id,n) VALUES (2,2)','type_error'),
    ]:
        fails(db, sql, code)
        assert db('SELECT * FROM safe') == baseline
        assert set((tmp_path/'objects').iterdir()) == objects
    fails(db, 'CREATE TABLE safe (id BIGINT PRIMARY KEY)', 'table_exists')
    # Fail after the rewrite has already streamed its first row into a candidate.
    source = max(objects, key=lambda path: path.stat().st_size)
    original = source.read_bytes()
    try:
        source.write_bytes(original[:-1])
        fails(db, 'UPDATE safe SET n=7', 'storage_error')
    finally:
        source.write_bytes(original)
    assert set((tmp_path/'objects').iterdir()) == objects
    assert db('SELECT * FROM safe') == baseline
    # Literal assignments are independently bound against the original schema/row.
    assert db("UPDATE safe SET n=8,s='好' WHERE id=1") == {'affected_rows':1}
    assert db('SELECT * FROM safe WHERE id=1')['rows'] == [[1,8,'好']]


def test_unicode_and_exact_bigint(db):
    db('CREATE TABLE exact (id BIGINT PRIMARY KEY, n BIGINT, s VARCHAR(2))')
    db("INSERT INTO exact VALUES (-9223372036854775808,9223372036854775807,'中😀'),(9007199254740993,9007199254740992,'é'),(9223372036854775807,-9223372036854775808,'é')")
    assert db('SELECT id FROM exact WHERE id>9007199254740992 ORDER BY id')['rows'] == [[9007199254740993],[9223372036854775807]]
    assert db('SELECT n FROM exact ORDER BY n')['rows'] == [[-9223372036854775808],[9007199254740992],[9223372036854775807]]
    assert db("SELECT s FROM exact WHERE s='é'")['rows'] == [['é']]
    assert db("SELECT s FROM exact WHERE s='é'")['rows'] == [['é']]
    fails(db, "UPDATE exact SET s='中文😀'", 'type_error')
    assert db('SELECT id FROM exact ORDER BY id LIMIT 1')['rows'] == [[-9223372036854775808]]
