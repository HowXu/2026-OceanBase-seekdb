#!/usr/bin/env bash
# Compare parser outputs against results captured before the FTS optimizations.
# Run only against a dedicated test instance; the SQL creates and drops test tables.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MYSQL="${MYSQL:-mysql --no-defaults -h127.0.0.1 -P2881 -uroot}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

$MYSQL -e 'CREATE DATABASE IF NOT EXISTS fts_parser_regression;'
for test in fts_beng_regression fts_ik_chain_regression fts_word_map_regression fts_build_sort_regression; do
  $MYSQL --default-character-set=utf8mb4 --batch --skip-column-names --raw \
    fts_parser_regression < "$SCRIPT_DIR/$test.sql" > "$TMP_DIR/$test.result"
  diff -u "$SCRIPT_DIR/$test.result" "$TMP_DIR/$test.result"
  echo "$test: PASS"
done
