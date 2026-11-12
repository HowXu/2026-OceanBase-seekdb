"""Small deterministic incremental-segment and compaction regressions."""
from _minivectordb import Engine
from minivectordb.frontend import parse_statement


def execute(db, sql):
    return db.execute(parse_statement(sql))


def test_single_row_updates_append_small_deltas(tmp_path):
    db = Engine(str(tmp_path))
    execute(db, 'CREATE TABLE inc (id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(32))')
    payload = 'x' * 32
    execute(db, 'INSERT INTO inc VALUES ' + ','.join(
        f"({i},{i},'{payload}')" for i in range(256)))
    before = {p.name:p.stat().st_size for p in (tmp_path/'objects').iterdir()}
    # The initial INSERT already owns one delta; stay below the compaction threshold.
    for i in range(63):
        assert execute(db, f'UPDATE inc SET n={1000+i} WHERE id={i}') == {'affected_rows':1}
    after = {p.name:p.stat().st_size for p in (tmp_path/'objects').iterdir()}
    assert len(after) == len(before) + 63
    assert sum(after.values()) - sum(before.values()) < 64 * 1024
    assert execute(db, 'SELECT id,n FROM inc ORDER BY id LIMIT 3')['rows'] == [
        [0,1000],[1,1001],[2,1002]]
    db.close()


def test_compaction_reinsert_rollback_and_recovery(tmp_path):
    db = Engine(str(tmp_path))
    execute(db, 'CREATE TABLE inc (id BIGINT PRIMARY KEY,n BIGINT)')
    execute(db, 'INSERT INTO inc VALUES ' + ','.join(f'({i},{i})' for i in range(128)))
    for i in range(70):
        assert execute(db, f'UPDATE inc SET n={2000+i} WHERE id={i%16}') == {'affected_rows':1}
    expected = [2000 + max(j for j in range(70) if j % 16 == i) for i in range(16)]
    assert execute(db, 'SELECT id,n FROM inc ORDER BY id LIMIT 16')['rows'] == [
        [i,expected[i]] for i in range(16)]
    # A tombstone in a newer segment does not prevent a later reinsert.
    execute(db, 'DELETE FROM inc WHERE id=3')
    execute(db, 'INSERT INTO inc VALUES (3,3333)')
    assert execute(db, 'SELECT n FROM inc WHERE id=3')['rows'] == [[3333]]
    execute(db, 'BEGIN')
    execute(db, 'UPDATE inc SET n=4444 WHERE id=3')
    execute(db, 'DELETE FROM inc WHERE id=4')
    assert execute(db, 'SELECT id,n FROM inc WHERE id<=4 ORDER BY id')['rows'] == [
        [0,2064],[1,2065],[2,2066],[3,4444]]
    execute(db, 'ROLLBACK')
    assert execute(db, 'SELECT id,n FROM inc WHERE id<=4 ORDER BY id')['rows'] == [
        [0,2064],[1,2065],[2,2066],[3,3333],[4,2068]]
    db.close()
    db = Engine(str(tmp_path))
    assert execute(db, 'SELECT id,n FROM inc WHERE id<=4 ORDER BY id')['rows'] == [
        [0,2064],[1,2065],[2,2066],[3,3333],[4,2068]]
    db.close()


def test_noop_and_failed_insert_leave_object_set_unchanged(tmp_path):
    db = Engine(str(tmp_path))
    execute(db, 'CREATE TABLE inc (id BIGINT PRIMARY KEY,n BIGINT)')
    execute(db, 'INSERT INTO inc VALUES (1,1),(2,2)')
    execute(db, 'UPDATE inc SET n=3 WHERE id=1')
    before = set((tmp_path/'objects').iterdir())
    assert execute(db, 'UPDATE inc SET n=9 WHERE id=99') == {'affected_rows':0}
    assert set((tmp_path/'objects').iterdir()) == before
    try:
        execute(db, 'INSERT INTO inc VALUES (4,4),(2,9)')
    except Exception:
        pass
    else:
        raise AssertionError('duplicate INSERT unexpectedly succeeded')
    assert set((tmp_path/'objects').iterdir()) == before
    assert execute(db, 'SELECT * FROM inc ORDER BY id')['rows'] == [[1,3],[2,2]]
    db.close()
