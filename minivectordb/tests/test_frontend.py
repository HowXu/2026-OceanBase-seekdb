"""Independent SQL-to-frozen-IR examples, never public fixtures."""
import pytest

from minivectordb.errors import DatabaseError


def parse_statement(sql):
    from minivectordb.frontend import parse_statement as parse
    return parse(sql)


def integer(text):
    return {"kind": "integer", "text": text}


def string(text):
    return {"kind": "string", "text": text}


def column(name):
    return {"kind": "column", "name": name}


def test_statement_families():
    assert parse_statement("CREATE TABLE notes (id BIGINT PRIMARY KEY, title VARCHAR(19), v VECTOR(3))") == {
        "op": "create_table", "table": "notes", "columns": [
            {"name": "id", "type": "bigint", "primary_key": True},
            {"name": "title", "type": "varchar", "length": 19},
            {"name": "v", "type": "vector", "length": 3}]}
    assert parse_statement("INSERT INTO notes (id,title) VALUES (7,'a'), (8,'b')") == {
        "op": "insert", "table": "notes", "columns": ["id", "title"],
        "rows": [[integer("7"), string("a")], [integer("8"), string("b")]]}
    condition = {"left": column("id"), "operator": ">=", "right": integer("-9")}
    distance = {"kind": "l2_distance", "column": "v", "vector": string("[1,2,3]")}
    assert parse_statement("SELECT id AS k, L2_DISTANCE(v,'[1,2,3]') AS d FROM notes "
                           "WHERE id >= -9 AND title = 'a' ORDER BY d DESC, id ASC LIMIT 0") == {
        "op": "select", "table": "notes", "projections": [
            {"expression": column("id"), "alias": "k"}, {"expression": distance, "alias": "d"}],
        "where": [condition, {"left": column("title"), "operator": "=", "right": string("a")}],
        "order_by": [{"expression": column("d"), "descending": True},
                     {"expression": column("id"), "descending": False}], "limit": 0}
    assert parse_statement("SELECT * FROM notes") == {
        "op": "select", "table": "notes", "projections": [{"expression": {"kind": "star"}}]}
    assert parse_statement("UPDATE notes SET title='new', id=2 WHERE id>=-9") == {
        "op": "update", "table": "notes", "assignments": [
            {"column": "title", "value": string("new")}, {"column": "id", "value": integer("2")}],
        "where": [condition]}
    assert parse_statement("DELETE FROM notes WHERE id>=-9") == {
        "op": "delete", "table": "notes", "where": [condition]}
    for sql, op in [("BEGIN", "begin"), ("START TRANSACTION", "begin"),
                    ("COMMIT", "commit"), ("ROLLBACK", "rollback")]:
        assert parse_statement(sql) == {"op": op}


def test_order_ordinal_parentheses_and_display_text():
    assert parse_statement("SELECT l2_distance(v,'[0,0]'), (id) FROM notes ORDER BY 1") == {
        "op": "select", "table": "notes", "projections": [
            {"expression": {"kind": "l2_distance", "column": "v",
                            "vector": string("[0,0]")},
             "display_text": "l2_distance(v,'[0,0]')"},
            {"expression": column("id"), "display_text": "(id)"}],
        "order_by": [{"expression": {"kind": "ordinal", "position": 1},
                      "descending": False}]}


def test_literal_precision_and_escaping():
    assert parse_statement(r"INSERT INTO `odd;table` VALUES (9007199254740993, -9223372036854775808, 9223372036854775807, '中文;it''s', 'a\nb', '[1,2]');")["rows"] == [[
        integer("9007199254740993"), integer("-9223372036854775808"),
        integer("9223372036854775807"), string("中文;it's"), string("a\nb"), string("[1,2]")]]


def test_vector_index_token_grammar():
    expected = {"op": "create_vector_index", "index": "ix;name", "table": "notes", "column": "v"}
    assert parse_statement("CREATE /* comment */ VECTOR INDEX `ix;name` ON notes(v);") == expected
    assert parse_statement("create vector index `ix;name` on `notes` (`v`) -- tail\n") == expected


