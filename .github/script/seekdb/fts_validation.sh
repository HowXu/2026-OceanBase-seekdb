#!/usr/bin/env bash
# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0.
# See LICENSE for the full license text.
# Run unchanged official FTS benchmarks sequentially on one dedicated CI runner.
set -euo pipefail

: "${RUNNER_TEMP:?Run on a dedicated GitHub Actions runner}"
: "${BASELINE_COMMIT:?Baseline revision is required}"
: "${CANDIDATE_COMMIT:?Candidate revision is required}"

ROOT=$(git rev-parse --show-toplevel)
WORK="$RUNNER_TEMP/fts-validation"
REPORTS="$WORK/reports"
mkdir -p "$REPORTS"
cd "$ROOT"
git rev-parse "$BASELINE_COMMIT^{commit}" "$CANDIDATE_COMMIT^{commit}" > "$REPORTS/revisions.txt"
uname -a > "$REPORTS/environment.txt"
lscpu >> "$REPORTS/environment.txt"
free -h >> "$REPORTS/environment.txt"
df -h . >> "$REPORTS/environment.txt"

# Reject benchmark drift rather than comparing runs with different workloads or scoring.
git diff --exit-code "$BASELINE_COMMIT" "$CANDIDATE_COMMIT" -- tools/benchmark/
cp "$RUNNER_TEMP/fts-unicode.sql" "$WORK/unicode.sql"
db_pid=""

stop_database() {
  if [[ -n "$db_pid" ]]; then
    kill -TERM "$db_pid" 2>/dev/null || true
    if ! timeout 30s tail --pid="$db_pid" -f /dev/null; then
      kill -KILL "$db_pid" 2>/dev/null || true
    fi
    wait "$db_pid" 2>/dev/null || true
    db_pid=""
  fi
}
trap stop_database EXIT

run_variant() {
  local label=$1 binary=$2 instance="$WORK/$1-instance"
  mkdir -p "$instance"
  "$binary" -V > "$REPORTS/$label-version.txt" 2>&1
  "$binary" --nodaemon --base-dir="$instance" --port=2883 \
    --parameter memory_budget=2G --parameter datafile_size=1G \
    --parameter datafile_maxsize=4G --parameter log_disk_size=2G \
    > "$REPORTS/$label-console.txt" 2>&1 &
  db_pid=$!
  local ready=0
  for ((attempt = 0; attempt < 180; ++attempt)); do
    if mysql --no-defaults -h127.0.0.1 -P2883 -uroot --connect-timeout=2 -e 'SELECT 1' >/dev/null 2>&1; then
      ready=1
      break
    fi
    if ! kill -0 "$db_pid" 2>/dev/null; then
      cat "$REPORTS/$label-console.txt"
      return 1
    fi
    sleep 1
  done
  if [[ "$ready" != 1 ]]; then
    echo "Database readiness timed out" >&2
    return 1
  fi

  mysql --no-defaults -h127.0.0.1 -P2883 -uroot -e 'CREATE DATABASE fts_unicode_regression'
  mysql --no-defaults -h127.0.0.1 -P2883 -uroot --batch --raw \
    --default-character-set=utf8mb4 fts_unicode_regression \
    < "$WORK/unicode.sql" > "$REPORTS/$label-unicode.tsv"

  MYSQL='mysql --no-defaults -h127.0.0.1 -P2883 -uroot -N -s -A --default-character-set=utf8mb4' \
  MYSQL_VERBOSE='mysql --no-defaults -h127.0.0.1 -P2883 -uroot -A --default-character-set=utf8mb4' \
  LABEL="$label" ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORTS/$label-report.txt" \
    bash tools/benchmark/fts_large_bench.sh 2>&1 | tee "$REPORTS/$label-benchmark-log.txt"
  python3 tools/benchmark/fts_large_bench_score.py "$REPORTS/$label-report.txt" \
    > "$REPORTS/$label-score.txt"
  stop_database
}

git switch --detach "$BASELINE_COMMIT"
./build.sh release --init 2>&1 | tee "$REPORTS/baseline-configure.txt"
cmake --build build_release --target seekdb -j2 2>&1 | tee "$REPORTS/baseline-build.txt"
cp build_release/src/observer/seekdb "$WORK/baseline-seekdb"
run_variant baseline "$WORK/baseline-seekdb"

git switch --detach "$CANDIDATE_COMMIT"
./build.sh release 2>&1 | tee "$REPORTS/candidate-configure.txt"
cmake --build build_release --target seekdb -j2 2>&1 | tee "$REPORTS/candidate-build.txt"
run_variant candidate "$ROOT/build_release/src/observer/seekdb"

diff -u "$REPORTS/baseline-unicode.tsv" "$REPORTS/candidate-unicode.tsv" \
  > "$REPORTS/unicode-diff.txt"
echo 'TOKENIZE outputs and MATCH document IDs match the baseline.' > "$REPORTS/correctness.txt"
echo 'These are same-runner development measurements; official competition scores require the official environment.' \
  > "$REPORTS/measurement-scope.txt"
