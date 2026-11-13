"""Strict, flushed JSONL transport with SQL-to-native-IR dispatch."""
import argparse
import json
import math
import sys

from _minivectordb import Engine
from .errors import DatabaseError
from .frontend import parse_statement


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f"nonfinite JSON number: {value}")


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("nonfinite JSON number")
    return result


def emit(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=True, allow_nan=False, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def execute_request(engine, request):
    if not isinstance(request.get("sql"), str):
        raise DatabaseError("invalid_request", "sql must be a string")
    if request.get("search_mode", "exact") not in ("exact", "ann"):
        raise DatabaseError("invalid_request", "invalid search_mode")
    sql = request["sql"]
    try:
        sql.encode("utf-8")
    except UnicodeError as error:
        raise DatabaseError("unsupported_sql", str(error)) from error
    mode = request.get("search_mode", "exact")
    fast_insert = getattr(engine, 'try_execute_insert', None)
    result = fast_insert(sql, mode) if fast_insert is not None else None
    if result is not None:
        return result
    return engine.execute(parse_statement(sql), mode)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default="/data")
    args = parser.parse_args()
    try:
        engine = Engine(args.data_dir)
    except DatabaseError as error:
        print(f"{error.code}: {error.message}", file=sys.stderr)
        return 1
    try:
        emit({"type": "ready"})
        for line in sys.stdin:
            ident = None
            try:
                try:
                    request = json.loads(line, object_pairs_hook=unique_object,
                                         parse_constant=reject_constant, parse_float=finite_float)
                except (ValueError, RecursionError) as error:
                    raise DatabaseError("invalid_request", str(error)) from error
                if not isinstance(request, dict):
                    raise DatabaseError("invalid_request", "request must be an object")
                ident = request.get("id")
                if "id" not in request:
                    raise DatabaseError("invalid_request", "missing request id")
                if request.get("op") == "shutdown":
                    engine.close()
                    emit({"id": ident, "ok": True})
                    return 0
                if request.get("op") != "execute":
                    raise DatabaseError("invalid_request", "unknown request op")
                payload = execute_request(engine, request)
                emit({"id": ident, "ok": True, **payload})
            except DatabaseError as error:
                if error.code == "fatal_storage_error":
                    print(f"{error.code}: {error.message}", file=sys.stderr)
                    return 1
                emit({"id": ident, "ok": False,
                      "error": {"code": error.code, "message": error.message}})
    finally:
        engine.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
