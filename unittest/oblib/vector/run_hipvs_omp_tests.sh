#!/usr/bin/env bash
set -Eeuo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
FLAGS=(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
       -I"$ROOT/tools/hipvs_bridge" -Wall -Wextra -Werror)
"${CC:-cc}" -std=c11 "${FLAGS[@]}" "$ROOT/unittest/oblib/vector/test_hipvs_omp_config.c" \
  -Wl,--wrap=fopen -Wl,--wrap=sched_getaffinity -o "$OUT/test_omp_config"
check() {
  local quota=$1 affinity=$2 expected=$3
  shift 3
  env -u OMP_NUM_THREADS -u TEST_CPU_MAX -u TEST_AFFINITY_CPUS \
    TEST_CPU_MAX="$quota" TEST_AFFINITY_CPUS="$affinity" "$@" "$OUT/test_omp_config" "$expected"
}
check '1600000 100000' 128 16
check 'max 100000' 128 unset
check '150000 100000' 128 2
check '25000 100000' 128 1
check '1600000 100000' 4 unset
check 'invalid 100000' 128 unset
check '1600000 0' 128 unset
check '1600000 100000' 128 7 OMP_NUM_THREADS=7
check '1600000 100000' 128 '' OMP_NUM_THREADS=
env -u OMP_NUM_THREADS -u TEST_CPU_MAX -u TEST_AFFINITY_CPUS "$OUT/test_omp_config" unset
printf '#include "seekdb_omp_config.h"\n' | "${CXX:-c++}" -x c++ -std=c++17 "${FLAGS[@]}" -c -o "$OUT/omp-cxx.o" -
echo 'PASS 10 startup policy cases and C++ compatibility'
