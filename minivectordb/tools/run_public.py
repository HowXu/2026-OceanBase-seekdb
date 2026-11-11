#!/usr/bin/env python3
"""Collect per-SQL diagnostics using the unchanged public transport and assertions."""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "protocol_check"))
from _transport import Session, strict_json  # noqa: E402
from _assertions import assert_response  # noqa: E402


def within(package, relative):
    path = (package / relative).resolve(strict=True)
    path.relative_to(package.resolve())
    return path


def prepare_case(case, package):
    """Bind assertions only to SQL, before starting any database process."""
    ident = case["id"]
    if not isinstance(ident, str) or not ident or ident in (".", "..") or Path(ident).name != ident:
        raise ValueError("case ID must be a single nonempty path component")
    expectations = strict_json(within(package, case["result"]).read_text())
    if not isinstance(expectations, list) or any(not isinstance(x, dict) for x in expectations):
        raise ValueError("expected results must be an array of objects")
    statements, events, mode = [], [], "exact"
    for line, text in enumerate(within(package, case["test"]).read_text().splitlines(), 1):
        sql = text.strip()
        if not sql or sql.startswith("#"):
            continue
        if sql == "--restart":
            events.append(None)
        elif sql in ("--mode exact", "--mode ann"):
            mode = sql.split()[1]
        elif sql.startswith("--"):
            raise ValueError(f"unsupported public directive: {sql}")
        else:
            index = len(statements)
            if index >= len(expectations):
                raise ValueError(f"{ident}: missing expected result at line {line}")
            row = dict(index=index, line=line, sql=sql, mode=mode,
                       expected=expectations[index], response=None, attempted=False,
                       transport_ok=False, status="NOT_RUN", category="not_run", message="")
            statements.append(row)
            events.append(row)
    if len(statements) != len(expectations):
        raise ValueError(f"{ident}: SQL/result count mismatch")
    return dict(id=ident, status="NOT_RUN", errors=[], statements=statements), events


def not_run(case, message):
    for row in case["statements"]:
        if not row["attempted"]:
            row["message"] = message


def run_case(case, events, config, command, output, keep_going):
    session, current, phase, aborted = None, None, "startup", None
    pending_reason = "not run after session failure"
    try:
        # A Session owns one fresh directory; start() on this same object reuses it.
        session = Session(config, output / (case["id"] + ".stderr.log"), command=command)
        session.start()
        for row in events:
            if row is None:
                phase = "restart"
                session.shutdown()
                session.start()
                continue
            phase, current = "execute", row
            row["attempted"] = True
            row["response"] = session.request(row["sql"], row["mode"])
            row["transport_ok"] = True
            try:
                assert_response(row["response"], row["expected"], config)
            except AssertionError as exc:
                row.update(status="FAIL", category="assertion", message=str(exc))
                if not keep_going:
                    current = None
                    pending_reason = "not run after assertion failure (use --keep-going)"
                    break
            else:
                row.update(status="PASS", category=None)
            current = None
        phase = "shutdown"
        session.shutdown()
    except (Exception, KeyboardInterrupt) as exc:
        message = str(exc) or type(exc).__name__
        case["errors"].append(dict(phase=phase, type=type(exc).__name__, message=message))
        if current is not None:
            current.update(status="FAIL", category="transport" if not current["transport_ok"] else "assertion_error",
                           message=message)
        if isinstance(exc, (TimeoutError, subprocess.TimeoutExpired)):
            aborted = "not run after first timeout"
        elif isinstance(exc, KeyboardInterrupt):
            aborted = "not run after interruption"
        pending_reason = aborted or f"not run after {phase} failure: {message}"
    finally:
        if session is not None:
            try:
                session.close()
            except (Exception, KeyboardInterrupt) as exc:
                case["errors"].append(dict(phase="cleanup", type=type(exc).__name__, message=str(exc)))
                aborted = "not run after cleanup failure"
                pending_reason = aborted
    not_run(case, pending_reason)
    case["status"] = "PASS" if not case["errors"] and all(
        row["status"] == "PASS" for row in case["statements"]) else "FAIL"
    return aborted


def statistics(cases):
    rows = [row for case in cases for row in case["statements"]]
    return dict(attempted=sum(row["attempted"] for row in rows),
                transport_ok=sum(row["transport_ok"] for row in rows),
                sql_passed=sum(row["status"] == "PASS" for row in rows),
                sql_failed=sum(row["status"] == "FAIL" for row in rows),
                not_run=sum(row["status"] == "NOT_RUN" for row in rows))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command", required=True,
                        help="trusted local JSON argv; {data_dir} placeholder supported")
    parser.add_argument("--case", action="append", help="case ID; repeat to select multiple cases")
    parser.add_argument("--package", type=Path, default=ROOT / "mysqltest")
    parser.add_argument("--config", type=Path, default=ROOT / "protocol_check" / "config.json")
    parser.add_argument("--output", required=True, type=Path, help="new diagnostic output directory")
    parser.add_argument("--keep-going", action="store_true", help="continue after SQL assertion failures")
    args = parser.parse_args(argv)
    try:
        command = strict_json(args.command)
        if not isinstance(command, list) or not command or any(not isinstance(x, str) for x in command):
            raise ValueError("--command must be a nonempty JSON array of strings")
        config = strict_json(args.config.read_text())
        cases = strict_json((args.package / "manifest.json").read_text())["cases"]
        ids = [case["id"] for case in cases]
        if len(set(ids)) != len(ids):
            raise ValueError("duplicate case ID")
        if args.case:
            unknown = set(args.case) - set(ids)
            if unknown:
                raise ValueError("unknown public case: " + ", ".join(sorted(unknown)))
            cases = [case for case in cases if case["id"] in args.case]
        if not cases:
            raise ValueError("no public cases selected")
        prepared = [prepare_case(case, args.package) for case in cases]
    except (ValueError, KeyError, TypeError, OSError) as exc:
        parser.error(str(exc))
    args.output.mkdir(parents=True, exist_ok=False)
    aborted, results = None, []
    for case, events in prepared:
        if aborted:
            not_run(case, aborted)
        else:
            aborted = run_case(case, events, config, command, args.output, args.keep_going)
        results.append(case)
        print(f"{case['id']}: {case['status']}", flush=True)
        (args.output / "cases.json").write_text(json.dumps(results, ensure_ascii=True, indent=2) + "\n")
    passed = all(case["status"] == "PASS" for case in results)
    report = dict(scope="public-sql-diagnostics", passed=passed, statistics=statistics(results),
                  cases=results, note="Local diagnostics only; transport success is not SQL success. "
                  "Unmapped expected errors retain the original permissive assertion semantics.")
    (args.output / "result.json").write_text(json.dumps(report, ensure_ascii=True, indent=2) + "\n")
    print(f"Report: {args.output / 'result.json'}")
    return 0 if passed else 1


if __name__ == "__main__":
    def terminate(signum, frame):
        raise KeyboardInterrupt("terminated")
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
