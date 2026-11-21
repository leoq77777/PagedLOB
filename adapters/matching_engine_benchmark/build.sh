#!/usr/bin/env bash
set -euo pipefail

# Build `pagedlob_adapter.so` for https://github.com/flash1-dev/matching-engine-benchmark
# Usage:
#   MEB_DIR=/path/to/matching-engine-benchmark ./adapters/matching_engine_benchmark/build.sh [output.so]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MEB_DIR="${MEB_DIR:-/tmp/meb}"
OUT_SO="${1:-${MEB_DIR}/pagedlob_adapter.so}"
CXX_BIN="${CXX:-g++}"

if [[ ! -f "${MEB_DIR}/api/matching_engine_api.h" ]]; then
    echo "Error: matching_engine_api.h not found under MEB_DIR=${MEB_DIR}/api" >&2
    echo "Clone https://github.com/flash1-dev/matching-engine-benchmark and set MEB_DIR." >&2
    exit 1
fi

"${CXX_BIN}" -std=c++20 -O3 -DNDEBUG -march=native -flto -shared -fPIC \
    -Wall -Wextra -Werror \
    -I"${REPO_ROOT}/include" \
    -I"${MEB_DIR}/api" \
    "${SCRIPT_DIR}/pagedlob_adapter.cpp" \
    -o "${OUT_SO}"

echo "Built ${OUT_SO}"
