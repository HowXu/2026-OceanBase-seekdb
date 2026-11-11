"""Read-only acceptance gate for original protocol_check/run.py reports."""
import argparse
import json
import math
from pathlib import Path


def _case_ids(cases: object) -> list[str]:
    if not isinstance(cases, list) or not cases:
        raise ValueError('cases must be a nonempty list')
    ids = []
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError('each case must be an object')
        case_id = case.get('id')
        if not isinstance(case_id, str) or not case_id.strip():
            raise ValueError('case IDs must be nonempty strings')
        ids.append(case_id)
    if len(set(ids)) != len(ids):
        raise ValueError('case IDs must be unique')
    return ids


def validate_delivery_report(report: dict, expected_ids: list[str]) -> None:
    """Reject incomplete/inconsistent results without mutating the report."""
    if not isinstance(expected_ids, list):
        raise ValueError('expected IDs must be a list')
    expected = _case_ids([{'id': item} for item in expected_ids])
    if not isinstance(report, dict) or report.get('passed') is not True:
        raise ValueError('report passed must be the boolean true')
    actual = _case_ids(report.get('cases'))
    if set(actual) != set(expected):
        raise ValueError('report IDs must exactly match manifest IDs')
    if any(case.get('status') != 'PASS' for case in report['cases']):
        raise ValueError('every case status must be PASS')


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def _constant(value):
    raise ValueError(f'nonfinite JSON constant: {value}')


def _float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError('nonfinite JSON number')
    return value


def _load(path: Path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=_object,
                      parse_constant=_constant, parse_float=_float)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = _load(args.manifest)
        if not isinstance(manifest, dict):
            raise ValueError('manifest must be an object')
        expected = _case_ids(manifest.get('cases'))
        validate_delivery_report(_load(args.report), expected)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'delivery rejected: {exc}\n')
    print(f'delivery report accepted: {len(expected)} unique PASS cases')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
