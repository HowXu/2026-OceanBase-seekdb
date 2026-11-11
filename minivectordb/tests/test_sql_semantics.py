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
