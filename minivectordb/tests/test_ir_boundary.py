"""Concrete controlled IR is validated in the real C++ binding, before execution."""
import importlib
import pytest


def modules():
    return importlib.import_module('_minivectordb'), importlib.import_module('minivectordb.errors')


def lit(text, kind='integer'):
    return {'kind': kind, 'text': text}


def col(name='id'):
    return {'kind': 'column', 'name': name}


def test_real_extension_lifecycle_and_error_contract(tmp_path):
    core, errors = modules()
    assert any(core.__file__.endswith(suffix) for suffix in importlib.machinery.EXTENSION_SUFFIXES)
    engine = core.Engine(str(tmp_path))
    for mode in ('exact', 'ann'):
        assert engine.execute({'op': 'begin'}, mode) == {}
        with pytest.raises(errors.DatabaseError) as caught:
            engine.execute({'op': 'begin'}, mode)
        assert caught.value.code == 'transaction_active'
        assert caught.value.message and str(caught.value) == caught.value.message
        assert engine.execute({'op': 'rollback'}, mode) == {}
    engine.close()
    engine.close()
    with pytest.raises(errors.DatabaseError, match='closed') as caught:
        engine.execute({'op': 'begin'})
    assert caught.value.code == 'engine_closed'


def test_all_statement_variants_and_precision(tmp_path):
    core, errors = modules()
    engine = core.Engine(str(tmp_path))
    condition = {'left': col(), 'operator': '>=', 'right': lit('-9223372036854775808')}
    distance = {'kind': 'l2_distance', 'column': 'v', 'vector': lit('[16777217,0]', 'string')}
    assert engine.execute({'op': 'create_table', 'table': 'docs', 'columns': [
        {'name': 'id', 'type': 'bigint', 'primary_key': True},
        {'name': 'text', 'type': 'varchar', 'length': 12},
        {'name': 'v', 'type': 'vector', 'length': 2}]}) == {}
    for statement in [
        {'op': 'insert', 'table': 'docs', 'rows': [[lit('9007199254740993'), lit('中文;[]', 'string')]]},
        {'op': 'insert', 'table': 'docs', 'columns': ['id'], 'rows': [[lit('9223372036854775807')]]},
    ]:
        with pytest.raises(errors.DatabaseError) as caught:
            engine.execute(statement)
        assert caught.value.code == 'type_error'
    values = [[lit('-9223372036854775808'), lit('中文;[]', 'string'), lit('[16777217,0]', 'string')],
              [lit('9007199254740993'), lit('large', 'string'), lit('[0,1]', 'string')],
              [lit('9223372036854775807'), lit('max', 'string'), lit('[1,2]', 'string')]]
    assert engine.execute({'op': 'insert', 'table': 'docs', 'rows': values}) == {'affected_rows': 3}
    assert engine.execute({'op': 'select', 'table': 'docs', 'projections': [
        {'expression': col(), 'alias': 'key'}], 'where': [condition],
        'order_by': [{'expression': col(), 'descending': False}]}) == {
            'columns': ['key'], 'rows': [[-9223372036854775808], [9007199254740993], [9223372036854775807]]}
    assert engine.execute({'op': 'select', 'table': 'docs', 'projections': [
        {'expression': {'kind': 'star'}}]}) == {'columns': ['id', 'text', 'v'], 'rows': [
            [-9223372036854775808, '中文;[]', [16777216.0, 0.0]],
            [9007199254740993, 'large', [0.0, 1.0]],
            [9223372036854775807, 'max', [1.0, 2.0]]]}
    vector_select = {'op': 'select', 'table': 'docs', 'projections': [
        {'expression': col(), 'alias': 'key'}, {'expression': distance, 'alias': 'd'}],
        'where': [condition], 'order_by': [{'expression': distance}], 'limit': 0}
    assert engine.execute(vector_select) == {'columns': ['key', 'd'], 'rows': []}
    vector_select['limit'] = 1
    assert engine.execute(vector_select) == {
        'columns': ['key', 'd'], 'rows': [[-9223372036854775808, 0.0]]}
    assert engine.execute({'op': 'update', 'table': 'docs', 'assignments': [
        {'column': 'text', 'value': lit('ok', 'string')}], 'where': []}) == {'affected_rows': 3}
    assert engine.execute({'op': 'delete', 'table': 'docs'}) == {'affected_rows': 3}
    assert engine.execute({'op': 'select', 'table': 'docs', 'projections': [
        {'expression': col()}], 'limit': 0}) == {'columns': ['id'], 'rows': []}
    index = {'op': 'create_vector_index', 'index': 'ix', 'table': 'docs', 'column': 'v'}
    assert engine.execute(index, 'ann') == {}
    with pytest.raises(errors.DatabaseError) as caught:
        engine.execute(index)
    assert caught.value.code == 'index_exists'
    for op in ('begin', 'commit', 'rollback'):
        assert engine.execute({'op': op}) == {}
    engine.close()


@pytest.mark.parametrize('statement', [
    {'op': 'unknown'}, {'op': 'begin', 'ignored': True},
    {'op': 'insert', 'table': 't', 'rows': [[lit('9223372036854775808')]]},
    {'op': 'insert', 'table': 't', 'rows': [[lit('-9223372036854775809')]]},
    {'op': 'insert', 'table': 't', 'rows': [[lit('1.0')]]},
    {'op': 'insert', 'table': 't', 'rows': [[lit('NaN', 'float')]]},
    {'op': 'insert', 'table': 't', 'rows': [[{'kind': 'integer', 'text': 123}]]},
    {'op': 'select', 'table': 't', 'projections': [{'expression': col()}], 'limit': True},
    {'op': 'select', 'table': 't', 'projections': [{'expression': col()}], 'limit': -1},
    {'op': 'select', 'table': 't', 'projections': [{'expression': col()}], 'limit': 2**64},
    {'op': 'select', 'table': 't', 'projections': [{'expression': {'kind': 'python', 'code': 'x'}}]},
    {'op': 'create_table', 'table': 't', 'columns': [{'name': 'id', 'type': 'bigint', 'primary_key': 1}]},
])
def test_malformed_ir_is_not_silently_accepted(tmp_path, statement):
    core, errors = modules()
    engine = core.Engine(str(tmp_path))
    with pytest.raises(errors.DatabaseError) as caught:
        engine.execute(statement)
    assert caught.value.code == 'invalid_ir'
    engine.close()
