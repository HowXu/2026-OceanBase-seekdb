"""Independent miniature reports; never import public answers."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest

TOOL = Path(__file__).resolve().parents[1] / 'tools/check_delivery.py'


def validator():
    spec = importlib.util.spec_from_file_location('check_delivery', TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_delivery_report


def report():
    return {'scope': 'public-protocol-check', 'passed': True,
            'cases': [{'id': 'tiny_a', 'status': 'PASS', 'message': '', 'duration_ms': 1},
                      {'id': 'tiny_b', 'status': 'PASS', 'message': '', 'duration_ms': 2}],
            'note': 'independent fixture'}


def cli(tmp_path, value, manifest):
    source = tmp_path / 'report.json'
    config = tmp_path / 'manifest.json'
    source.write_text(value)
    config.write_text(manifest)
    before = source.read_bytes()
    result = subprocess.run([sys.executable, str(TOOL), '--report', str(source),
                             '--manifest', str(config)], capture_output=True, text=True)
    assert source.read_bytes() == before
    return result


def test_complete_report_accepted_without_rewriting(tmp_path):
    validate = validator()
    value = report()
    before = copy.deepcopy(value)
    assert validate(value, ['tiny_b', 'tiny_a']) is None
    assert value == before
    manifest = {'version': 'tiny', 'cases': [{'id': 'tiny_a'}, {'id': 'tiny_b'}]}
    assert cli(tmp_path, json.dumps(value), json.dumps(manifest)).returncode == 0


def test_incomplete_inconsistent_and_malformed_reports_rejected(tmp_path):
    validate = validator()
    bad = [None, [], {}, {'passed': True, 'cases': []}]
    for passed in (False, 1, 'true', None, [], {}):
        value = report()
        value['passed'] = passed
        bad.append(value)
    for status in ('FAIL', 'SKIPPED', 'BLOCKED', 'ERROR', 'TIMEOUT', '', True, None):
        value = report()
        value['cases'][0]['status'] = status
        bad.append(value)
    for cases in (None, {}, [None], [{'id': 'tiny_a'}], report()['cases'][:1],
                  report()['cases'] + [{'id': 'extra', 'status': 'PASS'}],
                  [report()['cases'][0]] * 2,
                  [{'id': [], 'status': 'PASS'}]):
        value = report()
        value['cases'] = cases
        bad.append(value)
    for value in bad:
        with pytest.raises(ValueError):
            validate(value, ['tiny_a', 'tiny_b'])
    for ids in (None, [], 'tiny_a', ['tiny_a', 'tiny_a'], [1], [''], [[]]):
        with pytest.raises(ValueError):
            validate(report(), ids)
    for manifest in ('null', '{}', '{"cases": []}', '{"cases": [null]}',
                     '{"cases": [{"id": "tiny_a"}, {"id": "tiny_a"}]}',
                     '{"cases": [{"id": true}]}', '{"cases": NaN}',
                     '{"cases": [{"id": "tiny_a"}, {"id": "tiny_b"}], "other": 1e999}'):
        assert cli(tmp_path, json.dumps(report()), manifest).returncode != 0
    manifest = '{"cases": [{"id": "tiny_a"}, {"id": "tiny_b"}]}'
    for value in ('{', '{"passed": true, "passed": false, "cases": []}',
                  '{"passed": true, "cases": [], "other": NaN}',
                  json.dumps(report())[:-1] + ', "other": 1e999}'):
        assert cli(tmp_path, value, manifest).returncode != 0
    for value in bad:
        assert cli(tmp_path, json.dumps(value), manifest).returncode != 0
