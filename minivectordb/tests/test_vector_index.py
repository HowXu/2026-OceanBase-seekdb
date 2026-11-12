"""Independent miniature FlatScan declaration and visible-root tests."""
import struct

import pytest
from _minivectordb import Engine
from minivectordb.errors import DatabaseError
from minivectordb.frontend import parse_statement


def execute(db, sql, mode='exact'):
    return db.execute(parse_statement(sql), mode)


def descriptor(name, table='points', column='v'):
    # Catalog v3: three length-prefixed names and backend tag 1 (FlatScan).
    return b''.join(struct.pack('<Q', len(s.encode())) + s.encode()
                    for s in (name, table, column)) + b'\x01'


def catalog_bytes(path):
    return (path / (path / 'CURRENT').read_text().strip()).read_bytes()


def check_rows(db, expected):
    sql = "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM points WHERE keep=1 ORDER BY d LIMIT 9"
    for mode in ('exact', 'ann'):
        result = execute(db, sql, mode)
        assert result == {'columns': ['id', 'd'], 'rows': expected}
        ids = [row[0] for row in result['rows']]
        assert len(ids) == len(set(ids))


def test_definition_validation_and_persisted_descriptor(tmp_path):
    db = Engine(str(tmp_path))
    execute(db, 'CREATE TABLE points (id BIGINT PRIMARY KEY, keep BIGINT, v VECTOR(2))')
    execute(db, 'CREATE TABLE other (id BIGINT PRIMARY KEY, v VECTOR(2))')
    before = catalog_bytes(tmp_path)
    for sql, code in [
        ('CREATE VECTOR INDEX ix ON missing(v)', 'unknown_table'),
        ('CREATE VECTOR INDEX ix ON points(missing)', 'unknown_column'),
        ('CREATE VECTOR INDEX ix ON points(keep)', 'type_error'),
    ]:
        with pytest.raises(DatabaseError) as error:
            execute(db, sql)
        assert error.value.code == code
        assert catalog_bytes(tmp_path) == before
    # Validate names even at the native boundary, not only SQL parsing.
    for name in ('', 'bad\x00name'):
        with pytest.raises(DatabaseError):
            db.execute({'op': 'create_vector_index', 'index': name, 'table': 'points', 'column': 'v'})
    assert execute(db, 'CREATE VECTOR INDEX ix ON points(v)') == {}
    data = catalog_bytes(tmp_path)
    assert data.startswith(b'MVDBC003')
    assert data.endswith(struct.pack('<Q', 1) + descriptor('ix') + b'\x00')
    for table in ('points', 'other'):
        with pytest.raises(DatabaseError) as error:
            execute(db, f'CREATE VECTOR INDEX ix ON {table}(v)')
        assert error.value.code == 'index_exists'
        assert catalog_bytes(tmp_path) == data
    db.close()
    db = Engine(str(tmp_path))
    with pytest.raises(DatabaseError) as error:
        execute(db, 'CREATE VECTOR INDEX ix ON points(v)')
    assert error.value.code == 'index_exists'
    db.close()
    # Persisted descriptors are validated on recovery, not blindly trusted.
    current = tmp_path / (tmp_path / 'CURRENT').read_text().strip()
    for invalid, code in [
        (descriptor('ix', column='keep'), 'type_error'),
        (descriptor('ix', table='missing'), 'unknown_table'),
        (descriptor('ix', column='missing'), 'unknown_column'),
        (descriptor('ix')[:-1] + b'\x02', 'storage_error'),
    ]:
        current.write_bytes(data.replace(descriptor('ix'), invalid))
        with pytest.raises(DatabaseError) as error:
            Engine(str(tmp_path))
        assert error.value.code == code


def test_indexed_crud_modes_and_reopen(tmp_path):
    db = Engine(str(tmp_path))
    execute(db, 'CREATE TABLE points (id BIGINT PRIMARY KEY, keep BIGINT, v VECTOR(2))')
    execute(db, "INSERT INTO points VALUES (2,0,'[0,0]'),(1,1,'[3,4]')")
    execute(db, 'CREATE VECTOR INDEX scan ON points(v)')
    check_rows(db, [[1, 5.0]])
    execute(db, "INSERT INTO points VALUES (4,1,'[0,2]'),(3,1,'[0,-2]')")
    check_rows(db, [[3, 2.0], [4, 2.0], [1, 5.0]])
    execute(db, "UPDATE points SET v='[0,1]' WHERE id=1")
    execute(db, 'DELETE FROM points WHERE id=3')
    check_rows(db, [[1, 1.0], [4, 2.0]])
    with pytest.raises(DatabaseError):
        execute(db, "INSERT INTO points VALUES (5,1,'[0,0]'),(1,1,'[0,0]')")
    check_rows(db, [[1, 1.0], [4, 2.0]])
    db.close()
    db = Engine(str(tmp_path))
    check_rows(db, [[1, 1.0], [4, 2.0]])
    assert descriptor('scan') in catalog_bytes(tmp_path)
    db.close()


def test_transaction_descriptors_vectors_and_rollback(tmp_path):
    db = Engine(str(tmp_path))
    execute(db, 'CREATE TABLE points (id BIGINT PRIMARY KEY, keep BIGINT, v VECTOR(2))')
    execute(db, "INSERT INTO points VALUES (1,1,'[3,4]'),(2,0,'[0,0]')")
    committed = catalog_bytes(tmp_path)
    execute(db, 'BEGIN')
    execute(db, 'CREATE VECTOR INDEX temporary ON points(v)')
    execute(db, "UPDATE points SET v='[0,1]' WHERE id=1")
    check_rows(db, [[1, 1.0]])
    assert catalog_bytes(tmp_path) == committed
    execute(db, 'ROLLBACK')
    check_rows(db, [[1, 5.0]])
    execute(db, 'BEGIN')
    execute(db, 'CREATE VECTOR INDEX temporary ON points(v)')
    execute(db, 'COMMIT')
    assert descriptor('temporary') in catalog_bytes(tmp_path)
    execute(db, 'BEGIN')
    execute(db, 'DELETE FROM points WHERE id=1')
    check_rows(db, [])
    execute(db, 'ROLLBACK')
    check_rows(db, [[1, 5.0]])
    execute(db, 'BEGIN')
    execute(db, "UPDATE points SET v='[0,2]' WHERE id=1")
    execute(db, 'COMMIT')
    execute(db, 'BEGIN')
    execute(db, 'CREATE VECTOR INDEX discarded ON points(v)')
    execute(db, 'DELETE FROM points WHERE id=1')
    db.close()  # uncommitted descriptor and deletion both disappear
    db = Engine(str(tmp_path))
    check_rows(db, [[1, 2.0]])
    assert descriptor('discarded') not in catalog_bytes(tmp_path)
    execute(db, 'CREATE VECTOR INDEX discarded ON points(v)')
    db.close()
