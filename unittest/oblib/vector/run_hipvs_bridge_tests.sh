#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
TEST_DIR=$ROOT/unittest/oblib/vector
BRIDGE_DIR=$ROOT/tools/hipvs_bridge
FLAGS=(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
       -I"$TEST_DIR/hipvs_mock" -I"$BRIDGE_DIR")
"${CC:-cc}" -std=c11 "${FLAGS[@]}" -Wall -Wextra -Werror \
  -c "$BRIDGE_DIR/seekdb_cuvs_bridge.c" -o "$OUT/bridge.o"
"${CXX:-c++}" -std=c++17 "${FLAGS[@]}" "$TEST_DIR/test_hipvs_bridge.cpp" \
  "$OUT/bridge.o" -o "$OUT/test_bridge"
ASAN_OPTIONS=detect_leaks=1 "$OUT/test_bridge"
# hipcc may compile .c inputs as C++; also check this mode.
"${CXX:-c++}" -x c++ -std=c++17 "${FLAGS[@]}" \
  -c "$BRIDGE_DIR/seekdb_cuvs_bridge.c" -o "$OUT/bridge-cxx.o"