@pytest.mark.parametrize("sql", [
    "ALTER TABLE notes ADD x BIGINT", "SHOW TABLES", "CREATE INDEX ix ON notes(id)",
    "CREATE WIDGET thing",
    "SELECT id FROM notes; DELETE FROM notes", "BEGIN; COMMIT", "", ";", "SELECT FROM",
    "SELECT id FROM notes JOIN other ON notes.id=other.id", "SELECT DISTINCT id FROM notes",
    "SELECT id FROM notes GROUP BY id", "SELECT id FROM notes HAVING id=1",
    "SELECT id FROM notes UNION SELECT id FROM notes", "SELECT id FROM (SELECT id FROM notes) s",
    "SELECT id FROM notes LIMIT 1 OFFSET 2", "SELECT id FROM notes WHERE id=1 OR id=2",
    "SELECT id FROM notes WHERE id != 1", "SELECT mystery(id) FROM notes",
    "SELECT L2_DISTANCE(v, '[1]', 2) FROM notes", "SELECT L2_DISTANCE(id+1, '[1]') FROM notes",
    "SELECT id+1 FROM notes", "SELECT NULL FROM notes", "SELECT 1.5 FROM notes",
    "SELECT id FROM notes ORDER BY -1", "SELECT id FROM notes ORDER BY id COLLATE utf8_bin",
    "SELECT id FROM notes FOR UPDATE", "SELECT id FROM notes ORDER BY id NULLS LAST",
    "SELECT id FROM notes ORDER BY id DESC NULLS FIRST", "INSERT IGNORE INTO notes VALUES (1)",
    "INSERT INTO notes SELECT * FROM other", "UPDATE notes SET id=id+1", "DELETE FROM notes LIMIT 1",
    "CREATE TABLE notes (id INT)", "CREATE TABLE notes (id BIGINT DEFAULT 1)",
    "CREATE TABLE IF NOT EXISTS notes (id BIGINT)", "CREATE TABLE notes (id BIGINT) ENGINE=InnoDB",
    "COMMIT AND CHAIN", "COMMIT AND NO CHAIN", "ROLLBACK AND NO CHAIN",
    "ROLLBACK TO SAVEPOINT s", "START TRANSACTION READ ONLY",
    "CREATE VECTOR INDEX ix ON notes(v) garbage", "CREATE VECTOR INDEX ix ON notes(v,id)",
    "CREATE VECTOR INDEX ix ON notes(v); BEGIN", "CREATE VECTOR INDEX 'ix' ON notes(v)",
    "CREATE VECTOR INDEX ix ON notes(v) USING HNSW", "CREATE VECTOR INDEX ix ON db.notes(v)",
    "CREATE VECTOR INDEX ix ON notes(v);;", "SELECT id FROM notes;;",
    "SELECT '\ud800' FROM notes", "SELECT `\udfff` FROM notes",
])
def test_rejects_unrepresented_syntax(sql):
    with pytest.raises(DatabaseError) as caught:
        parse_statement(sql)
    assert caught.value.code == "unsupported_sql"
    assert caught.value.message


def test_parsed_ir_reaches_real_binding(tmp_path):
    from _minivectordb import Engine
    engine = Engine(str(tmp_path))
    try:
        assert engine.execute(parse_statement("CREATE TABLE n (id BIGINT PRIMARY KEY, v VECTOR(2))")) == {}
        assert engine.execute(parse_statement("INSERT INTO n VALUES (-9223372036854775808, '[1,2]')")) == {'affected_rows': 1}
        assert engine.execute(parse_statement("SELECT * FROM n")) == {
            'columns': ['id', 'v'], 'rows': [[-9223372036854775808, [1.0, 2.0]]]}
        assert engine.execute(parse_statement(
            "SELECT L2_DISTANCE(v,'[0,0]') AS d FROM n WHERE id<2 ORDER BY d LIMIT 1")) == {
                'columns': ['d'], 'rows': [[5 ** 0.5]]}
        assert engine.execute(parse_statement("CREATE VECTOR INDEX i ON n(v)")) == {}
        for sql, code in [
            ("UPDATE n SET id=3", 'primary_key_update'),
            ("CREATE VECTOR INDEX i ON n(v)", 'index_exists'),
        ]:
            with pytest.raises(DatabaseError) as caught:
                engine.execute(parse_statement(sql))
            assert caught.value.code == code, sql
        for command in ('BEGIN', 'COMMIT', 'ROLLBACK'):
            assert engine.execute(parse_statement(command)) == {}
        assert engine.execute(parse_statement("DELETE FROM n")) == {'affected_rows': 1}
    finally:
        engine.close()


def test_execute_request_parses_and_passes_payload_and_mode():
    from minivectordb.__main__ import execute_request

    class RecordingEngine:
        def execute(self, statement, mode):
            assert statement == {"op": "delete", "table": "notes"}
            assert mode == "ann"
            return {"affected_rows": 4}

    assert execute_request(RecordingEngine(), {"sql": "DELETE FROM notes", "search_mode": "ann"}) == {"affected_rows": 4}
    with pytest.raises(DatabaseError) as caught:
        execute_request(RecordingEngine(), {"sql": "ALTER TABLE notes ADD x BIGINT"})
    assert caught.value.code == "unsupported_sql"
